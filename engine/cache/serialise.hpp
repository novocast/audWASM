#pragma once

// Bounds-checked binary serialization helpers for .awc chunk payloads.
// All read/write operations on numeric types enforce little-endian and detect out-of-bounds access.
//
// Design: these are defensive, minimal helpers — one-shot conversions, no state machines.
// Hand-written for simplicity and to avoid endianness and alignment surprises from generic
// libraries (see M16 decision).

#include <cstring>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "../util/result.hpp"

namespace aud::cache {

class PayloadWriter {
public:
    PayloadWriter() = default;

    // Serialize a u8.
    void writeU8(uint8_t value) {
        m_buffer.push_back(value);
    }

    // Serialize a u32, little-endian.
    void writeU32(uint32_t value) {
        m_buffer.push_back(static_cast<uint8_t>(value & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }

    // Serialize an i32, little-endian (two's complement).
    void writeI32(int32_t value) {
        writeU32(static_cast<uint32_t>(value));
    }

    // Serialize a u64, little-endian.
    void writeU64(uint64_t value) {
        m_buffer.push_back(static_cast<uint8_t>(value & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
        m_buffer.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    }

    // Serialize an i64, little-endian.
    void writeI64(int64_t value) {
        writeU64(static_cast<uint64_t>(value));
    }

    // Serialize a float (IEEE 754), little-endian.
    void writeFloat(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU32(bits);
    }

    // Serialize a double (IEEE 754), little-endian.
    void writeDouble(double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU64(bits);
    }

    // Serialize a span of floats.
    void writeFloatArray(std::span<const float> values) {
        for (float v : values) {
            writeFloat(v);
        }
    }

    // Serialize a span of doubles.
    void writeDoubleArray(std::span<const double> values) {
        for (double v : values) {
            writeDouble(v);
        }
    }

    // Serialize a string (UTF-8) with a u32 length prefix.
    void writeString(std::string_view str) {
        writeU32(static_cast<uint32_t>(str.size()));
        m_buffer.insert(m_buffer.end(), str.begin(), str.end());
    }

    // Serialize raw bytes.
    void writeBytes(std::span<const uint8_t> bytes) {
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
    }

    // Get the current payload.
    [[nodiscard]] const std::vector<uint8_t>& buffer() const { return m_buffer; }

    // Move ownership of the buffer.
    [[nodiscard]] std::vector<uint8_t> take() {
        return std::move(m_buffer);
    }

    // Current write position.
    [[nodiscard]] size_t position() const { return m_buffer.size(); }

private:
    std::vector<uint8_t> m_buffer;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const uint8_t> payload)
        : m_payload(payload), m_offset(0) {}

    // Read a u8, bounds-checked.
    Result<uint8_t> readU8() {
        if (m_offset + 1 > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (u8)");
        }
        return m_payload[m_offset++];
    }

    // Read a u16, little-endian, bounds-checked.
    Result<uint16_t> readU16() {
        if (m_offset + 2 > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (u16)");
        }
        uint16_t value = static_cast<uint16_t>(m_payload[m_offset]) |
                         (static_cast<uint16_t>(m_payload[m_offset + 1]) << 8);
        m_offset += 2;
        return value;
    }

    // Read a u32, little-endian, bounds-checked.
    Result<uint32_t> readU32() {
        if (m_offset + 4 > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (u32)");
        }
        uint32_t value = static_cast<uint32_t>(m_payload[m_offset]) |
                         (static_cast<uint32_t>(m_payload[m_offset + 1]) << 8) |
                         (static_cast<uint32_t>(m_payload[m_offset + 2]) << 16) |
                         (static_cast<uint32_t>(m_payload[m_offset + 3]) << 24);
        m_offset += 4;
        return value;
    }

    // Read an i32, little-endian.
    Result<int32_t> readI32() {
        auto u = readU32();
        if (!u.ok) return Err(u.error);
        return static_cast<int32_t>(u.value);
    }

    // Read a u64, little-endian, bounds-checked.
    Result<uint64_t> readU64() {
        if (m_offset + 8 > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (u64)");
        }
        uint64_t value = static_cast<uint64_t>(m_payload[m_offset]) |
                         (static_cast<uint64_t>(m_payload[m_offset + 1]) << 8) |
                         (static_cast<uint64_t>(m_payload[m_offset + 2]) << 16) |
                         (static_cast<uint64_t>(m_payload[m_offset + 3]) << 24) |
                         (static_cast<uint64_t>(m_payload[m_offset + 4]) << 32) |
                         (static_cast<uint64_t>(m_payload[m_offset + 5]) << 40) |
                         (static_cast<uint64_t>(m_payload[m_offset + 6]) << 48) |
                         (static_cast<uint64_t>(m_payload[m_offset + 7]) << 56);
        m_offset += 8;
        return value;
    }

    // Read an i64, little-endian.
    Result<int64_t> readI64() {
        auto u = readU64();
        if (!u.ok) return Err(u.error);
        return static_cast<int64_t>(u.value);
    }

    // Read a float (IEEE 754), little-endian.
    Result<float> readFloat() {
        auto bits = readU32();
        if (!bits.ok) return Err(bits.error);
        float value;
        std::memcpy(&value, &bits.value, sizeof(bits.value));
        return value;
    }

    // Read a double (IEEE 754), little-endian.
    Result<double> readDouble() {
        auto bits = readU64();
        if (!bits.ok) return Err(bits.error);
        double value;
        std::memcpy(&value, &bits.value, sizeof(bits.value));
        return value;
    }

    // Read a span of floats (count provided).
    Result<std::vector<float>> readFloatArray(size_t count) {
        std::vector<float> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto v = readFloat();
            if (!v.ok) return Err(v.error);
            values.push_back(v.value);
        }
        return values;
    }

    // Read a span of doubles (count provided).
    Result<std::vector<double>> readDoubleArray(size_t count) {
        std::vector<double> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto v = readDouble();
            if (!v.ok) return Err(v.error);
            values.push_back(v.value);
        }
        return values;
    }

    // Read a string with u32 length prefix.
    Result<std::string> readString() {
        auto len = readU32();
        if (!len.ok) return Err(len.error);

        if (m_offset + len.value > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (string)");
        }

        std::string result(reinterpret_cast<const char*>(m_payload.data() + m_offset), len.value);
        m_offset += len.value;
        return result;
    }

    // Read raw bytes (count provided).
    Result<std::vector<uint8_t>> readBytes(size_t count) {
        if (m_offset + count > m_payload.size()) {
            return Err("PayloadReader: out-of-bounds read (bytes)");
        }
        std::vector<uint8_t> result(m_payload.begin() + m_offset, m_payload.begin() + m_offset + count);
        m_offset += count;
        return result;
    }

    // Peek at remaining bytes without advancing.
    [[nodiscard]] std::span<const uint8_t> remaining() const {
        return m_payload.subspan(m_offset);
    }

    // Current read position.
    [[nodiscard]] size_t position() const { return m_offset; }

    // Seek to an absolute position.
    Result<void> seek(size_t offset) {
        if (offset > m_payload.size()) {
            return Err("PayloadReader: seek out-of-bounds");
        }
        m_offset = offset;
        return Ok();
    }

private:
    std::span<const uint8_t> m_payload;
    size_t                   m_offset;
};

}  // namespace aud::cache

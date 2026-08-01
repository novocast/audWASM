#pragma once

// Push -> pull adapter. The Decoder interface (decoder.hpp) is push-shaped: the host feeds bytes
// as they arrive from a ReadableStream. dr_wav/dr_flac/dr_mp3 are pull-shaped (they call back into
// a read callback whenever they want bytes). ByteRing is the buffer sitting between the two: feed()
// appends host-supplied bytes, and the decoder wrapper drives the underlying library's read
// callback from readInto()/skip(), which report "0 bytes available" rather than blocking when the
// ring has drained — the library then reports a short read, and the wrapper surfaces
// Result<std::size_t>{0} ("need more input") up through Decoder::read().
//
// ByteRing keeps the *entire* history of fed bytes (it never discards), because dr_libs occasionally
// seek backward while parsing headers/metadata. This is fine: encoded bytes are typically a small
// fraction of decoded PCM size, so buffering them in full does not threaten the AudioBuffer chunking
// budget (M00 §3), which is about decoded Sample storage.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace aud::decoder {

class ByteRing {
public:
    void feed(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        m_data.insert(m_data.end(), bytes, bytes + size);
    }

    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_data.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return m_cursor; }
    [[nodiscard]] std::size_t bytesAvailable() const noexcept { return m_data.size() - m_cursor; }

    // Copies up to `size` bytes from the current cursor into `out`, advancing the cursor. Returns
    // the number of bytes actually copied (may be less than `size`, including 0, if the ring has
    // drained — this is the "need more input" signal for the caller).
    std::size_t readInto(void* out, std::size_t size) {
        const std::size_t available = bytesAvailable();
        const std::size_t toCopy    = size < available ? size : available;
        if (toCopy > 0) {
            std::memcpy(out, m_data.data() + m_cursor, toCopy);
            m_cursor += toCopy;
        }
        return toCopy;
    }

    // Absolute seek within all bytes fed so far. Returns false if `position` is beyond what has
    // been fed (the caller should treat this as "need more input", not an error).
    bool seek(std::size_t position) noexcept {
        if (position > m_data.size()) {
            return false;
        }
        m_cursor = position;
        return true;
    }

    // Contiguous view of everything from the cursor to the end of what has been fed so far. Used
    // by stb_vorbis's pushdata API, which wants a raw pointer + length rather than a callback.
    [[nodiscard]] std::span<const std::byte> remaining() const noexcept {
        return std::span<const std::byte>(m_data.data() + m_cursor, m_data.size() - m_cursor);
    }

    void consume(std::size_t bytes) noexcept { m_cursor += bytes; }

    // Drops fully-consumed bytes from the front once the caller guarantees it will never seek
    // before the current cursor again (e.g. stb_vorbis, which is strictly forward-only). Bounded
    // callers (dr_wav/dr_flac/dr_mp3, which may seek backward while parsing headers) must not call
    // this.
    void compact() {
        if (m_cursor == 0) {
            return;
        }
        m_data.erase(m_data.begin(), m_data.begin() + static_cast<std::ptrdiff_t>(m_cursor));
        m_cursor = 0;
    }

private:
    std::vector<std::byte> m_data;
    std::size_t            m_cursor = 0;
};

}  // namespace aud::decoder

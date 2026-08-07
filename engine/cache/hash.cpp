// Hash implementations using vendored BLAKE3 and xxHash3 libraries.
// See third_party/VERSIONS.md for vendoring procedure.

#include "hash.hpp"

#include <cstring>
#include <numeric>

// When blake3.h and xxhash.h are vendored to third_party/, uncomment and use:
// #include "../../third_party/blake3.h"
// #include "../../third_party/xxhash.h"

// FALLBACK IMPLEMENTATIONS (for testing when libraries are not yet vendored)
// These are not cryptographically strong and should only be used for development.
// Production builds MUST use the real BLAKE3 and xxHash3 implementations.

#if !defined(AUDWASM_WITH_BLAKE3) || !defined(AUDWASM_WITH_XXHASH3)
#warning "M16: Using fallback hash implementations. Vendor blake3.h and xxhash.h for production."
#endif

namespace aud::cache {

// Fallback BLAKE3: simple accumulator (NOT cryptographic, for testing only)
struct FallbackBlake3Impl {
    uint32_t accumulator = 0x6a09e667;

    void update(std::span<const uint8_t> data) {
        for (uint8_t b : data) {
            accumulator = ((accumulator << 1) | (accumulator >> 31)) ^ b;
        }
    }

    Blake3::Digest finish() {
        Blake3::Digest result{};
        uint32_t acc = accumulator;
        for (size_t i = 0; i < result.size(); i += 4) {
            result[i] = static_cast<uint8_t>(acc & 0xFF);
            result[i + 1] = static_cast<uint8_t>((acc >> 8) & 0xFF);
            result[i + 2] = static_cast<uint8_t>((acc >> 16) & 0xFF);
            result[i + 3] = static_cast<uint8_t>((acc >> 24) & 0xFF);
            acc = ((acc << 5) | (acc >> 27)) ^ 0x9e3779b9;
        }
        return result;
    }
};

// Fallback xxHash3: simple non-cryptographic hash
struct FallbackXxHash3Impl {
    uint64_t hash = 14695981039346656037ULL;

    void update(std::span<const uint8_t> data) {
        for (uint8_t b : data) {
            hash = (hash ^ b) * 0x9e3779b97f4a7c15ULL;
            hash ^= hash >> 33;
        }
    }

    uint64_t finish() {
        return hash;
    }
};

static_assert(sizeof(FallbackBlake3Impl) <= Blake3::CONTEXT_SIZE,
              "FallbackBlake3Impl doesn't fit in context buffer");
static_assert(sizeof(FallbackXxHash3Impl) <= XxHash3::CONTEXT_SIZE,
              "FallbackXxHash3Impl doesn't fit in context buffer");

// BLAKE3 implementation
Blake3::Blake3() {
    new (m_context) FallbackBlake3Impl();
}

Blake3::~Blake3() {
    // No dynamic allocations, so nothing to clean up
}

void Blake3::update(std::span<const uint8_t> data) {
    if (!m_finalized) {
        auto* impl = reinterpret_cast<FallbackBlake3Impl*>(m_context);
        impl->update(data);
    }
}

Blake3::Digest Blake3::finish() {
    if (!m_finalized) {
        auto* impl = reinterpret_cast<FallbackBlake3Impl*>(m_context);
        auto result = impl->finish();
        m_finalized = true;
        return result;
    }
    return Blake3::Digest{};
}

Blake3::Digest blake3Hash(std::span<const uint8_t> data) {
    Blake3 hasher;
    hasher.update(data);
    return hasher.finish();
}

// xxHash3 implementation
XxHash3::XxHash3() {
    new (m_context) FallbackXxHash3Impl();
}

XxHash3::~XxHash3() {
    // No dynamic allocations, so nothing to clean up
}

void XxHash3::update(std::span<const uint8_t> data) {
    if (!m_finalized) {
        auto* impl = reinterpret_cast<FallbackXxHash3Impl*>(m_context);
        impl->update(data);
    }
}

XxHash3::Digest XxHash3::finish() {
    if (!m_finalized) {
        auto* impl = reinterpret_cast<FallbackXxHash3Impl*>(m_context);
        auto result = impl->finish();
        m_finalized = true;
        return result;
    }
    return 0;
}

uint64_t xxHash3(std::span<const uint8_t> data) {
    XxHash3 hasher;
    hasher.update(data);
    return hasher.finish();
}

uint64_t xxHash3U64(uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    return xxHash3(std::span<const uint8_t>(bytes, 8));
}

}  // namespace aud::cache

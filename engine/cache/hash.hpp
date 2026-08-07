#pragma once

// Hash function wrappers for .awc: BLAKE3 (for content addressing) and xxHash3 (for checksums).
// These are thin wrappers around the vendored single-header libraries.
//
// BLAKE3 is used for the source file hash (cache key) — cryptographic strength is needed to ensure
// that two different files don't collide in the cache.
//
// xxHash3 is used for chunk checksums (corruption detection) and parameter hashes — speed matters
// more than collision resistance here, and a non-cryptographic hash is fine.

#include <array>
#include <cstdint>
#include <span>

namespace aud::cache {

// BLAKE3 hash (256-bit / 32-byte digest).
class Blake3 {
public:
    static constexpr size_t DIGEST_SIZE = 32;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    Blake3();   // Initialize context
    ~Blake3();  // Clean up context if needed
    Blake3(const Blake3&) = delete;
    Blake3& operator=(const Blake3&) = delete;

    // Update the hash with a chunk of data.
    void update(std::span<const uint8_t> data);

    // Finalize and return the digest. Call this once; subsequent calls return the same value.
    [[nodiscard]] Digest finish();

private:
    // Opaque state for the BLAKE3 context.
    // With real BLAKE3, this would be a blake3_hasher struct (~200 bytes).
    // With fallback, this holds a FallbackBlake3Impl.
    static constexpr size_t CONTEXT_SIZE = 64;
    uint8_t                 m_context[CONTEXT_SIZE];
    bool                    m_finalized = false;
};

// Compute BLAKE3 digest of a buffer in one shot.
[[nodiscard]] Blake3::Digest blake3Hash(std::span<const uint8_t> data);

// xxHash3 (64-bit digest).
class XxHash3 {
public:
    static constexpr size_t DIGEST_SIZE = 8;
    using Digest = uint64_t;

    XxHash3();   // Initialize context
    ~XxHash3();  // Clean up context if needed
    XxHash3(const XxHash3&) = delete;
    XxHash3& operator=(const XxHash3&) = delete;

    // Update the hash with a chunk of data.
    void update(std::span<const uint8_t> data);

    // Finalize and return the digest.
    [[nodiscard]] Digest finish();

private:
    // Opaque state for the xxHash3 context.
    static constexpr size_t CONTEXT_SIZE = 64;
    uint8_t                 m_context[CONTEXT_SIZE];
    bool                    m_finalized = false;
};

// Compute xxHash3 digest of a buffer in one shot.
[[nodiscard]] uint64_t xxHash3(std::span<const uint8_t> data);

// Compute xxHash3 of a 64-bit integer (for parameter hashing).
[[nodiscard]] uint64_t xxHash3U64(uint64_t value);

}  // namespace aud::cache

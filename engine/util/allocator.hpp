#pragma once

// Allocation failure is an expected outcome (bad_alloc-equivalent without exceptions), not a
// terminating one. See M00 §2: aud::Allocator returns null; large/hot-path allocations go through
// tryAllocate() so callers get an aud::Error instead of a null they might forget to check.

#include <cstddef>
#include <new>

#include "result.hpp"

namespace aud {

// Raw allocate/free pair. Returns nullptr on failure — never throws, never aborts.
class Allocator {
public:
    virtual ~Allocator() = default;

    [[nodiscard]] virtual void* allocate(std::size_t bytes, std::size_t alignment) noexcept = 0;
    virtual void                deallocate(void* ptr, std::size_t bytes, std::size_t alignment) noexcept = 0;
};

// The default allocator: operator new(std::nothrow) / operator delete, aligned when needed.
Allocator& defaultAllocator() noexcept;

// Allocates `bytes` (aligned to `alignment`) via `allocator`, surfacing ErrorCode::OutOfMemory
// instead of a null pointer. Use this for any allocation whose size is influenced by untrusted
// input (file-reported frame counts, chunk growth, etc.).
Result<void*> tryAllocate(Allocator& allocator, std::size_t bytes,
                           std::size_t alignment = alignof(std::max_align_t));

}  // namespace aud

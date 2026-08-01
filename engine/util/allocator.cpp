#include "allocator.hpp"

namespace aud {

namespace {

class DefaultAllocator final : public Allocator {
public:
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment) noexcept override {
        return ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
    }

    void deallocate(void* ptr, std::size_t /*bytes*/, std::size_t alignment) noexcept override {
        ::operator delete(ptr, std::align_val_t{alignment});
    }
};

}  // namespace

Allocator& defaultAllocator() noexcept {
    static DefaultAllocator instance;
    return instance;
}

Result<void*> tryAllocate(Allocator& allocator, std::size_t bytes, std::size_t alignment) {
    void* ptr = allocator.allocate(bytes, alignment);
    if (ptr == nullptr) {
        return Error{ErrorCode::OutOfMemory, "util.allocator", "allocation failed"};
    }
    return ptr;
}

}  // namespace aud

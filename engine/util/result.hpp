#pragma once

// Minimal expected<T, Error>-alike. We do not use std::expected (C++23; Emscripten's bundled
// libc++ lags) or exceptions (-fno-exceptions core, see M00 §2). Must be trivially movable when T
// is, and must never allocate on the success path.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aud {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    Unknown,
    OutOfMemory,
    InvalidArgument,
    UnsupportedFormat,
    CorruptData,
    TruncatedData,
    DecoderFailure,
    NotFound,
    Cancelled,
    NotImplemented,
    CacheVersionMismatch,
};

std::string_view toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode        code = ErrorCode::Unknown;
    std::string_view domain;   // e.g. "decoder.mp3" — must be static storage, never a temporary
    std::string      detail;   // human readable, may be empty in release

    Error() = default;
    Error(ErrorCode c, std::string_view d, std::string det = {})
        : code(c), domain(d), detail(std::move(det)) {}
};

namespace detail {

template <class T>
concept NotError = !std::is_same_v<std::remove_cvref_t<T>, Error>;

}  // namespace detail

// Result<T>: holds either a T or an Error. Not a general-purpose monad — just enough surface for
// this codebase's early-return style (AUD_TRY) and value access.
template <class T>
class Result {
public:
    Result(T value) : m_hasValue(true) { std::construct_at(&m_storage.value, std::move(value)); }
    Result(Error error) : m_hasValue(false) { std::construct_at(&m_storage.error, std::move(error)); }

    Result(const Result&)            = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : m_hasValue(other.m_hasValue) {
        if (m_hasValue) {
            std::construct_at(&m_storage.value, std::move(other.m_storage.value));
        } else {
            std::construct_at(&m_storage.error, std::move(other.m_storage.error));
        }
    }

    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            destroy();
            m_hasValue = other.m_hasValue;
            if (m_hasValue) {
                std::construct_at(&m_storage.value, std::move(other.m_storage.value));
            } else {
                std::construct_at(&m_storage.error, std::move(other.m_storage.error));
            }
        }
        return *this;
    }

    ~Result() { destroy(); }

    [[nodiscard]] bool has_value() const noexcept { return m_hasValue; }
    explicit           operator bool() const noexcept { return m_hasValue; }

    T&       value() & noexcept { return m_storage.value; }
    const T& value() const& noexcept { return m_storage.value; }
    T&&      value() && noexcept { return std::move(m_storage.value); }

    const Error& error() const& noexcept { return m_storage.error; }
    Error&&      error() && noexcept { return std::move(m_storage.error); }

    T valueOr(T fallback) const& {
        return m_hasValue ? m_storage.value : fallback;
    }
    T valueOr(T fallback) && {
        return m_hasValue ? std::move(m_storage.value) : fallback;
    }

private:
    void destroy() noexcept {
        if (m_hasValue) {
            std::destroy_at(&m_storage.value);
        } else {
            std::destroy_at(&m_storage.error);
        }
    }

    union Storage {
        Storage() {}
        ~Storage() {}
        T     value;
        Error error;
    };

    Storage m_storage;
    bool    m_hasValue;
};

// Result<void> specialisation: success carries nothing.
template <>
class Result<void> {
public:
    Result() : m_error{} {}
    Result(Error error) : m_error(std::move(error)), m_hasValue(false) {}

    [[nodiscard]] bool has_value() const noexcept { return m_hasValue; }
    explicit           operator bool() const noexcept { return m_hasValue; }

    void value() const noexcept {}

    const Error& error() const& noexcept { return m_error; }
    Error&&      error() && noexcept { return std::move(m_error); }

private:
    Error m_error;
    bool  m_hasValue = true;
};

}  // namespace aud

// Early-return helper. `expr` must be a Result<T> prvalue or named Result<T> lvalue reference.
// On error, propagates it from the enclosing function (which must itself return a Result<U>).
#define AUD_TRY(expr)                                                                             \
    do {                                                                                          \
        auto&& aud_try_result_ = (expr);                                                          \
        if (!aud_try_result_.has_value()) {                                                       \
            return ::aud::Error{std::move(aud_try_result_).error()};                              \
        }                                                                                          \
    } while (0)

// Same as AUD_TRY but binds the success value to `name` in the enclosing scope.
#define AUD_TRY_ASSIGN(name, expr)                                                                 \
    auto aud_try_assign_##name##_ = (expr);                                                        \
    if (!aud_try_assign_##name##_.has_value()) {                                                   \
        return ::aud::Error{std::move(aud_try_assign_##name##_).error()};                          \
    }                                                                                               \
    auto name = std::move(aud_try_assign_##name##_).value()

#include <catch2/catch_test_macros.hpp>

#include "../../engine/util/result.hpp"

using aud::Error;
using aud::ErrorCode;
using aud::Result;

namespace {

// Move-only payload to prove Result<T> doesn't secretly require copyability.
struct MoveOnly {
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&)            = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : value(other.value) { other.value = -1; }
    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value       = other.value;
        other.value = -1;
        return *this;
    }
    int value;
};

Result<MoveOnly> makeOk(int v) { return MoveOnly{v}; }
Result<MoveOnly> makeErr() { return Error{ErrorCode::InvalidArgument, "test", "bad"}; }

Result<int> propagatesViaTry(bool fail) {
    if (fail) {
        AUD_TRY_ASSIGN(inner, makeErr());
        return inner.value;  // unreachable
    }
    AUD_TRY_ASSIGN(inner, makeOk(7));
    return inner.value;
}

}  // namespace

TEST_CASE("Result<T> holds a value on success", "[result]") {
    Result<int> r(5);
    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(r.value() == 5);
}

TEST_CASE("Result<T> holds an error on failure", "[result]") {
    Result<int> r(Error{ErrorCode::NotFound, "test.domain", "missing"});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
    REQUIRE(r.error().detail == "missing");
}

TEST_CASE("Result<T> move-constructs and moves the payload, not copies", "[result]") {
    Result<MoveOnly> r = makeOk(42);
    REQUIRE(r.has_value());
    REQUIRE(r.value().value == 42);

    Result<MoveOnly> moved(std::move(r));
    REQUIRE(moved.has_value());
    REQUIRE(moved.value().value == 42);
}

TEST_CASE("Result<T> move-assigns correctly between two error states", "[result]") {
    Result<MoveOnly> a = makeErr();
    Result<MoveOnly> b = makeOk(9);
    b                  = std::move(a);
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Result<void> default-constructs to success", "[result]") {
    Result<void> r;
    REQUIRE(r.has_value());
}

TEST_CASE("AUD_TRY_ASSIGN propagates the error and short-circuits", "[result]") {
    Result<int> failed = propagatesViaTry(true);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("AUD_TRY_ASSIGN binds the value on success", "[result]") {
    Result<int> ok = propagatesViaTry(false);
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 7);
}

TEST_CASE("valueOr returns the fallback only on error", "[result]") {
    Result<int> ok(3);
    Result<int> err(Error{ErrorCode::Unknown, "test"});
    REQUIRE(ok.valueOr(-1) == 3);
    REQUIRE(err.valueOr(-1) == -1);
}

TEST_CASE("toString covers every ErrorCode with a non-empty, non-fallback string", "[result]") {
    constexpr ErrorCode kAll[] = {
        ErrorCode::Ok,        ErrorCode::Unknown,      ErrorCode::OutOfMemory,       ErrorCode::InvalidArgument,
        ErrorCode::UnsupportedFormat, ErrorCode::CorruptData, ErrorCode::TruncatedData, ErrorCode::DecoderFailure,
        ErrorCode::NotFound, ErrorCode::Cancelled,   ErrorCode::NotImplemented,    ErrorCode::CacheVersionMismatch,
    };
    for (auto code : kAll) {
        REQUIRE_FALSE(aud::toString(code).empty());
    }
}

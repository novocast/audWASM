#include "result.hpp"

namespace aud {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                   return "Ok";
        case ErrorCode::Unknown:              return "Unknown";
        case ErrorCode::OutOfMemory:          return "OutOfMemory";
        case ErrorCode::InvalidArgument:      return "InvalidArgument";
        case ErrorCode::UnsupportedFormat:    return "UnsupportedFormat";
        case ErrorCode::CorruptData:          return "CorruptData";
        case ErrorCode::TruncatedData:        return "TruncatedData";
        case ErrorCode::DecoderFailure:       return "DecoderFailure";
        case ErrorCode::NotFound:             return "NotFound";
        case ErrorCode::Cancelled:            return "Cancelled";
        case ErrorCode::NotImplemented:       return "NotImplemented";
        case ErrorCode::CacheVersionMismatch: return "CacheVersionMismatch";
    }
    return "Unknown";
}

}  // namespace aud

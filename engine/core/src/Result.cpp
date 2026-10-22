#include "px/core/Result.hpp"

#include "px/core/Log.hpp"

#include <cstdlib>

namespace px {

const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::InvalidData: return "InvalidData";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::LimitExceeded: return "LimitExceeded";
        case ErrorCode::AccessDenied: return "AccessDenied";
    }
    return "Unknown";
}

namespace detail {

void badResultAccess(const Error* error) noexcept {
    if (error != nullptr) {
        log::write(log::Level::Error,
                   std::string("Fatal: accessed the value of a failed Result: ") + error->message);
    } else {
        log::write(log::Level::Error, "Fatal: accessed the error of a successful Result");
    }
    std::abort();
}

} // namespace detail
} // namespace px

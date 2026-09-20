// MeetMind — 错误码实现
#include "mm/common/result.h"

namespace mm {

const char* toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::FileNotFound: return "FileNotFound";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::FormatUnsupported: return "FormatUnsupported";
        case ErrorCode::ModelNotLoaded: return "ModelNotLoaded";
        case ErrorCode::DecodeFailed: return "DecodeFailed";
        case ErrorCode::InferenceFailed: return "InferenceFailed";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

}  // namespace mm

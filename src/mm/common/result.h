// MeetMind — 统一错误码与结果类型
// 设计约束：核心库不抛异常，所有可失败操作返回 Result<T>。
#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <utility>

namespace mm {

/// 错误码。区分「可恢复」（用户/调用方可修正）与「不可恢复」（本次任务终止）。
enum class ErrorCode {
    Ok = 0,
    InvalidArgument,    ///< 参数非法
    FileNotFound,       ///< 文件不存在
    IoError,            ///< 读写失败
    FormatUnsupported,  ///< 格式不支持
    ModelNotLoaded,     ///< 模型未加载
    DecodeFailed,       ///< 音频解码失败
    InferenceFailed,    ///< 推理失败
    Cancelled,          ///< 用户取消
    Internal,           ///< 内部错误
};

/// 错误码的可读名称。
const char* toString(ErrorCode code) noexcept;

/// 失败描述值；可隐式转换为任意 Result<T>，便于 `return Failure{...};`
struct Failure {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

inline Failure fail(ErrorCode code, std::string message) {
    return Failure{code, std::move(message)};
}

/// 结果类型：要么持有值，要么持有错误。
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}                  // NOLINT(google-explicit-constructor)
    Result(Failure f) : code_(f.code), message_(std::move(f.message)) {}  // NOLINT

    bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

    const T& value() const noexcept { return *value_; }
    T& value() noexcept { return *value_; }
    const T* operator->() const noexcept { return &*value_; }
    T* operator->() noexcept { return &*value_; }
    const T& operator*() const noexcept { return *value_; }
    T& operator*() noexcept { return *value_; }

    /// 失败时返回兜底值，成功时返回值本身。
    T valueOr(T fallback) const {
        return value_.has_value() ? *value_ : std::move(fallback);
    }

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
    std::optional<T> value_;
};

/// 无返回值的特化。
template <>
class Result<void> {
public:
    Result() = default;
    Result(Failure f) : code_(f.code), message_(std::move(f.message)) {}  // NOLINT

    bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

using Status = Result<void>;

/// 便捷构造函数（避免书写模板参数）。
template <typename T>
inline Result<T> success(T value) {
    return Result<T>(std::move(value));
}

inline Status okStatus() { return Status{}; }

/// 取消令牌：跨线程传递取消意图。
class CancelToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }
    void reset() noexcept { cancelled_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

}  // namespace mm

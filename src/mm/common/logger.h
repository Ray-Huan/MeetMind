// MeetMind — 分级日志
// 线程安全；支持控制台/文件/自定义回调三种输出目标的任意组合。
// GUI 通过 setSink() 把日志转发到界面，核心库不感知 UI。
#pragma once

#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace mm {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

const char* toString(LogLevel level) noexcept;
/// 解析 "trace"/"debug"/"info"/"warn"/"error"/"off"（大小写不敏感）。
LogLevel logLevelFromString(const std::string& s, LogLevel fallback = LogLevel::Info);

/// 日志回调签名：(级别, 已格式化整行)
using LogSink = std::function<void(LogLevel, const std::string&)>;

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) noexcept;
    LogLevel level() const noexcept;
    bool shouldLog(LogLevel level) const noexcept;

    /// 注册界面回调；传入空函数对象即注销。
    void setSink(LogSink sink);

    /// 打开文件输出；失败返回 false。
    bool openFile(const std::string& path);
    void closeFile();
    bool hasFile() const;

    /// 是否在日志中输出时间戳（默认开启）。
    void setTimestampEnabled(bool on) noexcept { timestamp_ = on; }

    void log(LogLevel level, const std::string& tag, const std::string& message);

private:
    Logger() = default;

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    LogSink sink_;
    std::ofstream file_;
    bool timestamp_ = true;
};

/// 流式日志对象；析构时落地，避免多次加锁。
class LogStream {
public:
    LogStream(LogLevel level, const char* tag) : level_(level), tag_(tag ? tag : "-") {}
    ~LogStream() {
        Logger::instance().log(level_, tag_, stream_.str());
    }

    template <typename T>
    LogStream& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

    /// 支持 std::endl 之类的操纵符
    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        stream_ << manip;
        return *this;
    }

private:
    LogLevel level_;
    std::string tag_;
    std::ostringstream stream_;
};

}  // namespace mm

// 用法： MM_LOG_INFO("pipeline") << "stage=asr segment=" << i << "/" << n;
#define MM_LOG(level, tag)                                                            \
    if (::mm::Logger::instance().shouldLog(::mm::LogLevel::level))                    \
    ::mm::LogStream(::mm::LogLevel::level, tag)

#define MM_LOG_TRACE(tag) MM_LOG(Trace, tag)
#define MM_LOG_DEBUG(tag) MM_LOG(Debug, tag)
#define MM_LOG_INFO(tag) MM_LOG(Info, tag)
#define MM_LOG_WARN(tag) MM_LOG(Warn, tag)
#define MM_LOG_ERROR(tag) MM_LOG(Error, tag)

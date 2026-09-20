#include "mm/common/logger.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace mm {

const char* toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF  ";
    }
    return "?????";
}

LogLevel logLevelFromString(const std::string& s, LogLevel fallback) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (v == "trace") return LogLevel::Trace;
    if (v == "debug") return LogLevel::Debug;
    if (v == "info") return LogLevel::Info;
    if (v == "warn" || v == "warning") return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    if (v == "off" || v == "none") return LogLevel::Off;
    return fallback;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

bool Logger::shouldLog(LogLevel level) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(level) >= static_cast<int>(level_) && level_ != LogLevel::Off;
}

void Logger::setSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

bool Logger::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) file_.close();
    file_.open(path, std::ios::out | std::ios::app);
    return file_.is_open();
}

void Logger::closeFile() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

bool Logger::hasFile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_.is_open();
}

namespace {
std::string currentTimestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}
}  // namespace

void Logger::log(LogLevel level, const std::string& tag, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_) || level_ == LogLevel::Off) return;

    std::string line;
    line.reserve(message.size() + 64);
    line.push_back('[');
    if (timestamp_) {
        line += currentTimestamp();
        line.push_back(']');
        line.push_back('[');
    }
    line += toString(level);
    line += "][";
    line += tag;
    line += "] ";
    line += message;

    if (file_.is_open()) {
        file_ << line << '\n';
        if (level >= LogLevel::Error) file_.flush();
    }
    // sink 在锁内调用：保证输出顺序与日志顺序一致；sink 实现须自行避免重入。
    if (sink_) {
        sink_(level, line);
    } else if (!file_.is_open()) {
        std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
        std::fputs(line.c_str(), out);
        std::fputc('\n', out);
    }
}

}  // namespace mm

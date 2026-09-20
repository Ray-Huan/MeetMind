// MeetMind — 时间格式化与日期推算工具
#pragma once

#include <cstdint>
#include <string>

namespace mm::timeutil {

/// 毫秒 → "HH:MM:SS"
std::string formatDuration(int64_t ms);
/// 毫秒 → "HH:MM:SS,mmm"（SRT 规范）
std::string formatSrt(int64_t ms);
/// 毫秒 → "MM:SS"（界面显示友好）
std::string formatShort(int64_t ms);
/// 毫秒 → "HH:MM:SS.mmm"
std::string formatClock(int64_t ms);

/// 公历日期（不含时区语义）
struct Date {
    int year = 1970;
    int month = 1;   ///< 1..12
    int day = 1;     ///< 1..31
    bool valid = false;
};

/// 解析 "YYYY-MM-DD" / "YYYY/MM/DD"。
Date parseYmd(const std::string& text);
/// 当前本地日期。
Date today();
/// 序列化为 "YYYY-MM-DD"。
std::string formatYmd(const Date& d);
/// 日期加减天数。
Date addDays(const Date& d, int days);
/// 星期一 = 1 … 星期日 = 7。
int weekdayOf(const Date& d);
/// 星期中文名（"星期一"）。
std::string weekdayNameCn(int weekday);
/// 两个日期相差天数（to - from）。
int daysBetween(const Date& from, const Date& to);

/// 当前本地时间戳 "YYYY-MM-DD HH:MM:SS"。
std::string nowDateTime();
/// 当前本地时间戳（ISO 8601，带时区偏移）。
std::string nowIso8601();
/// 单调时钟毫秒（用于性能计时）。
int64_t steadyNowMs();

/// 时长（毫秒）→ 中文可读描述，如 "1 小时 23 分钟"。
std::string humanizeDuration(int64_t ms);

}  // namespace mm::timeutil

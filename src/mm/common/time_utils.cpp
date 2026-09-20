#include "mm/common/time_utils.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "mm/common/string_utils.h"

namespace mm::timeutil {
namespace {

/// 霍华德·欣南特公历算法：days_from_civil / civil_from_days
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int>(yy + (m <= 2));
}

std::tm localTm() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

}  // namespace

std::string formatDuration(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    const int64_t h = totalSec / 3600;
    const int64_t m = (totalSec % 3600) / 60;
    const int64_t s = totalSec % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                  static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s));
    return std::string(buf);
}

std::string formatSrt(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    const int64_t h = totalSec / 3600;
    const int64_t m = (totalSec % 3600) / 60;
    const int64_t s = totalSec % 60;
    const int64_t milli = ms % 1000;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s), static_cast<long long>(milli));
    return std::string(buf);
}

std::string formatShort(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                  static_cast<long long>(totalSec / 60),
                  static_cast<long long>(totalSec % 60));
    return std::string(buf);
}

std::string formatClock(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(totalSec / 3600),
                  static_cast<long long>((totalSec % 3600) / 60),
                  static_cast<long long>(totalSec % 60),
                  static_cast<long long>(ms % 1000));
    return std::string(buf);
}

Date parseYmd(const std::string& text) {
    Date d;
    const std::string s = str::trim(text);
    if (s.size() < 8) return d;
    int parts[3] = {0, 0, 0};
    int idx = 0;
    int acc = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            acc = acc * 10 + (c - '0');
            any = true;
        } else if (c == '-' || c == '/' || c == '.') {
            if (!any || idx >= 3) return d;
            parts[idx++] = acc;
            acc = 0;
            any = false;
            if (idx == 3) break;
        } else {
            break;
        }
    }
    if (idx < 3) {
        if (!any || idx != 2) return d;
        parts[2] = acc;
    }
    if (parts[1] < 1 || parts[1] > 12 || parts[2] < 1 || parts[2] > 31) return d;
    if (parts[0] < 1900 || parts[0] > 2200) return d;
    d.year = parts[0];
    d.month = parts[1];
    d.day = parts[2];
    d.valid = true;
    return d;
}

Date today() {
    const std::tm tm = localTm();
    Date d;
    d.year = tm.tm_year + 1900;
    d.month = tm.tm_mon + 1;
    d.day = tm.tm_mday;
    d.valid = true;
    return d;
}

std::string formatYmd(const Date& d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.year, d.month, d.day);
    return std::string(buf);
}

Date addDays(const Date& d, int days) {
    if (!d.valid) return d;
    const int64_t z = daysFromCivil(d.year, static_cast<unsigned>(d.month),
                                    static_cast<unsigned>(d.day)) + days;
    Date out;
    unsigned m = 1, day = 1;
    int y = 1970;
    civilFromDays(z, y, m, day);
    out.year = y;
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(day);
    out.valid = true;
    return out;
}

int weekdayOf(const Date& d) {
    if (!d.valid) return 1;
    const int64_t z = daysFromCivil(d.year, static_cast<unsigned>(d.month),
                                    static_cast<unsigned>(d.day));
    // 1970-01-01 为星期四(4)。ISO: 星期一 = 1
    int wd = static_cast<int>((z + 3) % 7);  // 0=Monday
    if (wd < 0) wd += 7;
    return wd + 1;
}

std::string weekdayNameCn(int weekday) {
    static const char* kNames[] = {"", "星期一", "星期二", "星期三", "星期四",
                                   "星期五", "星期六", "星期日"};
    if (weekday < 1 || weekday > 7) return "";
    return kNames[weekday];
}

int daysBetween(const Date& from, const Date& to) {
    if (!from.valid || !to.valid) return 0;
    const int64_t a = daysFromCivil(from.year, static_cast<unsigned>(from.month),
                                    static_cast<unsigned>(from.day));
    const int64_t b = daysFromCivil(to.year, static_cast<unsigned>(to.month),
                                    static_cast<unsigned>(to.day));
    return static_cast<int>(b - a);
}

std::string nowDateTime() {
    const std::tm tm = localTm();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

std::string nowIso8601() {
    const std::tm tm = localTm();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

int64_t steadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string humanizeDuration(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    const int64_t h = totalSec / 3600;
    const int64_t m = (totalSec % 3600) / 60;
    const int64_t s = totalSec % 60;
    std::string out;
    if (h > 0) out += std::to_string(h) + " 小时 ";
    if (m > 0) out += std::to_string(m) + " 分钟 ";
    if (h == 0 && s > 0) out += std::to_string(s) + " 秒";
    if (out.empty()) out = "0 秒";
    return str::trim(out);
}

}  // namespace mm::timeutil

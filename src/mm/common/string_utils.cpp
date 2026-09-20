#include "mm/common/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace mm::str {
namespace {

inline bool asciiSpaceByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// 解析一个 UTF-8 码点，返回消耗的字节数（1..4）；非法序列返回 1 并给出 U+FFFD。
size_t decodeOne(const char* p, size_t remain, char32_t& out) {
    const unsigned char c0 = static_cast<unsigned char>(p[0]);
    if (c0 < 0x80) {
        out = c0;
        return 1;
    }
    auto cont = [&](size_t i) {
        return i < remain && (static_cast<unsigned char>(p[i]) & 0xC0) == 0x80;
    };
    if ((c0 & 0xE0) == 0xC0 && cont(1)) {
        out = static_cast<char32_t>(((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(p[1]) & 0x3Fu));
        if (out < 0x80) { out = 0xFFFD; return 1; }
        return 2;
    }
    if ((c0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        out = static_cast<char32_t>(((c0 & 0x0Fu) << 12) |
                                    ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 6) |
                                    (static_cast<unsigned char>(p[2]) & 0x3Fu));
        if (out < 0x800) { out = 0xFFFD; return 1; }
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        out = static_cast<char32_t>(((c0 & 0x07u) << 18) |
                                    ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 12) |
                                    ((static_cast<unsigned char>(p[2]) & 0x3Fu) << 6) |
                                    (static_cast<unsigned char>(p[3]) & 0x3Fu));
        if (out < 0x10000 || out > 0x10FFFF) { out = 0xFFFD; return 1; }
        return 4;
    }
    out = 0xFFFD;
    return 1;
}

void encodeOne(char32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::string trimLeft(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && asciiSpaceByte(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string trimRight(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && asciiSpaceByte(static_cast<unsigned char>(s[n - 1]))) --n;
    return s.substr(0, n);
}

std::string trim(const std::string& s) { return trimLeft(trimRight(s)); }

std::string toLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u + 32);
    }
    return r;
}

std::string toUpperAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'a' && u <= 'z') c = static_cast<char>(u - 32);
    }
    return r;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

bool contains(const std::string& s, const std::string& needle) {
    return needle.empty() || s.find(needle) != std::string::npos;
}

bool equalsIgnoreCaseAscii(const std::string& a, const std::string& b) {
    return toLowerAscii(a) == toLowerAscii(b);
}

std::string replaceAll(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (true) {
        const size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
    return out;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t hit = s.find(delim, start);
        if (hit == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, hit - start));
        start = hit + 1;
    }
    return out;
}

std::vector<std::string> splitAny(const std::string& s, const std::string& delims) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && delims.find(s[i]) != std::string::npos) ++i;
        const size_t start = i;
        while (i < s.size() && delims.find(s[i]) == std::string::npos) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t hit = s.find('\n', start);
        const size_t end = (hit == std::string::npos) ? s.size() : hit;
        std::string line = trim(s.substr(start, end - start));
        if (!line.empty()) out.push_back(std::move(line));
        if (hit == std::string::npos) break;
        start = hit + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string repeat(const std::string& unit, int times) {
    std::string out;
    if (times <= 0) return out;
    out.reserve(unit.size() * static_cast<size_t>(times));
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

std::string padLeft(const std::string& s, size_t width, char fill) {
    if (s.size() >= width) return s;
    return std::string(width - s.size(), fill) + s;
}

std::u32string toUtf32(const std::string& utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = 0;
        i += decodeOne(utf8.data() + i, utf8.size() - i, cp);
        out.push_back(cp);
    }
    return out;
}

std::string toUtf8(const std::u32string& u32) {
    std::string out;
    out.reserve(u32.size() * 3);
    for (char32_t cp : u32) encodeOne(cp, out);
    return out;
}

size_t utf8Length(const std::string& utf8) { return toUtf32(utf8).size(); }

std::string utf8Substr(const std::string& utf8, size_t startCp, size_t countCp) {
    const std::u32string u32 = toUtf32(utf8);
    if (startCp >= u32.size()) return {};
    const size_t n = std::min(countCp, u32.size() - startCp);
    return toUtf8(u32.substr(startCp, n));
}

bool isCjk(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一汉字
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // 扩展 A
           (cp >= 0xF900 && cp <= 0xFAFF);     // 兼容汉字
}

bool isCjkPunct(char32_t cp) {
    switch (cp) {
        case U'。': case U'，': case U'、': case U'；': case U'：': case U'？': case U'！':
        case U'“': case U'”': case U'‘': case U'’': case U'（': case U'）': case U'《':
        case U'》': case U'〈': case U'〉': case U'「': case U'」': case U'『': case U'』':
        case U'【': case U'】': case U'〔': case U'〕': case U'—': case U'…': case U'～':
        case U'·': case U'　': case U'％': case U'＃': case U'＆': case U'￥':
            return true;
        default:
            return false;
    }
}

bool isSentenceEnd(char32_t cp) {
    return cp == U'。' || cp == U'！' || cp == U'？' || cp == U'!' || cp == U'?' ||
           cp == U'.' || cp == U'；' || cp == U';' || cp == U'…';
}

bool isAsciiDigit(char32_t cp) { return cp >= U'0' && cp <= U'9'; }
bool isAsciiAlpha(char32_t cp) {
    return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z');
}
bool isAsciiAlnum(char32_t cp) { return isAsciiAlpha(cp) || isAsciiDigit(cp); }
bool isAsciiSpace(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == U'\v' || cp == U'\f';
}

std::string fullWidthToHalfWidth(const std::string& s) {
    const std::u32string u32 = toUtf32(s);
    std::u32string out;
    out.reserve(u32.size());
    for (char32_t cp : u32) {
        if (cp == 0x3000) {
            out.push_back(U' ');
        } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
            out.push_back(cp - 0xFEE0);
        } else {
            out.push_back(cp);
        }
    }
    return toUtf8(out);
}

bool isContentToken(const std::string& token) {
    if (token.empty()) return false;
    const std::u32string u32 = toUtf32(token);
    bool hasContent = false;
    for (char32_t cp : u32) {
        if (isCjk(cp) || isAsciiAlnum(cp)) {
            hasContent = true;
        } else if (isCjkPunct(cp)) {
            return false;
        }
    }
    if (!hasContent) return false;
    // 纯数字不算内容词
    bool allDigit = true;
    for (char32_t cp : u32) {
        if (!(isAsciiDigit(cp) || cp == U'.')) { allDigit = false; break; }
    }
    return !allDigit;
}

std::string sanitizeFileName(const std::string& name, const std::string& fallback) {
    static const std::set<std::string> kReserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || u < 0x20) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.')) out.pop_back();
    out = trim(out);
    // 去除 ".", ".." 与隐藏前缀
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    if (out.empty()) return fallback;

    // 保留名检查（不区分大小写，按主名判断）
    std::string stem = out;
    const size_t dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    if (kReserved.count(toUpperAscii(stem)) > 0) return fallback + "_" + out;
    return out;
}

std::string numberToString(double value, int maxDecimals) {
    if (!std::isfinite(value)) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", maxDecimals, value);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace mm::str

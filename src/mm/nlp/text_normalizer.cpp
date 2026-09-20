#include "mm/nlp/text_normalizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "mm/common/string_utils.h"

namespace mm::nlp {
namespace {

/// 中文数词字符集
bool isCnDigit(char32_t c) {
    switch (c) {
        case U'零': case U'〇': case U'一': case U'二': case U'两': case U'三':
        case U'四': case U'五': case U'六': case U'七': case U'八': case U'九':
            return true;
        default:
            return false;
    }
}

int cnDigitValue(char32_t c) {
    switch (c) {
        case U'零': case U'〇': return 0;
        case U'一': return 1;
        case U'二': case U'两': return 2;
        case U'三': return 3;
        case U'四': return 4;
        case U'五': return 5;
        case U'六': return 6;
        case U'七': return 7;
        case U'八': return 8;
        case U'九': return 9;
        default: return -1;
    }
}

bool isCnUnit(char32_t c) {
    return c == U'十' || c == U'百' || c == U'千' || c == U'万' || c == U'亿';
}

/// 日期/时间量词：单个中文数字后紧跟这些字时按数字处理（九月 → 9月）。
/// 刻意不包含「点」「个」等易产生歧义的量词。
bool isDateUnitChar(char32_t c) {
    switch (c) {
        case U'月': case U'日': case U'号': case U'年': case U'时':
        case U'分': case U'秒': case U'周': case U'季': case U'天':
            return true;
        default:
            return false;
    }
}

long long cnUnitValue(char32_t c) {
    switch (c) {
        case U'十': return 10;
        case U'百': return 100;
        case U'千': return 1000;
        case U'万': return 10000;
        case U'亿': return 100000000;
        default: return 0;
    }
}

std::string formatInteger(long long v) { return std::to_string(v); }

std::string formatDouble(double v) {
    if (std::fabs(v - std::llround(v)) < 1e-9) return std::to_string(std::llround(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/// 解析「纯数字读法」序列：二零二六 → 2026
bool concatDigits(const std::u32string& digits, long long* out) {
    long long v = 0;
    for (char32_t c : digits) {
        const int d = cnDigitValue(c);
        if (d < 0) return false;
        v = v * 10 + d;
        if (v > 9999999999999LL) return false;
    }
    *out = v;
    return true;
}

/// 解析含单位的数词：三百零五 → 305，三千万 → 30000000
bool parseWithUnits(const std::u32string& s, long long* out) {
    long long result = 0;
    long long section = 0;
    long long number = 0;
    bool sawAnything = false;
    for (char32_t c : s) {
        if (isCnDigit(c)) {
            number = cnDigitValue(c);
            sawAnything = true;
            continue;
        }
        if (!isCnUnit(c)) return false;
        const long long unit = cnUnitValue(c);
        sawAnything = true;
        if (unit <= 1000) {
            if (number == 0 && unit == 10) number = 1;  // 「十五」= 15
            section += number * unit;
            number = 0;
        } else {
            section = (section + number) * unit;
            result += section;
            section = 0;
            number = 0;
        }
    }
    if (!sawAnything) return false;
    *out = result + section + number;
    return true;
}

/// 截断过大的乘积累积
bool safeGuard(long long v) { return v >= 0 && v <= 9999999999999LL; }

}  // namespace

TextNormalizer::TextNormalizer(ItnOptions options) : options_(options) {}

bool TextNormalizer::parseChineseInteger(const std::string& text, long long* out) {
    if (!out) return false;
    const std::u32string s = str::toUtf32(str::trim(text));
    if (s.empty()) return false;

    bool allDigits = true;
    for (char32_t c : s) {
        if (!isCnDigit(c)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) return concatDigits(s, out);

    long long v = 0;
    if (!parseWithUnits(s, &v)) return false;
    if (!safeGuard(v)) return false;
    *out = v;
    return true;
}

bool TextNormalizer::parseChineseNumber(const std::string& text, double* out) {
    if (!out) return false;
    const std::string t = str::trim(text);
    if (t.empty()) return false;

    const std::u32string u = str::toUtf32(t);
    const size_t dot = u.find(U'点');
    if (dot == std::u32string::npos) {
        long long v = 0;
        if (!parseChineseInteger(t, &v)) return false;
        *out = static_cast<double>(v);
        return true;
    }

    const std::string intPart = str::toUtf8(u.substr(0, dot));
    long long iv = 0;
    if (intPart.empty()) {
        iv = 0;
    } else if (!parseChineseInteger(intPart, &iv)) {
        return false;
    }

    const std::u32string frac = u.substr(dot + 1);
    if (frac.empty()) return false;
    double f = 0.0;
    double scale = 0.1;
    for (char32_t c : frac) {
        const int d = cnDigitValue(c);
        if (d < 0) return false;
        f += static_cast<double>(d) * scale;
        scale *= 0.1;
    }
    *out = static_cast<double>(iv) + f;
    return true;
}

std::string TextNormalizer::normalize(const std::string& text) const {
    std::string s = text;
    if (options_.normalizeFullWidth) {
        s = str::fullWidthToHalfWidth(s);
    }

    std::u32string u = str::toUtf32(s);
    std::u32string out;
    out.reserve(u.size());

    size_t i = 0;
    while (i < u.size()) {
        // ---- 百分之 X ----
        if (options_.convertPercent && i + 3 < u.size() && u[i] == U'百' &&
            u[i + 1] == U'分' && u[i + 2] == U'之') {
            size_t j = i + 3;
            while (j < u.size() && (isCnDigit(u[j]) || isCnUnit(u[j]) || u[j] == U'点')) ++j;
            const std::string numText = str::toUtf8(u.substr(i + 3, j - (i + 3)));
            double v = 0.0;
            if (!numText.empty() && parseChineseNumber(numText, &v)) {
                out += str::toUtf32(formatDouble(v) + "%");
                i = j;
                continue;
            }
        }

        if (options_.convertNumbers && (isCnDigit(u[i]) || isCnUnit(u[i]))) {
            // 数词串
            size_t j = i;
            bool hasUnit = false;
            bool hasCnDigit = false;
            while (j < u.size() && (isCnDigit(u[j]) || isCnUnit(u[j]))) {
                if (isCnUnit(u[j])) hasUnit = true;
                if (isCnDigit(u[j])) hasCnDigit = true;
                ++j;
            }
            // 纯单位词（如「100 万」中的「万」）不构成数词，避免破坏已有阿拉伯数字
            if (!hasCnDigit) {
                out.push_back(u[i]);
                ++i;
                continue;
            }

            // 小数模式：数词串 '点' 数字串
            if (j + 1 < u.size() && u[j] == U'点' && isCnDigit(u[j + 1])) {
                size_t k = j + 1;
                while (k < u.size() && isCnDigit(u[k])) ++k;
                const std::string txt = str::toUtf8(u.substr(i, k - i));
                double v = 0.0;
                if (parseChineseNumber(txt, &v)) {
                    out += str::toUtf32(formatDouble(v));
                    i = k;
                    continue;
                }
            }

            const size_t runLen = j - i;
            const std::u32string seq = u.substr(i, runLen);
            long long iv = 0;

            // (1) 含单位 → 按中文数词语法解析
            if (hasUnit) {
                long long v = 0;
                if (parseWithUnits(seq, &v) && safeGuard(v)) {
                    out += str::toUtf32(formatInteger(v));
                    i = j;
                    continue;
                }
            }

            // (2) 纯数字且长度足够 → 按逐位拼接（二零二六 → 2026）
            if (!hasUnit && runLen >= 2) {
                const bool yearLike =
                    (j < u.size() && u[j] == U'年') ||
                    static_cast<int>(runLen) >= options_.minDigitRunForYear;
                if (yearLike && concatDigits(seq, &iv)) {
                    out += str::toUtf32(formatInteger(iv));
                    i = j;
                    continue;
                }
            }

            // (3) 单个数字 + 日期/时间量词（九月 → 9月），保守处理以避免误伤口语
            if (!hasUnit && runLen == 1 && j < u.size() && isDateUnitChar(u[j])) {
                const long long single = cnDigitValue(u[i]);
                if (single >= 0) {
                    out += str::toUtf32(formatInteger(single));
                    i = j;
                    continue;
                }
            }
        }

        out.push_back(u[i]);
        ++i;
    }

    std::string result = str::toUtf8(out);
    if (options_.collapseSpaces) {
        std::string collapsed;
        collapsed.reserve(result.size());
        bool prevSpace = false;
        for (char c : result) {
            const bool isSpace = (c == ' ' || c == '\t');
            if (isSpace && prevSpace) continue;
            collapsed.push_back(c);
            prevSpace = isSpace;
        }
        result = str::trim(collapsed);
    }
    return result;
}

}  // namespace mm::nlp

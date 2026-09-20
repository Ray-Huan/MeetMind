// MeetMind — 字符串与 UTF-8 工具
// 约定：所有接口以 std::string 承载 UTF-8 字节序列；需要按「字符」操作时先转换为 std::u32string。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm::str {

// ---- 基础 ----
std::string trim(const std::string& s);
std::string trimLeft(const std::string& s);
std::string trimRight(const std::string& s);
std::string toLowerAscii(const std::string& s);
std::string toUpperAscii(const std::string& s);

bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool contains(const std::string& s, const std::string& needle);
bool equalsIgnoreCaseAscii(const std::string& a, const std::string& b);

std::string replaceAll(const std::string& s, const std::string& from, const std::string& to);

/// 按单字符分隔符切分，保留空字段。
std::vector<std::string> split(const std::string& s, char delim);
/// 按任意分隔字符集合切分，丢弃空字段。
std::vector<std::string> splitAny(const std::string& s, const std::string& delims);
/// 按行切分，自动去除 \r 与行尾空白，跳过空行。
std::vector<std::string> splitLines(const std::string& s);
std::string join(const std::vector<std::string>& parts, const std::string& sep);

std::string repeat(const std::string& unit, int times);
std::string padLeft(const std::string& s, size_t width, char fill = '0');

// ---- UTF-8 ----
/// UTF-8 → UTF-32 码点序列；非法字节按 U+FFFD 替换，不抛异常。
std::u32string toUtf32(const std::string& utf8);
/// UTF-32 → UTF-8。
std::string toUtf8(const std::u32string& u32);
/// 码点数量（非字节数）。
size_t utf8Length(const std::string& utf8);
/// 按码点取子串。
std::string utf8Substr(const std::string& utf8, size_t startCp, size_t countCp);

/// CJK 统一汉字（含扩展 A）。
bool isCjk(char32_t cp);
/// 中日韩标点与全角符号（。，、；：？！「」『』（）《》…—～·　等）。
bool isCjkPunct(char32_t cp);
/// 句末终止标点（。！？!?；;…）
bool isSentenceEnd(char32_t cp);
bool isAsciiDigit(char32_t cp);
bool isAsciiAlpha(char32_t cp);
bool isAsciiAlnum(char32_t cp);
bool isAsciiSpace(char32_t cp);

/// 全角 → 半角（ASIIC 可见范围与空格）；非全角字符原样返回。
std::string fullWidthToHalfWidth(const std::string& s);

/// 词元是否为「有意义」的中文/英文内容（用于关键词候选过滤）。
bool isContentToken(const std::string& token);

/// 净化文件名：剔除路径分隔符、控制字符、Windows 保留名与首尾空白。
std::string sanitizeFileName(const std::string& name, const std::string& fallback = "untitled");

/// 数值转字符串，去掉多余的尾随零。
std::string numberToString(double value, int maxDecimals = 4);

}  // namespace mm::str

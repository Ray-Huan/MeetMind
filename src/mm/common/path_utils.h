// MeetMind — 路径工具（UTF-8 与原生路径的安全互转）
//
// 为什么需要这个模块
// ---------------------------------------------------------------------------
// 事实 1：本项目的字符串约定是「std::string 承载 UTF-8」。
// 事实 2：Windows 上 std::filesystem::path 的窄字符构造函数（以及 std::ifstream
//         的窄字符路径）按**本地代码页**解释字节，而不是 UTF-8。
// 事实 3：libstdc++（MinGW）在窄窄转换失败时**抛异常**，且该异常发生在
//         path 对象构造阶段 —— 即使调用方使用了带 error_code 的 API 也拦不住。
//
// 后果：只要路径里有一个中文字符，`fs::exists(utf8Path, ec)` 就会在构造 path 时
//       抛出 filesystem_error("Cannot convert character sequence")，直接 terminate。
//       对中文用户来说这等于「任何中文文件名都会让程序崩溃」。
//
// 做法：所有路径进出 std::filesystem 都必须经过本模块。
//       Windows 下走宽字符（UTF-16），这是唯一可靠且不抛异常的路径；
//       非 Windows 下 UTF-8 即原生编码，直接透传。
//
// 输入兼容性：fromUtf8() 先按 UTF-8 严格解码；失败则回退按本地 ANSI 代码页解码。
//             这样既能处理我们内部的 UTF-8 约定，也能容忍从 ANSI 命令行拿到的参数。
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mm::pathutil {

/// UTF-8 字符串 → std::filesystem::path。永不抛异常。
std::filesystem::path fromUtf8(const std::string& utf8);

/// std::filesystem::path → UTF-8 字符串。
std::string toUtf8(const std::filesystem::path& path);

/// 宽字符 → UTF-8（Windows 专用，其他平台做窄化处理）。
std::string wideToUtf8(const wchar_t* wide);
/// UTF-8 → 宽字符（Windows 专用）。
std::wstring utf8ToWide(const std::string& utf8);

// ---- 常用文件系统操作（接受 UTF-8 路径字符串，内部做安全转换）----

bool exists(const std::string& utf8Path);
bool isDirectory(const std::string& utf8Path);
bool isRegularFile(const std::string& utf8Path);
bool createDirectories(const std::string& utf8Path);
bool removeFile(const std::string& utf8Path);
/// 重命名/覆盖；失败时返回 false（不抛异常）。
bool rename(const std::string& fromUtf8, const std::string& toUtf8);

/// 规范化绝对路径；失败时返回原字符串。
std::string canonical(const std::string& utf8Path);

// ---- 路径分解 ----
std::string join(const std::string& dirUtf8, const std::string& nameUtf8);
/// 去掉最后一个扩展名与目录部分（"a/b/c.wav" → "c"）。
std::string stem(const std::string& utf8Path);
std::string parentPath(const std::string& utf8Path);
/// 小写化的扩展名（含点），如 ".wav"；无扩展名返回空串。
std::string extensionLower(const std::string& utf8Path);

/// 列举目录下的普通文件（返回 UTF-8 全路径，已排序）。
/// @param recursive 是否递归子目录
/// @param extensionLowerFilter 非空时只返回该扩展名（小写，含点）
std::vector<std::string> listFiles(const std::string& utf8Dir, bool recursive = false,
                                   const std::string& extensionLowerFilter = {});

/// 由多个片段拼出 UTF-8 路径。
template <typename... Rest>
std::string joinPath(const std::string& first, const Rest&... rest) {
    (void)std::initializer_list<int>{0, (static_cast<void>(rest), 0)...};
    std::filesystem::path p = fromUtf8(first);
    std::initializer_list<std::string> parts{rest...};
    for (const std::string& s : parts) {
        if (s.empty()) continue;
        p /= fromUtf8(s);
    }
    return toUtf8(p);
}

// ---- 文件流（窄字符路径在 Windows 上同样不可用，必须走 path）----
std::ifstream openInput(const std::string& utf8Path);
std::ofstream openOutput(const std::string& utf8Path, bool truncate = true);

}  // namespace mm::pathutil

#include "mm/common/path_utils.h"

#include <algorithm>
#include <system_error>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mm::pathutil {
namespace fs = std::filesystem;

#if defined(_WIN32)
namespace {

/// 按指定代码页把多字节串转成宽字符；失败返回空。
std::wstring decodeWith(unsigned codePage, const std::string& bytes) {
    if (bytes.empty()) return {};
    const int need = ::MultiByteToWideChar(codePage, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring wide(static_cast<size_t>(need), L'\0');
    const int got = ::MultiByteToWideChar(codePage, 0, bytes.data(),
                                          static_cast<int>(bytes.size()), wide.data(), need);
    if (got <= 0) return {};
    wide.resize(static_cast<size_t>(got));
    return wide;
}

}  // namespace
#endif

std::wstring utf8ToWide(const std::string& utf8) {
#if defined(_WIN32)
    std::wstring wide = decodeWith(CP_UTF8, utf8);
    if (!wide.empty() || utf8.empty()) return wide;
    // 回退：调用方给的可能是本地 ANSI 编码（例如 Windows 的命令行参数）
    wide = decodeWith(CP_ACP, utf8);
    if (!wide.empty()) return wide;
    // 最后兜底：逐字节映射，至少保证不抛异常（路径会打不开，但不会崩）
    wide.reserve(utf8.size());
    for (char c : utf8) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return wide;
#else
    std::wstring wide;
    wide.reserve(utf8.size());
    for (char c : utf8) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return wide;
#endif
}

std::string wideToUtf8(const wchar_t* wide) {
    if (wide == nullptr) return {};
#if defined(_WIN32)
    const int len = static_cast<int>(::wcslen(wide));
    if (len == 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    const int got = ::WideCharToMultiByte(CP_UTF8, 0, wide, len, out.data(), need, nullptr, nullptr);
    if (got <= 0) return {};
    out.resize(static_cast<size_t>(got));
    return out;
#else
    const size_t len = std::wcslen(wide);
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(wide[i] & 0xFF)));
    }
    return out;
#endif
}

fs::path fromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
#if defined(_WIN32)
    // 关键：在 Windows 上必须用宽字符构造 path，窄字符构造会按本地代码页
    // 转换并在失败时抛异常（libstdc++ 的已知行为）。
    return fs::path(utf8ToWide(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string toUtf8(const fs::path& path) {
#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    return wideToUtf8(wide.c_str());
#else
    return path.string();
#endif
}

bool exists(const std::string& utf8Path) {
    std::error_code ec;
    return fs::exists(fromUtf8(utf8Path), ec);
}

bool isDirectory(const std::string& utf8Path) {
    std::error_code ec;
    return fs::is_directory(fromUtf8(utf8Path), ec);
}

bool isRegularFile(const std::string& utf8Path) {
    std::error_code ec;
    return fs::is_regular_file(fromUtf8(utf8Path), ec);
}

bool createDirectories(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fromUtf8(utf8Path), ec);
    return !ec || fs::is_directory(fromUtf8(utf8Path), ec);
}

bool removeFile(const std::string& utf8Path) {
    std::error_code ec;
    return fs::remove(fromUtf8(utf8Path), ec);
}

bool rename(const std::string& fromUtf8Path, const std::string& toUtf8Path) {
    std::error_code ec;
    fs::rename(fromUtf8(fromUtf8Path), fromUtf8(toUtf8Path), ec);
    if (!ec) return true;
    // 跨设备或目标已存在时回退为「删除后重命名」
    fs::remove(fromUtf8(toUtf8Path), ec);
    fs::rename(fromUtf8(fromUtf8Path), fromUtf8(toUtf8Path), ec);
    return !ec;
}

std::string canonical(const std::string& utf8Path) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fromUtf8(utf8Path), ec);
    if (ec) return utf8Path;
    return toUtf8(p);
}

std::string join(const std::string& dirUtf8, const std::string& nameUtf8) {
    fs::path p = fromUtf8(dirUtf8);
    if (!nameUtf8.empty()) p /= fromUtf8(nameUtf8);
    return toUtf8(p);
}

std::string stem(const std::string& utf8Path) {
    return toUtf8(fromUtf8(utf8Path).stem());
}

std::string parentPath(const std::string& utf8Path) {
    return toUtf8(fromUtf8(utf8Path).parent_path());
}

std::string extensionLower(const std::string& utf8Path) {
    return str::toLowerAscii(toUtf8(fromUtf8(utf8Path).extension()));
}

std::vector<std::string> listFiles(const std::string& utf8Dir, bool recursive,
                                   const std::string& extensionLowerFilter) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fromUtf8(utf8Dir);
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;

    auto consider = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file(ec)) return;
        if (!extensionLowerFilter.empty()) {
            const std::string ext = str::toLowerAscii(toUtf8(e.path().extension()));
            if (ext != extensionLowerFilter) return;
        }
        out.push_back(toUtf8(e.path()));
    };

    if (recursive) {
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir, ec)) {
            consider(e);
        }
    } else {
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
            consider(e);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::ifstream openInput(const std::string& utf8Path) {
    // libstdc++ 提供 basic_ifstream(const std::filesystem::path&) 重载，
    // 它内部按原生编码（Windows 下为宽字符）打开，可正确处理中文路径。
    std::ifstream in(fromUtf8(utf8Path), std::ios::binary);
    return in;
}

std::ofstream openOutput(const std::string& utf8Path, bool truncate) {
    std::ios::openmode mode = std::ios::binary | std::ios::out;
    mode |= truncate ? std::ios::trunc : std::ios::app;
    std::ofstream out(fromUtf8(utf8Path), mode);
    return out;
}

}  // namespace mm::pathutil

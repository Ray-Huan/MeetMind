#include "mm/nlp/t2s.h"

#include <filesystem>
#include <fstream>
#include <mutex>

#include "mm/common/path_utils.h"
#include "mm/common/config.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::nlp {

Result<void> TraditionalConverter::loadMapping(const std::string& path) {
    if (path.empty() || !pathutil::exists(path)) {
        return fail(ErrorCode::FileNotFound, "繁简映射表不存在: " + path);
    }
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::IoError, "无法打开繁简映射表: " + path);

    std::unordered_map<char32_t, char32_t> table;
    table.reserve(5000);
    std::string line;
    size_t skipped = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            ++skipped;
            continue;
        }
        const std::u32string from = str::toUtf32(line.substr(0, tab));
        const std::u32string to = str::toUtf32(line.substr(tab + 1));
        if (from.size() != 1 || to.size() != 1) {
            ++skipped;
            continue;
        }
        table[from.front()] = to.front();
    }
    if (table.empty()) {
        return fail(ErrorCode::InvalidArgument, "繁简映射表为空或格式不正确: " + path);
    }
    table_ = std::move(table);
    MM_LOG_INFO("t2s") << "繁简映射加载完成: " << table_.size() << " 条 (跳过 " << skipped
                       << " 行)";
    return okStatus();
}

Result<void> TraditionalConverter::loadDefault(const std::string& explicitPath) {
    std::string path = explicitPath;
    if (path.empty()) {
        const std::string dir = Config::locateDataDir();
        if (!dir.empty()) path = pathutil::join(dir, "t2s_zh.txt");
    }
    if (path.empty()) {
        const std::string src = pathutil::join(MEETMIND_SOURCE_DIR, "data/t2s_zh.txt");
        if (pathutil::exists(src)) path = src;
    }
    if (path.empty()) {
        return fail(ErrorCode::FileNotFound,
                    "未找到 data/t2s_zh.txt，请运行 tools/prepare_data.py --t2s 生成");
    }
    return loadMapping(path);
}

bool TraditionalConverter::needsConversion(const std::string& text) const {
    if (table_.empty() || text.empty()) return false;
    for (char32_t c : str::toUtf32(text)) {
        if (table_.find(c) != table_.end()) return true;
    }
    return false;
}

std::string TraditionalConverter::convert(const std::string& text) const {
    if (table_.empty() || text.empty()) return text;
    const std::u32string u = str::toUtf32(text);
    std::u32string out;
    out.reserve(u.size());
    bool changed = false;
    for (char32_t c : u) {
        const auto it = table_.find(c);
        if (it != table_.end()) {
            out.push_back(it->second);
            changed = true;
        } else {
            out.push_back(c);
        }
    }
    return changed ? str::toUtf8(out) : text;
}

const TraditionalConverter& sharedTraditionalConverter() {
    static TraditionalConverter* instance = nullptr;
    static std::once_flag flag;
    std::call_once(flag, [] {
        static TraditionalConverter converter;
        Result<void> r = converter.loadDefault();
        if (!r.ok()) {
            MM_LOG_WARN("t2s") << "繁简映射加载失败（将跳过繁简转换）: " << r.message();
        }
        instance = &converter;
    });
    return *instance;
}

}  // namespace mm::nlp

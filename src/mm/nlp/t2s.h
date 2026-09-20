// MeetMind — 繁→简转换
// 动机：whisper 等中文识别模型对同一段音频可能在繁体/简体之间摇摆（实测 small 模型
//       会把部分句子输出为繁体）。转写后统一转简体，可显著提升关键词抽取质量与纪要可读性。
// 实现：字符级映射表（data/t2s_zh.txt，由 tools/prepare_data.py --t2s 从 zhconv 生成并验证）。
//       不采用「整词替换」是出于稳健性考虑：字符级映射不会因分词错误而漏转。
#pragma once

#include <string>
#include <unordered_map>

#include "mm/common/result.h"

namespace mm::nlp {

class TraditionalConverter {
public:
    TraditionalConverter() = default;

    /// 加载映射表；格式为每行「繁体字<TAB>简体字」，'#' 开头为注释。
    Result<void> loadMapping(const std::string& path);
    /// 自动探测并加载内置映射表（data/t2s_zh.txt）。
    Result<void> loadDefault(const std::string& explicitPath = {});

    bool loaded() const { return !table_.empty(); }
    size_t mappingSize() const { return table_.size(); }

    /// 逐字符转换；未在映射表中的字符原样保留。
    std::string convert(const std::string& text) const;

    /// 是否包含繁体特征字符（用于判断是否需要转换，避免无谓开销）。
    bool needsConversion(const std::string& text) const;

private:
    std::unordered_map<char32_t, char32_t> table_;
};

/// 全局共享实例（首次调用时惰性加载，线程安全）。
const TraditionalConverter& sharedTraditionalConverter();

}  // namespace mm::nlp

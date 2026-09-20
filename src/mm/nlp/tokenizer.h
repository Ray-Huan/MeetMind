// MeetMind — 中文分词器
// 算法：词典驱动的最大概率路径（DAG + 动态规划），与 jieba 的核心思路一致。
//   route[i] = max_j [ log(freq(w_ij)) - log(totalFreq) + route[j+1] ]
// 未登录词回退策略：连续汉字在词典中无匹配时按「单字」输出。
// 词典格式：每行 "词<TAB>词频<TAB>词性"，# 开头为注释。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mm/common/result.h"

namespace mm::nlp {

/// 一个词元。
struct Token {
    std::string text;
    int startChar = 0;   ///< 起始码点索引
    int length = 0;      ///< 码点长度
    double logProb = 0.0;///< 词典对数概率（未登录词为一个很小的惩罚值）
    bool inLexicon = false;
    bool isAscii = false;
};

struct TokenizerConfig {
    /// 词的最大码点长度（与词典构建时保持一致）
    size_t maxWordLength = 12;
    /// 未登录词的等效词频（越小则越倾向长词）
    double unknownWordFrequency = 1.0;
    /// 是否把连续英文/数字作为一个整体词元
    bool groupAsciiRuns = true;
};

class Tokenizer {
public:
    explicit Tokenizer(TokenizerConfig config = TokenizerConfig{});
    ~Tokenizer() = default;

    /// 加载词典；可重复调用以切换词典。
    Result<void> loadLexicon(const std::string& path);
    /// 追加加载同格式的补充词典（不覆盖已有词条；已存在的词取较大词频）。
    Result<void> mergeLexicon(const std::string& path);
    /// 自动探测并加载内置词典（data/lexicon_zh.txt）。
    Result<void> loadDefaultLexicon(const std::string& explicitPath = {});

    bool loaded() const { return !freq_.empty(); }
    size_t lexiconSize() const { return freq_.size(); }
    double totalFrequency() const { return totalFreq_; }

    /// 分词。filterStopwords=true 时过滤停用词（需要先 setStopwords）。
    std::vector<Token> tokenize(const std::string& text, bool filterStopwords = false) const;
    /// 仅返回词元文本。
    std::vector<std::string> cut(const std::string& text, bool filterStopwords = false) const;

    /// 词频查询；不存在返回 0。
    double wordFrequency(const std::string& word) const;
    /// 是否在词典中。
    bool inLexicon(const std::string& word) const { return freq_.find(word) != freq_.end(); }

    void setStopwords(std::vector<std::string> words);
    bool isStopword(const std::string& word) const;

    const TokenizerConfig& config() const { return config_; }

private:
    void segmentCjkRun(const std::string& text, const std::vector<size_t>& charStart, int begin,
                       int end, std::vector<Token>& out) const;

    TokenizerConfig config_;
    std::unordered_map<std::string, double> freq_;  ///< 词 → log 概率
    std::unordered_map<std::string, uint8_t> stopwords_;
    double totalFreq_ = 1.0;
    std::string lexiconPath_;
};

/// 全局共享分词器（首次调用时惰性加载内置词典，线程安全）。
const Tokenizer& sharedTokenizer();

}  // namespace mm::nlp

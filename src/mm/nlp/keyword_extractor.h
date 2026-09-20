// MeetMind — 关键词抽取
// 融合策略：score = α · TextRank(归一化) + (1-α) · TF-IDF(归一化)
// IDF 以「内置词典词频」作为语料统计的近似：idf(w) = log(1 + N / (1 + freq(w)))
#pragma once

#include <string>
#include <vector>

#include "mm/nlp/nlp_types.h"
#include "mm/nlp/textrank.h"
#include "mm/nlp/tokenizer.h"

namespace mm::nlp {

struct KeywordOptions {
    int topK = 12;
    double alpha = 0.6;         ///< TextRank 权重
    int cooccurWindow = 5;
    int minLength = 2;          ///< 词元最小码点长度
    bool preferNounsLike = true;///< 偏好名词性词元（过滤纯动词性单字等）
    TextRankOptions textRank;
};

class KeywordExtractor {
public:
    explicit KeywordExtractor(KeywordOptions options = KeywordOptions{});

    /// 从已分词的多句词元序列抽取关键词。
    std::vector<Keyword> extractFromTokens(
        const std::vector<std::vector<std::string>>& sentenceTokens,
        const Tokenizer& tokenizer) const;

    /// 直接从文本抽取（内部自行分句与分词）。
    std::vector<Keyword> extract(const std::string& text, const Tokenizer& tokenizer) const;

    const KeywordOptions& options() const { return options_; }

private:
    KeywordOptions options_;
};

}  // namespace mm::nlp

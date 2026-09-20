// MeetMind — 抽取式摘要（MMR）
// MMR = argmax_i [ λ·Sim(s_i, D) − (1−λ)·max_{s_j∈S} Sim(s_i, s_j) ]
// 其中 Sim(s, D) 用句子的 TextRank 重要性，Sim(s_i, s_j) 用 TextRank 句子相似度。
// 该实现完全离线、确定性、可复现，作为默认摘要后端。
#pragma once

#include <string>

#include "mm/nlp/isummarizer.h"
#include "mm/nlp/textrank.h"
#include "mm/nlp/tokenizer.h"

namespace mm::nlp {

struct ExtractiveOptions {
    double ratio = 0.25;       ///< 压缩比
    int minSentences = 3;
    int maxSentences = 12;
    double lambda = 0.70;      ///< MMR 权衡系数
    int overviewSentences = 2; ///< 概览取前 N 条要点
    TextRankOptions textRank;
};

class ExtractiveSummarizer final : public ISummarizer {
public:
    explicit ExtractiveSummarizer(const Tokenizer& tokenizer,
                                  ExtractiveOptions options = ExtractiveOptions{});
    ~ExtractiveSummarizer() override = default;

    std::string id() const override { return "extractive"; }
    std::string displayName() const override { return "抽取式摘要（MMR + TextRank）"; }
    bool available() const override { return true; }
    bool isRemote() const override { return false; }

    Result<SummaryDraft> summarize(const SummaryRequest& request) override;

    /// 供测试直接使用：以「已分词句子」为输入做 MMR 选择。
    /// @return 被选中句子的下标（按原文顺序升序）
    std::vector<size_t> selectSentences(
        const std::vector<std::vector<std::string>>& sentenceTokens,
        const std::vector<double>& importance) const;

    /// 目标句数。
    size_t targetSentenceCount(size_t total) const;

private:
    const Tokenizer& tokenizer_;
    ExtractiveOptions options_;
};

}  // namespace mm::nlp

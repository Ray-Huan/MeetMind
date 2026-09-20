// MeetMind — 话题分段（TextTiling 简化版）
// 步骤：句 → 伪句块（每 blockSentences 句）→ 相邻块词向量余弦相似度 → 谷底切分 → 段标题
#pragma once

#include <string>
#include <vector>

#include "mm/nlp/nlp_types.h"
#include "mm/nlp/sentence_splitter.h"
#include "mm/nlp/tokenizer.h"

namespace mm::nlp {

struct TopicSegmentOptions {
    int blockSentences = 3;     ///< 每个伪句块包含的句子数
    double valleySigma = 0.5;   ///< 谷底判定：低于 mean - σ·std
    int minTopicSentences = 3;  ///< 议题最少句子数
    int maxTopics = 12;
    int titleKeywordCount = 3;
};

class TopicSegmenter {
public:
    explicit TopicSegmenter(const Tokenizer& tokenizer,
                            TopicSegmentOptions options = TopicSegmentOptions{});
    ~TopicSegmenter() = default;

    std::vector<Topic> segment(const std::vector<Sentence>& sentences) const;

    /// 计算相邻块的相似度序列（长度 = 块数 - 1），供测试与调试。
    std::vector<double> blockSimilarities(
        const std::vector<std::vector<std::string>>& sentenceTokens) const;

    const TopicSegmentOptions& options() const { return options_; }

private:
    const Tokenizer& tokenizer_;
    TopicSegmentOptions options_;
};

}  // namespace mm::nlp

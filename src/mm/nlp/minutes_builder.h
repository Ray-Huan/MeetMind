// MeetMind — 结构化会议纪要生成
#pragma once

#include <string>
#include <vector>

#include "mm/common/json.h"
#include "mm/common/result.h"
#include "mm/nlp/isummarizer.h"
#include "mm/nlp/nlp_types.h"
#include "mm/nlp/sentence_splitter.h"
#include "mm/nlp/tokenizer.h"

namespace mm::nlp {

/// 会议纪要（最终交付结构）。
struct Minutes {
    std::string title;
    std::string meetingDate;
    std::string overview;                    ///< 会议概览
    std::vector<Topic> topics;               ///< 议题
    std::vector<SummarySentence> keyPoints;  ///< 关键结论
    std::vector<Decision> decisions;         ///< 决议
    std::vector<ActionItem> actionItems;     ///< 待办
    std::vector<Keyword> keywords;           ///< 关键词
    std::vector<std::string> risks;          ///< 风险/待确认事项
    Statistics stats;
    std::string summaryEngine;               ///< 摘要引擎标识

    bool empty() const { return overview.empty() && keyPoints.empty() && actionItems.empty(); }
    Json toJson() const;
};

struct MinutesOptions {
    std::string title;             ///< 空则自动推导
    std::string meetingDate;       ///< "YYYY-MM-DD"，空则今天
    bool enableSummarization = true;
    double summaryRatio = 0.25;
    int summaryMaxSentences = 12;
    int maxKeywords = 12;
    int maxActionItems = 30;
    int topicBlockSentences = 3;
};

class MinutesBuilder {
public:
    MinutesBuilder(const Tokenizer& tokenizer, MinutesOptions options = MinutesOptions{});
    ~MinutesBuilder() = default;

    /// 生成纪要。
    /// @param sentences     带时间戳的句子序列
    /// @param speakerLabels 说话人显示名（下标 = speakerId）
    /// @param stats         统计信息
    /// @param summarizer    摘要后端；为空则使用内置抽取式摘要
    Result<Minutes> build(const std::vector<Sentence>& sentences,
                          const std::vector<std::string>& speakerLabels,
                          const Statistics& stats, ISummarizer* summarizer = nullptr) const;

    /// 从正文自动推导标题。
    static std::string deriveTitle(const std::string& fullText, const std::string& fallback);

    const MinutesOptions& options() const { return options_; }

private:
    const Tokenizer& tokenizer_;
    MinutesOptions options_;
};

}  // namespace mm::nlp

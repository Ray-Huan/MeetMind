// MeetMind — 摘要后端抽象
// 默认实现为完全离线的抽取式摘要（MMR + TextRank）；接口保持可插拔，便于替换其它摘要实现。
#pragma once

#include <string>
#include <vector>

#include "mm/common/result.h"
#include "mm/nlp/nlp_types.h"
#include "mm/nlp/sentence_splitter.h"

namespace mm::nlp {

/// 摘要请求。
struct SummaryRequest {
    std::string title;
    std::vector<Sentence> sentences;
    std::vector<Keyword> keywords;
    std::vector<std::string> speakerLabels;
};

/// 摘要产物。
struct SummaryDraft {
    std::string overview;                 ///< 一段话概览
    std::vector<SummarySentence> keyPoints;///< 要点（保留来源时间戳）
    std::string engine;                   ///< 产出该结果的引擎标识
};

class ISummarizer {
public:
    virtual ~ISummarizer() = default;

    virtual std::string id() const = 0;
    virtual std::string displayName() const = 0;
    /// 当前环境下是否可用；不可用时调用方会回退到内置抽取式摘要。
    virtual bool available() const = 0;
    /// 是否依赖网络/外部服务。
    virtual bool isRemote() const = 0;

    virtual Result<SummaryDraft> summarize(const SummaryRequest& request) = 0;
};

}  // namespace mm::nlp

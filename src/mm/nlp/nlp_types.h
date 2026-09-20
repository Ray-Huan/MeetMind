// MeetMind — NLP 公共类型
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mm/common/json.h"

namespace mm::nlp {

/// 关键词（含权重与来源）。
struct Keyword {
    std::string word;
    double score = 0.0;        ///< 融合得分
    double textRank = 0.0;     ///< TextRank 分量（归一化）
    double tfidf = 0.0;        ///< TF-IDF 分量（归一化）
    int frequency = 0;         ///< 出现次数
};

/// 摘要要点（保留来源时间戳，便于在纪要中回跳原文）。
struct SummarySentence {
    std::string text;
    double score = 0.0;
    int64_t startMs = -1;
    int64_t endMs = -1;
    int speakerId = -1;
    int order = 0;             ///< 原文顺序，用于重组
};

/// 待办事项。
struct ActionItem {
    std::string task;          ///< 任务描述
    std::string owner;         ///< 责任人（空 = 未指明）
    std::string dueDate;       ///< 截止日期 "YYYY-MM-DD"（空 = 未指明）
    std::string dueText;       ///< 原始截止描述文本（如「下周三」）
    std::string priority = "中";///< 高 / 中 / 低
    std::string sourceText;    ///< 原文
    int64_t startMs = -1;
    int speakerId = -1;
};

/// 决议事项。
struct Decision {
    std::string text;
    std::string kind;          ///< 通过 / 否决 / 确定 / 同意 / 其他
    int64_t startMs = -1;
    int speakerId = -1;
};

/// 议题（话题分段结果）。
struct Topic {
    std::string title;
    std::vector<std::string> keywords;
    int startSentence = 0;
    int endSentence = 0;       ///< 开区间
    int64_t startMs = -1;
    int64_t endMs = -1;

    int sentenceCount() const { return endSentence - startSentence; }
};

/// 统计信息。
struct Statistics {
    int64_t totalDurationMs = 0;
    int64_t speechDurationMs = 0;
    int totalCharacters = 0;      ///< 转写正文字符数（码点）
    int cjkCharacters = 0;
    int segmentCount = 0;
    int sentenceCount = 0;
    int speakerCount = 0;
    int lowConfidenceSegments = 0;
    double speechRatio = 0.0;
    std::vector<int> speakerTalkMs;    ///< 各说话人发言时长
    std::vector<int> speakerCharCount; ///< 各说话人字数

    Json toJson() const;
};

}  // namespace mm::nlp

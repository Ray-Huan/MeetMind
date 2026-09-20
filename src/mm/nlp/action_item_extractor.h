// MeetMind — 待办事项抽取
// 方法：触发词识别 + 三要素抽取（任务 / 责任人 / 截止时间），规则可解释、零训练成本。
// 说明：截止时间基于「会议日期」做相对时间推算，因此 meetingDate 必须正确。
#pragma once

#include <string>
#include <vector>

#include "mm/nlp/nlp_types.h"
#include "mm/nlp/sentence_splitter.h"

namespace mm::nlp {

struct ActionItemOptions {
    std::string meetingDate;          ///< "YYYY-MM-DD"；空则用今天
    int maxItems = 30;
    bool inferOwnerFromSpeaker = true;///< 主语为「我」时用说话人作为责任人
    bool requireTrigger = true;       ///< 必须命中触发词才算待办
};

class ActionItemExtractor {
public:
    explicit ActionItemExtractor(ActionItemOptions options = ActionItemOptions{});

    std::vector<ActionItem> extract(const std::vector<Sentence>& sentences,
                                    const std::vector<std::string>& speakerLabels) const;

    /// 判断句子是否为待办句，命中时输出触发词。
    static bool isActionSentence(const std::string& text, std::string* trigger = nullptr);

    /// 从文本中抽取截止日期；返回 ISO 日期字符串，未识别返回空。
    static std::string extractDueDate(const std::string& text, const std::string& meetingDate,
                                      std::string* rawText = nullptr);

    /// 从文本中抽取责任人；未识别返回空。
    static std::string extractOwner(const std::string& text);

    /// 优先级判定。
    static std::string detectPriority(const std::string& text);

    const ActionItemOptions& options() const { return options_; }

private:
    ActionItemOptions options_;
};

}  // namespace mm::nlp

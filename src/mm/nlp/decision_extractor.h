// MeetMind — 决议抽取
// 识别含「决定/通过/同意/否决」等决策表达的句子，并归入决议类型。
#pragma once

#include <string>
#include <vector>

#include "mm/nlp/nlp_types.h"
#include "mm/nlp/sentence_splitter.h"

namespace mm::nlp {

struct DecisionOptions {
    int maxItems = 20;
    bool skipQuestions = true;
};

class DecisionExtractor {
public:
    explicit DecisionExtractor(DecisionOptions options = DecisionOptions{});

    std::vector<Decision> extract(const std::vector<Sentence>& sentences) const;

    /// 判定句子是否含决策表达；命中时输出决策类型与触发词。
    static bool classify(const std::string& text, std::string* kind = nullptr,
                         std::string* trigger = nullptr);

    const DecisionOptions& options() const { return options_; }

private:
    DecisionOptions options_;
};

}  // namespace mm::nlp

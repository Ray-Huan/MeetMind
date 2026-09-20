#include "mm/nlp/decision_extractor.h"

#include <unordered_set>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::nlp {
namespace {

struct DecisionRule {
    const char* kind;
    const char* trigger;
};

const std::vector<DecisionRule>& rules() {
    static const std::vector<DecisionRule> kRules = {
        // 否决优先匹配（避免「不同意」被「同意」误判）
        {"否决", "不同意"}, {"否决", "不通过"}, {"否决", "否决"}, {"否决", "驳回"},
        {"否决", "否决掉"}, {"否决", "暂缓"},   {"否决", "取消"},
        {"通过", "通过"},   {"通过", "批准"},   {"通过", "审批通过"},
        {"确定", "决定"},   {"确定", "确定"},   {"确定", "敲定"},   {"确定", "定下来"},
        {"确定", "拍板"},
        {"确定", "就按"},   {"确定", "就这么定"},
        {"同意", "同意"},   {"同意", "赞成"},   {"同意", "认可"},   {"同意", "无异议"},
        {"同意", "没意见"}, {"同意", "达成一致"}, {"同意", "达成共识"}, {"同意", "一致同意"},
    };
    return kRules;
}

}  // namespace

DecisionExtractor::DecisionExtractor(DecisionOptions options) : options_(options) {}

bool DecisionExtractor::classify(const std::string& text, std::string* kind,
                                 std::string* trigger) {
    const std::string t = str::trim(text);
    if (t.empty()) return false;
    for (const DecisionRule& r : rules()) {
        if (str::contains(t, r.trigger)) {
            if (kind) *kind = r.kind;
            if (trigger) *trigger = r.trigger;
            return true;
        }
    }
    return false;
}

std::vector<Decision> DecisionExtractor::extract(const std::vector<Sentence>& sentences) const {
    std::vector<Decision> out;
    std::unordered_set<std::string> seen;

    for (const Sentence& s : sentences) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        if (options_.skipQuestions &&
            (str::endsWith(text, "？") || str::endsWith(text, "?"))) {
            continue;
        }
        // 排除「能不能通过」这类询问
        if (str::contains(text, "能不能") || str::contains(text, "能不能通过") ||
            str::contains(text, "是否通过") || str::contains(text, "会不会")) {
            continue;
        }

        std::string kind;
        std::string trigger;
        if (!classify(text, &kind, &trigger)) continue;

        // 「同意」若同时出现否定词则归为否决
        if (kind == std::string("同意") &&
            (str::contains(text, "不同意") || str::contains(text, "不赞成") ||
             str::contains(text, "无法同意"))) {
            kind = "否决";
        }

        if (!seen.insert(text).second) continue;

        Decision d;
        d.text = text;
        d.kind = kind;
        d.startMs = s.startMs;
        d.speakerId = s.speakerId;
        out.push_back(std::move(d));
    }

    if (options_.maxItems > 0 && static_cast<int>(out.size()) > options_.maxItems) {
        out.resize(static_cast<size_t>(options_.maxItems));
    }
    MM_LOG_INFO("decision") << "决议抽取完成: " << out.size() << " 条";
    return out;
}

}  // namespace mm::nlp

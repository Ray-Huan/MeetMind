#include "mm/nlp/punctuation_restorer.h"

#include <algorithm>

#include "mm/common/string_utils.h"

namespace mm::nlp {
namespace {

const std::vector<std::string>& interrogativeMarkers() {
    static const std::vector<std::string> kMarkers = {
        // 注意：「吧」是建议/祈使语气词（「我们开始吧」），不计入疑问
        "吗", "呢", "么", "是不是", "有没有", "行不行", "好不好", "对不对",
        "如何", "是否", "为什么", "怎么", "怎样", "多少", "哪些", "哪个", "哪里",
        "什么时候", "能不能", "可不可以", "要不要", "值不值",
    };
    return kMarkers;
}

const std::vector<std::string>& imperativeMarkers() {
    static const std::vector<std::string> kMarkers = {
        "请", "麻烦", "务必", "尽快", "记得", "别忘", "一定要", "需要你", "麻烦你",
    };
    return kMarkers;
}

/// 内部断句锚点（连接词），断句时将标点插入到连接词之前。
const std::vector<std::string>& breakAnchors() {
    static const std::vector<std::string> kAnchors = {
        "但是", "不过", "然而", "所以", "因此", "然后", "接着", "另外", "此外",
        "同时", "因为", "由于", "而且", "并且", "其实", "另外一方面", "另一方面",
        "第一", "第二", "第三", "第四",
    };
    return kAnchors;
}

char32_t lastCodePoint(const std::string& text, size_t* byteLen = nullptr) {
    const std::u32string u = str::toUtf32(text);
    if (u.empty()) {
        if (byteLen) *byteLen = 0;
        return 0;
    }
    if (byteLen) {
        std::u32string tail(1, u.back());
        *byteLen = str::toUtf8(tail).size();
    }
    return u.back();
}

size_t codePointCount(const std::string& text) { return str::utf8Length(text); }

}  // namespace

PunctuationRestorer::PunctuationRestorer(PunctuationOptions options) : options_(options) {}

bool PunctuationRestorer::hasTerminalPunctuation(const std::string& text) {
    const std::string t = str::trimRight(text);
    if (t.empty()) return false;
    const char32_t c = lastCodePoint(t);
    return str::isSentenceEnd(c) || c == U'。' || c == U'，' || c == U'、' || c == U'：' ||
           c == U'：' || c == U'”' || c == U'）';
}

bool PunctuationRestorer::looksInterrogative(const std::string& text) {
    const std::string t = str::trim(text);
    if (t.empty()) return false;
    for (const std::string& m : interrogativeMarkers()) {
        if (str::endsWith(t, m)) return true;
    }
    // 句中疑问词 + 结尾助词
    static const std::vector<std::string> kInner = {"为什么", "怎么", "是否", "能不能",
                                                    "要不要", "多少"};
    for (const std::string& m : kInner) {
        if (str::contains(t, m)) return true;
    }
    return false;
}

bool PunctuationRestorer::looksImperative(const std::string& text) {
    const std::string t = str::trim(text);
    if (t.empty()) return false;
    for (const std::string& m : imperativeMarkers()) {
        if (str::startsWith(t, m)) return true;
    }
    return false;
}

std::string PunctuationRestorer::restore(const std::string& text,
                                         const PunctuationContext& context) const {
    std::string t = str::trim(text);
    if (t.empty()) return t;
    if (hasTerminalPunctuation(t)) return t;  // 幂等

    // ---- 内部断句 ----
    if (options_.insertInternalBreaks) {
        const size_t len = codePointCount(t);
        if (len >= static_cast<size_t>(options_.longTextChars)) {
            const char32_t inner = (len >= static_cast<size_t>(options_.veryLongTextChars))
                                       ? U'。'
                                       : U'，';
            const std::u32string u = str::toUtf32(t);
            std::u32string built;
            built.reserve(u.size() + 8);
            const size_t threshold = static_cast<size_t>(options_.longTextChars);
            size_t sinceBreak = 0;
            size_t i = 0;
            while (i < u.size()) {
                if (sinceBreak >= threshold && !built.empty()) {
                    for (const std::string& anchor : breakAnchors()) {
                        const std::u32string a = str::toUtf32(anchor);
                        if (i + a.size() <= u.size() && u.compare(i, a.size(), a) == 0) {
                            if (!str::isSentenceEnd(built.back()) && built.back() != U'，') {
                                built.push_back(inner);
                                sinceBreak = 0;
                            }
                            break;
                        }
                    }
                }
                built.push_back(u[i]);
                ++sinceBreak;
                ++i;
            }
            t = str::toUtf8(built);
        }
    }

    // ---- 终止标点 ----
    if (!options_.addTerminal) return t;

    char32_t terminal = 0;
    if (looksInterrogative(t)) {
        terminal = U'？';
    } else if (looksImperative(t)) {
        terminal = U'。';
    } else if (context.isLast || context.gapToNextMs < 0) {
        terminal = U'。';
    } else if (context.gapToNextMs >= options_.periodPauseMs) {
        terminal = U'。';
    } else if (context.gapToNextMs >= options_.commaPauseMs) {
        terminal = U'，';
    } else {
        // 停顿很短：视为被 VAD 断开的同一句，暂不补标点
        return t;
    }
    t += str::toUtf8(std::u32string(1, terminal));
    return t;
}

void PunctuationRestorer::restoreSequence(std::vector<std::string>& texts,
                                          const std::vector<int64_t>& gaps) const {
    for (size_t i = 0; i < texts.size(); ++i) {
        PunctuationContext ctx;
        ctx.isLast = (i + 1 >= texts.size());
        if (!ctx.isLast && i < gaps.size()) {
            ctx.gapToNextMs = gaps[i];
        }
        texts[i] = restore(texts[i], ctx);
    }
}

}  // namespace mm::nlp

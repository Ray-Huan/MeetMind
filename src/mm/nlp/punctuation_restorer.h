// MeetMind — 标点恢复
// 面向「无标点 ASR 输出」的场景：依据段间停顿长度与句式特征补全终止标点，
// 并对超长无标点串按语义连接词做内部断句。对已有标点的文本保持幂等。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm::nlp {

struct PunctuationOptions {
    int commaPauseMs = 350;    ///< 停顿 ≥ 该值视为分句，插入「，」
    int periodPauseMs = 800;   ///< 停顿 ≥ 该值视为句末，插入「。」
    int longTextChars = 30;    ///< 超过该长度的无标点串触发内部断句
    int veryLongTextChars = 60;///< 超过该长度时使用句号断句
    bool addTerminal = true;   ///< 是否补充终止标点
    bool insertInternalBreaks = true; ///< 是否对超长串插入内部标点
};

/// 段落级上下文（用于判决终止标点）。
struct PunctuationContext {
    int64_t gapToNextMs = -1;  ///< 与下一段的间隔；-1 表示未知/最后一段
    bool isLast = false;
};

class PunctuationRestorer {
public:
    explicit PunctuationRestorer(PunctuationOptions options = PunctuationOptions{});
    ~PunctuationRestorer() = default;

    /// 单段恢复。
    std::string restore(const std::string& text, const PunctuationContext& context) const;

    /// 批量恢复：就地修改 texts，gaps[i] 表示 texts[i] 与 texts[i+1] 之间的静音时长。
    void restoreSequence(std::vector<std::string>& texts, const std::vector<int64_t>& gaps) const;

    /// 文本是否已以终止标点结尾。
    static bool hasTerminalPunctuation(const std::string& text);
    /// 是否含疑问特征（疑问词/疑问语气词/问号）。
    static bool looksInterrogative(const std::string& text);
    /// 是否为请求/祈使句（「请…」「麻烦…」等）。
    static bool looksImperative(const std::string& text);

    const PunctuationOptions& options() const { return options_; }

private:
    PunctuationOptions options_;
};

}  // namespace mm::nlp

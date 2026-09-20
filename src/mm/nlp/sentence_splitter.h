// MeetMind — 句子切分
// 依据中文/英文句末标点切分，保留标点；支持超长句在逗号处二次切分。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm::nlp {

struct Sentence {
    std::string text;
    int index = 0;         ///< 全局序号
    int64_t startMs = -1;  ///< 若已知来源时间戳
    int64_t endMs = -1;
    int speakerId = -1;
};

struct SentenceSplitOptions {
    /// 超过该码点长度的句子会在逗号/分号处二次切分（0 = 关闭）
    int maxSentenceChars = 0;
    /// 是否保留标点
    bool keepPunctuation = true;
};

class SentenceSplitter {
public:
    explicit SentenceSplitter(SentenceSplitOptions options = SentenceSplitOptions{});

    /// 切分纯文本（无时间戳）。
    std::vector<Sentence> split(const std::string& text) const;

    /// 切分带时间戳的转写段：段内多句按字符比例粗略分配时间戳。
    struct TimedText {
        std::string text;
        int64_t startMs = 0;
        int64_t endMs = 0;
        int speakerId = -1;
    };
    std::vector<Sentence> splitTimed(const std::vector<TimedText>& segments) const;

    /// 按给定时长权重把一段文本切分为 weights.size() 段。
    /// 切分点优先落在句子边界，避免把句子切断；用于把「一个转写单元的结果」
    /// 回填到其覆盖的多个语音段上。
    /// @param weights   各段权重（通常为时长，毫秒）；全为 0 时按等分处理
    /// @param tokenizer 非空时切点会优先吸附到**词边界**，避免把「包装」切成「包|装」。
    ///                  中文 ASR 输出常整段无标点，此时词边界是唯一的「安全切点」。
    /// @return 长度等于 weights.size() 的文本片段（可能为空串）
    static std::vector<std::string> splitByWeights(const std::string& text,
                                                   const std::vector<int64_t>& weights,
                                                   const class Tokenizer* tokenizer = nullptr);

    const SentenceSplitOptions& options() const { return options_; }

private:
    SentenceSplitOptions options_;
};

}  // namespace mm::nlp

// MeetMind — 语音识别公共类型
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mm/common/json.h"

namespace mm::asr {

/// 词级时间戳（可选，由后端能力决定）。
struct AsrWord {
    std::string text;
    int64_t startMs = 0;
    int64_t endMs = 0;
    float probability = 1.0f;
};

/// 一条转写结果（通常对应一个语音段）。
struct AsrSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string text;
    float confidence = 1.0f;   ///< 平均 token 概率，[0,1]
    float noSpeechProb = 0.0f; ///< 后端给出的「非语音」概率
    int speakerId = -1;
    std::vector<AsrWord> words;

    int64_t durationMs() const noexcept { return endMs > startMs ? endMs - startMs : 0; }
};

/// 一次完整文件的转写结果。
struct AsrResult {
    std::string language;       ///< 后端识别出的语言代码
    std::string backend;        ///< 使用的后端标识
    std::vector<AsrSegment> segments;
    int64_t audioMs = 0;
    int64_t elapsedMs = 0;
    int64_t modelLoadMs = 0;

    /// 拼接全文；sep 默认为空（中文无需空格）。
    std::string fullText(const std::string& sep = "") const;

    /// 低置信段数量（confidence < threshold）。
    int lowConfidenceCount(float threshold = 0.45f) const;

    /// 实时倍率（audioMs / elapsedMs）；elapsedMs 为 0 时返回 0。
    double realTimeFactor() const;

    Json toJson() const;
};

/// 后端初始化配置。
struct AsrModelConfig {
    std::string modelPath;              ///< whisper ggml 模型路径
    std::string language = "auto";      ///< "auto" / "zh" / "en"
    int threads = 4;
    bool useGpu = false;
    bool translate = false;
    bool wordTimestamps = false;
    float temperatureInc = 0.2f;
    /// 回放后端使用：脚本文件路径（每行一条文本；空行表示该段无语音）
    std::string replayScriptPath;
    /// 回放后端使用：是否按段时长做小幅随机（0 = 完全确定性）
    double replayJitter = 0.0;
};

/// 后端能力声明。
struct AsrCapabilities {
    bool wordTimestamps = false;
    bool streaming = false;
    bool multilingual = true;
    bool onDevice = true;
    bool requiresModelFile = true;
    std::string description;
};

}  // namespace mm::asr

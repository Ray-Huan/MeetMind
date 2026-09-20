// MeetMind — 语音活动检测（VAD）
// 双门限（自适应能量 + 过零率）+ 迟滞判决，输出带时间戳的语音段。
#pragma once

#include <vector>

#include "mm/audio/audio_types.h"
#include "mm/common/result.h"

namespace mm::audio {

struct VadConfig {
    int frameLengthMs = 25;
    int frameShiftMs = 10;
    /// >0 时使用固定门限增量（dB）；<=0 时按噪声底自适应
    double thresholdDeltaDb = 0.0;
    double noiseFloorPercentile = 0.10;
    /// 自适应门限增量 = clamp(dynamicFactor × (高声位能量 − 噪声底), min, max)
    double dynamicFactor = 0.40;
    double minDeltaDb = 5.0;
    double maxDeltaDb = 18.0;
    /// 过零率上限（仅在能量处于门限边界 3 dB 内时参与判决）
    double maxZeroCrossingRate = 0.60;
    int enterFrames = 3;
    int exitFrames = 8;
    int minSpeechMs = 200;
    int minSilenceMs = 300;
};

/// 单帧分析结果。
struct VadFrame {
    int64_t startMs = 0;
    float rmsDb = -100.0f;
    float zcr = 0.0f;
    bool voiced = false;
};

struct VadResult {
    std::vector<VadFrame> frames;
    std::vector<SpeechSegment> segments;
    double noiseFloorDb = -100.0;
    double speechLevelDb = -100.0;
    double thresholdDb = -100.0;
    double speechRatio = 0.0;   ///< 语音时长 / 总时长
};

class VoiceActivityDetector {
public:
    explicit VoiceActivityDetector(const VadConfig& config = VadConfig{});

    /// 逐帧分析 + 判决 + 段合并。
    Result<VadResult> process(const AudioBuffer& audio) const;

    /// 仅做逐帧能量/过零率分析（不自适应），供可视化使用。
    static std::vector<VadFrame> frameMetrics(const AudioBuffer& audio, int frameLengthMs,
                                              int frameShiftMs);

    const VadConfig& config() const { return config_; }

private:
    VadConfig config_;
};

}  // namespace mm::audio

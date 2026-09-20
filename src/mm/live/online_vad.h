// MeetMind — 在线（增量）语音活动检测
// 与离线 VoiceActivityDetector 的区别：本类按「音频块」增量喂入、边收边判，
// 内部维持自适应噪声底与迟滞状态机，无需看到完整音频，适合麦克风 / 实时流。
// 判决口径与离线版一致（双门限 + 迟滞），但噪声底改为「瞬时下降 / 缓慢回升」。
#pragma once

#include <cstdint>
#include <vector>

#include "mm/audio/audio_types.h"

namespace mm::live {

struct OnlineVadConfig {
    int frameLengthMs = 25;
    int frameShiftMs = 10;
    /// 门限 = 噪声底 + clamp(dynamicFactor ×（峰值位 − 噪声底）, minDeltaDb, maxDeltaDb)
    double dynamicFactor = 0.40;
    double minDeltaDb = 5.0;
    double maxDeltaDb = 18.0;
    /// 过零率上限：仅在能量贴近门限时参与判决（用于区分清音与噪声）
    double maxZeroCrossingRate = 0.60;
    /// 连续多少帧有声才进入语音（抑制突发噪声）
    int enterFrames = 3;
    /// 连续多少帧无声才判定语音结束。≈ 判句停顿：取 600 ms，
    /// 会议对话的自然停顿约 1.5 s，600 ms 既能及时断句又不会在句中误断。
    int exitFrames = 60;
    int minSpeechMs = 250;
    int sampleRate = 16000;
};

/// 增量 VAD。调用方把任意长度的音频块喂进来，取得「本块内被判定结束」的语音段。
class OnlineVad {
public:
    explicit OnlineVad(const OnlineVadConfig& cfg = OnlineVadConfig{});

    /// 增量喂入 16 kHz 单声道浮点样本。
    /// @return 本次调用中闭合的语音段（可能为空）；时间戳相对流起点，单位毫秒
    std::vector<SpeechSegment> feed(const float* samples, size_t count);

    /// 强制闭合当前语音段（用于流结束或达到时延上限）。
    /// @param out 非空且确实有语音时写入该段
    /// @return 是否产出了一段
    bool forceClose(SpeechSegment* out);

    bool inSpeech() const { return inSpeech_; }
    /// 当前语音段起始毫秒；不在语音中返回 -1。
    int64_t currentStartMs() const { return speechStartMs_; }
    /// 当前语音段已持续毫秒（不在语音中返回 0）。
    int64_t currentSpeechMs() const;
    /// 已分析音频总时长（毫秒）。
    int64_t elapsedMs() const;

    /// 便于诊断：当前自适应门限（dBFS）。
    double thresholdDb() const { return thresholdDb_; }
    /// 便于诊断：当前噪声底（dBFS）。
    double noiseFloorDb() const { return noiseFloorDb_; }

private:
    void processFrame(const float* frame, int len, std::vector<SpeechSegment>* out);
    void closeSegment(int64_t endMs, std::vector<SpeechSegment>* out);

    OnlineVadConfig cfg_;
    std::vector<float> buf_;      ///< 样本缓冲（含帧重叠所需的历史）
    size_t pos_ = 0;              ///< 下一个待分析帧在 buf_ 中的起点
    int64_t frameIndex_ = 0;      ///< 已处理帧计数
    int64_t analyzedSamples_ = 0; ///< 已分析样本数（用于时间轴）

    bool inSpeech_ = false;
    int64_t speechStartMs_ = -1;
    int64_t lastVoicedEndMs_ = -1;
    int voicedRun_ = 0;
    int unvoicedRun_ = 0;

    double noiseFloorDb_ = -60.0;
    double peakDb_ = -60.0;
    double thresholdDb_ = -50.0;
    double energySum_ = 0.0;      ///< 当前段能量累加（线性功率）
    int64_t energyFrames_ = 0;
};

}  // namespace mm::live

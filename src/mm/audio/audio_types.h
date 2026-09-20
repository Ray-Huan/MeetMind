// MeetMind — 音频基础类型
// 约定：内部统一使用「单声道 / float32 / -1..1 归一化」表示。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mm/common/json.h"

namespace mm {

/// 单声道浮点音频缓冲。
struct AudioBuffer {
    std::vector<float> samples;
    int sampleRate = 16000;

    size_t frameCount() const noexcept { return samples.size(); }
    bool empty() const noexcept { return samples.empty(); }

    int64_t durationMs() const noexcept {
        if (sampleRate <= 0) return 0;
        return static_cast<int64_t>(samples.size()) * 1000 / sampleRate;
    }

    void clear() {
        samples.clear();
    }

    void reserveMs(int64_t ms) {
        if (sampleRate > 0 && ms > 0) {
            samples.reserve(static_cast<size_t>(ms) * static_cast<size_t>(sampleRate) / 1000u);
        }
    }
};

/// 采样点到毫秒（截断）。
inline int64_t samplesToMs(size_t samples, int sampleRate) {
    if (sampleRate <= 0) return 0;
    return static_cast<int64_t>(samples) * 1000 / sampleRate;
}

/// 毫秒到采样点（四舍五入）。
inline size_t msToSamples(int64_t ms, int sampleRate) {
    if (sampleRate <= 0 || ms <= 0) return 0;
    return static_cast<size_t>((ms * sampleRate + 500) / 1000);
}

/// 语音段（时间轴区间，毫秒，闭开区间语义 [startMs, endMs)）。
struct SpeechSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    float energyDb = -100.0f;   ///< 段内平均 RMS（dBFS）
    int speakerId = -1;         ///< -1 表示未分配

    int64_t durationMs() const noexcept { return endMs > startMs ? endMs - startMs : 0; }
};

/// WAV 文件元信息。
struct WavInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int formatCode = 0;          ///< 1=PCM, 3=IEEE float, 0xFFFE=EXTENSIBLE
    int64_t frameCount = 0;
    std::string encodingName;    ///< "PCM16" / "FLOAT32" ...
    int64_t dataBytes = 0;
};

/// 音频质量报告（用于界面提示与测试断言）。
struct AudioQualityReport {
    int64_t durationMs = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    std::string encoding;
    int64_t totalSamples = 0;
    double peakDbfs = -100.0;
    double rmsDbfs = -100.0;
    double clippingRatio = 0.0;   ///< |x| >= 0.999 的样本占比
    int64_t nearSilentMs = 0;     ///< 20 ms 块内 RMS < -60 dBFS 的累计时长
    double dcOffset = 0.0;

    Json toJson() const;
    /// 单行可读摘要。
    std::string toSummary() const;
};

}  // namespace mm

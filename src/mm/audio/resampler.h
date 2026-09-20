// MeetMind — 采样率转换
// 策略：降采样时先施加抗混叠低通（窗函数 sinc），再做线性插值；升采样直接线性插值。
#pragma once

#include "mm/audio/audio_types.h"
#include "mm/common/result.h"

namespace mm::audio {

struct ResampleStats {
    int sourceRate = 0;
    int targetRate = 0;
    int64_t inputSamples = 0;
    int64_t outputSamples = 0;
    int filterTaps = 0;
    double cutoffHz = 0.0;
};

class Resampler {
public:
    /// @param quality 0=快速（无滤波） 1=标准  2=高质量（更长的 FIR）
    static Result<AudioBuffer> resample(const AudioBuffer& input,
                                        int targetSampleRate,
                                        int quality = 1,
                                        ResampleStats* stats = nullptr);

    /// 计算抗混叠 FIR 系数（汉明窗 sinc），供测试与复用。
    static std::vector<float> designLowpass(double cutoffNormalized, int taps);
};

}  // namespace mm::audio

// MeetMind — Mel 滤波器组与 MFCC 特征
// 实现细节：预加重 → 分帧(汉明窗) → 功率谱 → Mel 三角滤波 → 对数 → DCT-II → 倒谱提升
#pragma once

#include <utility>
#include <vector>

#include "mm/audio/audio_types.h"

namespace mm::dsp {

struct MfccConfig {
    int sampleRate = 16000;
    int frameLengthMs = 25;
    int frameShiftMs = 10;
    int numFilters = 24;      ///< Mel 三角滤波器个数
    int numCoeffs = 13;       ///< 保留的倒谱系数个数
    double lowFreqHz = 20.0;
    double highFreqHz = -1.0;  ///< <0 表示取 Nyquist
    double preemphasis = 0.97;
    bool useLifter = true;
    double lifterCepstral = 22.0;
};

/// 稀疏表示的 Mel 三角滤波器组。
class MelFilterBank {
public:
    explicit MelFilterBank(const MfccConfig& config);

    /// 输入功率谱（长度 = FFT/2 + 1），输出各滤波器能量。
    std::vector<float> apply(const std::vector<float>& powerSpectrum) const;

    size_t filterCount() const { return filters_.size(); }
    /// 第 i 个滤波器的中心频率（Hz）。
    double centerFrequencyHz(size_t index) const;

    static double hzToMel(double hz);
    static double melToHz(double mel);

private:
    MfccConfig config_;
    int fftSize_ = 0;
    int spectrumBins_ = 0;
    std::vector<std::vector<std::pair<int, float>>> filters_;  ///< (bin, weight)
    std::vector<double> centersHz_;
};

/// MFCC 特征提取器。
class MfccExtractor {
public:
    explicit MfccExtractor(const MfccConfig& config = MfccConfig{});

    /// 整段音频 → 特征矩阵（行 = 帧，列 = 系数）。
    std::vector<std::vector<float>> compute(const AudioBuffer& audio) const;

    /// 单帧特征（帧长度不足时零填充）。
    std::vector<float> computeFrame(const std::vector<float>& frame, float prevSample = 0.0f) const;

    const MfccConfig& config() const { return config_; }
    int fftSize() const { return fftSize_; }
    int spectrumBins() const { return fftSize_ / 2 + 1; }
    int frameLengthSamples() const;
    int frameShiftSamples() const;
    /// 给定样本数可产生的帧数；不足一帧返回 0。
    int frameCount(size_t sampleCount) const;
    /// 第 i 帧的起始采样点。
    size_t frameStartSample(int index) const {
        return static_cast<size_t>(index) * static_cast<size_t>(frameShiftSamples());
    }

    /// 计算一阶差分（Δ）与二阶差分（ΔΔ）。
    static std::vector<std::vector<float>> delta(const std::vector<std::vector<float>>& features,
                                                 int window = 2);
    /// 倒谱均值归一化（CMN），逐维减均值，可提升噪声鲁棒性。
    static void meanNormalize(std::vector<std::vector<float>>& features);

private:
    MfccConfig config_;
    int fftSize_ = 512;
    MelFilterBank bank_;
    std::vector<float> window_;
    std::vector<float> lifter_;
};

}  // namespace mm::dsp

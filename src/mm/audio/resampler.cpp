#include "mm/audio/resampler.h"

#include <algorithm>
#include <cmath>

#include "mm/common/logger.h"

namespace mm::audio {
namespace {
// 不依赖 M_PI（严格 ANSI 模式下部分工具链不提供）
constexpr double kPi = 3.14159265358979323846;
}  // namespace

std::vector<float> Resampler::designLowpass(double cutoffNormalized, int taps) {
    // cutoffNormalized: 相对 Nyquist 的归一化截止频率 (0,1]
    if (taps < 3) taps = 3;
    if (taps % 2 == 0) ++taps;
    std::vector<float> h(static_cast<size_t>(taps), 0.0f);
    const int mid = taps / 2;
    const double fc = std::max(1e-6, std::min(1.0, cutoffNormalized));

    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double x = static_cast<double>(n - mid);
        // 理想低通冲击响应
        const double sinc = (std::fabs(x) < 1e-9) ? fc : std::sin(kPi * fc * x) / (kPi * x);
        // 汉明窗
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                                static_cast<double>(taps - 1));
        h[static_cast<size_t>(n)] = static_cast<float>(sinc * w);
        sum += h[static_cast<size_t>(n)];
    }
    if (std::fabs(sum) > 1e-12) {
        for (float& v : h) v = static_cast<float>(v / sum);
    }
    return h;
}

Result<AudioBuffer> Resampler::resample(const AudioBuffer& input, int targetSampleRate,
                                        int quality, ResampleStats* stats) {
    if (targetSampleRate <= 0) {
        return fail(ErrorCode::InvalidArgument, "目标采样率必须为正数");
    }
    if (input.sampleRate <= 0) {
        return fail(ErrorCode::InvalidArgument, "源采样率非法");
    }

    AudioBuffer out;
    out.sampleRate = targetSampleRate;

    if (stats) {
        *stats = ResampleStats{};
        stats->sourceRate = input.sampleRate;
        stats->targetRate = targetSampleRate;
        stats->inputSamples = static_cast<int64_t>(input.samples.size());
    }

    if (input.samples.empty()) return out;

    if (input.sampleRate == targetSampleRate) {
        out.samples = input.samples;
        if (stats) stats->outputSamples = static_cast<int64_t>(out.samples.size());
        return out;
    }

    const bool downsampling = targetSampleRate < input.sampleRate;
    const double ratio = static_cast<double>(input.sampleRate) / static_cast<double>(targetSampleRate);

    // 抗混叠：仅在降采样且质量 > 0 时启用
    const AudioBuffer* source = &input;
    AudioBuffer filtered;
    if (downsampling && quality > 0) {
        const int taps = (quality >= 2) ? 127 : 63;
        // 目标截止 = 0.45 × 目标采样率；换算为相对「源 Nyquist」的归一化频率
        const double cutoffNorm = 0.90 / ratio;
        const std::vector<float> h = designLowpass(cutoffNorm, taps);
        const int mid = static_cast<int>(h.size()) / 2;
        filtered.sampleRate = input.sampleRate;
        filtered.samples.resize(input.samples.size());
        const int n = static_cast<int>(input.samples.size());
        for (int i = 0; i < n; ++i) {
            double acc = 0.0;
            double weight = 0.0;
            const int lo = std::max(0, i - mid);
            const int hi = std::min(n - 1, i + mid);
            for (int k = lo; k <= hi; ++k) {
                const double c = h[static_cast<size_t>(k - i + mid)];
                acc += static_cast<double>(input.samples[static_cast<size_t>(k)]) * c;
                weight += c;
            }
            // 边界处按实际参与系数归一化，避免首尾出现淡入淡出
            filtered.samples[static_cast<size_t>(i)] =
                static_cast<float>(std::fabs(weight) > 1e-9 ? acc / weight : 0.0);
        }
        source = &filtered;
        if (stats) {
            stats->filterTaps = taps;
            stats->cutoffHz = cutoffNorm * 0.5 * input.sampleRate;
        }
    }

    const size_t outCount = static_cast<size_t>(
        std::llround(static_cast<double>(source->samples.size()) / ratio));
    out.samples.resize(outCount);
    const size_t inCount = source->samples.size();

    for (size_t i = 0; i < outCount; ++i) {
        const double srcPos = static_cast<double>(i) * ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        if (i0 >= inCount - 1) {
            out.samples[i] = source->samples[inCount - 1];
            continue;
        }
        const double frac = srcPos - static_cast<double>(i0);
        const double a = source->samples[i0];
        const double b = source->samples[i0 + 1];
        out.samples[i] = static_cast<float>(a + (b - a) * frac);
    }

    if (stats) stats->outputSamples = static_cast<int64_t>(out.samples.size());
    MM_LOG_DEBUG("resampler") << "resample " << input.sampleRate << " -> " << targetSampleRate
                             << " samples " << input.samples.size() << " -> "
                             << out.samples.size();
    return out;
}

}  // namespace mm::audio

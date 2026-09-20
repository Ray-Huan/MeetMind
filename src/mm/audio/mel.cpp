#include "mm/audio/mel.h"

#include <algorithm>
#include <cmath>

#include "mm/audio/fft.h"

namespace mm::dsp {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kLogFloor = 1e-10f;
}  // namespace

double MelFilterBank::hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double MelFilterBank::melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

MelFilterBank::MelFilterBank(const MfccConfig& config) : config_(config) {
    fftSize_ = Fft::nextPowerOfTwo(
        static_cast<size_t>(std::max(1, config_.sampleRate * config_.frameLengthMs / 1000)));
    spectrumBins_ = fftSize_ / 2 + 1;

    const double nyquist = config_.sampleRate / 2.0;
    double high = config_.highFreqHz > 0 ? std::min(config_.highFreqHz, nyquist) : nyquist;
    double low = std::max(0.0, config_.lowFreqHz);
    if (high <= low) high = nyquist;

    const int m = std::max(2, config_.numFilters);
    const double melLow = hzToMel(low);
    const double melHigh = hzToMel(high);
    const double step = (melHigh - melLow) / static_cast<double>(m + 1);

    std::vector<double> edges(static_cast<size_t>(m) + 2);
    for (int i = 0; i < m + 2; ++i) {
        edges[static_cast<size_t>(i)] = melToHz(melLow + step * static_cast<double>(i));
    }

    const double binHz = static_cast<double>(config_.sampleRate) / static_cast<double>(fftSize_);
    filters_.resize(static_cast<size_t>(m));
    centersHz_.resize(static_cast<size_t>(m));

    for (int i = 0; i < m; ++i) {
        const double left = edges[static_cast<size_t>(i)];
        const double center = edges[static_cast<size_t>(i) + 1];
        const double right = edges[static_cast<size_t>(i) + 2];
        centersHz_[static_cast<size_t>(i)] = center;

        const int binLo = static_cast<int>(std::ceil(left / binHz));
        const int binHi = static_cast<int>(std::floor(right / binHz));
        for (int b = binLo; b <= binHi; ++b) {
            if (b < 0 || b >= spectrumBins_) continue;
            const double f = b * binHz;
            double w = 0.0;
            if (f >= left && f <= center && center > left) {
                w = (f - left) / (center - left);
            } else if (f > center && f <= right && right > center) {
                w = (right - f) / (right - center);
            }
            if (w > 1e-6) {
                filters_[static_cast<size_t>(i)].emplace_back(b, static_cast<float>(w));
            }
        }
        // 退化保护：滤波器落在 bin 之外时绑定到最近 bin
        if (filters_[static_cast<size_t>(i)].empty()) {
            const int b = std::max(0, std::min(spectrumBins_ - 1,
                                              static_cast<int>(std::lround(center / binHz))));
            filters_[static_cast<size_t>(i)].emplace_back(b, 1.0f);
        }
    }
}

std::vector<float> MelFilterBank::apply(const std::vector<float>& powerSpectrum) const {
    std::vector<float> out(filters_.size(), 0.0f);
    for (size_t i = 0; i < filters_.size(); ++i) {
        double acc = 0.0;
        for (const auto& [bin, weight] : filters_[i]) {
            if (static_cast<size_t>(bin) < powerSpectrum.size()) {
                acc += static_cast<double>(powerSpectrum[static_cast<size_t>(bin)]) * weight;
            }
        }
        out[i] = static_cast<float>(acc);
    }
    return out;
}

double MelFilterBank::centerFrequencyHz(size_t index) const {
    return index < centersHz_.size() ? centersHz_[index] : 0.0;
}

MfccExtractor::MfccExtractor(const MfccConfig& config) : config_(config), bank_(config) {
    // fftSize_ 必须与 MelFilterBank 内部保持一致
    fftSize_ = Fft::nextPowerOfTwo(static_cast<size_t>(
        std::max(1, config_.sampleRate * config_.frameLengthMs / 1000)));
    const int len = frameLengthSamples();
    window_.resize(static_cast<size_t>(len));
    for (int n = 0; n < len; ++n) {
        window_[static_cast<size_t>(n)] =
            static_cast<float>(0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                                      static_cast<double>(len - 1)));
    }

    lifter_.resize(static_cast<size_t>(std::max(1, config_.numCoeffs)), 1.0f);
    if (config_.useLifter) {
        for (int i = 0; i < config_.numCoeffs; ++i) {
            lifter_[static_cast<size_t>(i)] = static_cast<float>(
                1.0 + 0.5 * config_.lifterCepstral *
                          std::sin(kPi * static_cast<double>(i) / config_.lifterCepstral));
        }
    }
}

int MfccExtractor::frameLengthSamples() const {
    return config_.sampleRate * config_.frameLengthMs / 1000;
}

int MfccExtractor::frameShiftSamples() const {
    return std::max(1, config_.sampleRate * config_.frameShiftMs / 1000);
}

int MfccExtractor::frameCount(size_t sampleCount) const {
    const int fl = frameLengthSamples();
    const int fs = frameShiftSamples();
    if (static_cast<int>(sampleCount) < fl) return 0;
    return static_cast<int>((sampleCount - static_cast<size_t>(fl)) / static_cast<size_t>(fs)) + 1;
}

std::vector<float> MfccExtractor::computeFrame(const std::vector<float>& frame,
                                               float prevSample) const {
    const int fl = frameLengthSamples();
    std::vector<float> buf(static_cast<size_t>(fftSize_), 0.0f);
    float last = prevSample;
    const int n = std::min<int>(fl, static_cast<int>(frame.size()));
    for (int i = 0; i < n; ++i) {
        const float x = frame[static_cast<size_t>(i)];
        // 预加重
        const float emphasized = x - static_cast<float>(config_.preemphasis) * last;
        last = x;
        buf[static_cast<size_t>(i)] = emphasized * window_[static_cast<size_t>(i)];
    }

    const std::vector<float> power = powerSpectrum(buf);
    const std::vector<float> melEnergies = bank_.apply(power);

    std::vector<float> logMel(melEnergies.size(), 0.0f);
    for (size_t i = 0; i < melEnergies.size(); ++i) {
        logMel[i] = std::log(std::max(kLogFloor, melEnergies[i]));
    }

    // DCT-II
    const int numCoeffs = std::max(1, config_.numCoeffs);
    const int numFilters = static_cast<int>(logMel.size());
    std::vector<float> coeffs(static_cast<size_t>(numCoeffs), 0.0f);
    for (int k = 0; k < numCoeffs; ++k) {
        double acc = 0.0;
        for (int i = 0; i < numFilters; ++i) {
            acc += static_cast<double>(logMel[static_cast<size_t>(i)]) *
                   std::cos(kPi * static_cast<double>(k) * (static_cast<double>(i) + 0.5) /
                            static_cast<double>(numFilters));
        }
        const double norm = std::sqrt(2.0 / static_cast<double>(numFilters));
        coeffs[static_cast<size_t>(k)] =
            static_cast<float>(acc * norm * lifter_[static_cast<size_t>(k)]);
    }
    return coeffs;
}

std::vector<std::vector<float>> MfccExtractor::compute(const AudioBuffer& audio) const {
    std::vector<std::vector<float>> out;
    const int frames = frameCount(audio.samples.size());
    if (frames <= 0) return out;
    out.reserve(static_cast<size_t>(frames));

    const int fl = frameLengthSamples();
    std::vector<float> frame(static_cast<size_t>(fl), 0.0f);

    for (int f = 0; f < frames; ++f) {
        const size_t start = frameStartSample(f);
        for (int i = 0; i < fl; ++i) {
            const size_t idx = start + static_cast<size_t>(i);
            frame[static_cast<size_t>(i)] = idx < audio.samples.size() ? audio.samples[idx] : 0.0f;
        }
        const float prev = (start > 0) ? audio.samples[start - 1] : 0.0f;
        out.push_back(computeFrame(frame, prev));
    }
    return out;
}

std::vector<std::vector<float>> MfccExtractor::delta(
    const std::vector<std::vector<float>>& features, int window) {
    if (features.empty()) return {};
    const int t = static_cast<int>(features.size());
    const int dim = static_cast<int>(features.front().size());
    const int w = std::max(1, window);
    double denom = 0.0;
    for (int n = 1; n <= w; ++n) denom += 2.0 * n * n;
    if (denom <= 0.0) denom = 1.0;

    std::vector<std::vector<float>> out(static_cast<size_t>(t),
                                        std::vector<float>(static_cast<size_t>(dim), 0.0f));
    for (int i = 0; i < t; ++i) {
        for (int n = 1; n <= w; ++n) {
            const int prev = std::max(0, i - n);
            const int next = std::min(t - 1, i + n);
            for (int d = 0; d < dim; ++d) {
                const double diff = static_cast<double>(features[static_cast<size_t>(next)][static_cast<size_t>(d)]) -
                                    static_cast<double>(features[static_cast<size_t>(prev)][static_cast<size_t>(d)]);
                out[static_cast<size_t>(i)][static_cast<size_t>(d)] +=
                    static_cast<float>(diff * n / denom);
            }
        }
    }
    return out;
}

void MfccExtractor::meanNormalize(std::vector<std::vector<float>>& features) {
    if (features.empty()) return;
    const int t = static_cast<int>(features.size());
    const int dim = static_cast<int>(features.front().size());
    for (int d = 0; d < dim; ++d) {
        double sum = 0.0;
        for (int i = 0; i < t; ++i) sum += features[static_cast<size_t>(i)][static_cast<size_t>(d)];
        const float mean = static_cast<float>(sum / t);
        for (int i = 0; i < t; ++i) features[static_cast<size_t>(i)][static_cast<size_t>(d)] -= mean;
    }
}

}  // namespace mm::dsp

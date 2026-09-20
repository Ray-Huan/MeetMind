// MeetMind 测试辅助：合成音频、临时文件路径、断言扩展
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mm/audio/audio_types.h"
#include "testing.h"

namespace mmtest {

constexpr double kPi = 3.14159265358979323846;

/// 确定性伪随机数（xorshift32），保证测试可复现。
class Rng {
public:
    explicit Rng(uint32_t seed = 20260914u) : state_(seed ? seed : 1u) {}
    uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    /// [-1, 1)
    double uniform() { return static_cast<double>(next()) / 2147483648.0 - 1.0; }

private:
    uint32_t state_;
};

inline mm::AudioBuffer makeSilence(int ms, int sampleRate = 16000) {
    mm::AudioBuffer b;
    b.sampleRate = sampleRate;
    b.samples.assign(static_cast<size_t>(sampleRate) * static_cast<size_t>(ms) / 1000u, 0.0f);
    return b;
}

inline void appendSilence(mm::AudioBuffer& b, int ms) {
    b.samples.insert(b.samples.end(),
                     static_cast<size_t>(b.sampleRate) * static_cast<size_t>(ms) / 1000u, 0.0f);
}

inline mm::AudioBuffer makeTone(double freqHz, int ms, double amplitude = 0.3,
                                int sampleRate = 16000) {
    mm::AudioBuffer b;
    b.sampleRate = sampleRate;
    const size_t n = static_cast<size_t>(sampleRate) * static_cast<size_t>(ms) / 1000u;
    b.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        b.samples[i] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * freqHz * static_cast<double>(i) / sampleRate));
    }
    return b;
}

inline mm::AudioBuffer makeNoise(int ms, double amplitude = 0.01, int sampleRate = 16000,
                                 uint32_t seed = 1u) {
    mm::AudioBuffer b;
    b.sampleRate = sampleRate;
    const size_t n = static_cast<size_t>(sampleRate) * static_cast<size_t>(ms) / 1000u;
    b.samples.resize(n);
    Rng rng(seed);
    for (size_t i = 0; i < n; ++i) {
        b.samples[i] = static_cast<float>(amplitude * rng.uniform());
    }
    return b;
}

/// 模拟语音：带共振峰结构的谐波 + 幅度包络 + 轻微噪声。
/// 目的不是"真实语音"，而是提供与真实语音相近的能量/过零率分布，便于 VAD 测试。
inline mm::AudioBuffer makeSpeechLike(int ms, double amplitude = 0.25, int sampleRate = 16000,
                                      uint32_t seed = 7u, double f0 = 120.0) {
    mm::AudioBuffer b;
    b.sampleRate = sampleRate;
    const size_t n = static_cast<size_t>(sampleRate) * static_cast<size_t>(ms) / 1000u;
    b.samples.resize(n);
    Rng rng(seed);
    const double formants[3] = {700.0, 1220.0, 2600.0};
    const double gains[3] = {1.0, 0.55, 0.28};
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        // 基频 + 谐波
        double s = 0.0;
        for (int h = 1; h <= 12; ++h) {
            const double f = f0 * h;
            for (int k = 0; k < 3; ++k) {
                const double bw = 90.0;
                const double g = gains[k] / (1.0 + std::pow((f - formants[k]) / bw, 2.0));
                s += g * std::sin(2.0 * kPi * f * t);
            }
        }
        s /= 8.0;
        // 音节包络（约 4 Hz 调制）
        const double env = 0.55 + 0.45 * std::sin(2.0 * kPi * 4.0 * t);
        b.samples[i] = static_cast<float>(amplitude * env * s + 0.004 * rng.uniform());
    }
    return b;
}

/// 追加一段"语音"（含 5 ms 淡入淡出，避免切换爆音）。
inline void appendSpeechLike(mm::AudioBuffer& b, int ms, double amplitude = 0.25,
                             uint32_t seed = 7u) {
    mm::AudioBuffer s = makeSpeechLike(ms, amplitude, b.sampleRate, seed);
    const size_t fade = static_cast<size_t>(b.sampleRate) / 200;  // 5 ms
    for (size_t i = 0; i < fade && i < s.samples.size(); ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(fade);
        s.samples[i] *= g;
        s.samples[s.samples.size() - 1 - i] *= g;
    }
    b.samples.insert(b.samples.end(), s.samples.begin(), s.samples.end());
}

inline void appendTone(mm::AudioBuffer& b, double freqHz, int ms, double amplitude = 0.3) {
    mm::AudioBuffer t = makeTone(freqHz, ms, amplitude, b.sampleRate);
    b.samples.insert(b.samples.end(), t.samples.begin(), t.samples.end());
}

/// 构造「语音-静音-语音-静音」交替片段。
/// @param speechMs       每段语音时长
/// @param silenceMs      每段静音时长
/// @param segments       语音段数量
/// @param leadSilenceMs  开头静音
/// @param tailSilenceMs  结尾静音
inline mm::AudioBuffer makeAlternatingClip(int speechMs, int silenceMs, int segments,
                                           int leadSilenceMs = 300, int tailSilenceMs = 300,
                                           int sampleRate = 16000, double speechAmp = 0.25) {
    mm::AudioBuffer b;
    b.sampleRate = sampleRate;
    if (leadSilenceMs > 0) appendSilence(b, leadSilenceMs);
    for (int i = 0; i < segments; ++i) {
        appendSpeechLike(b, speechMs, speechAmp, 7u + static_cast<uint32_t>(i));
        if (i + 1 < segments) appendSilence(b, silenceMs);
    }
    if (tailSilenceMs > 0) appendSilence(b, tailSilenceMs);
    return b;
}

/// 测试输出目录下的路径。
inline std::string outPath(const std::string& relative) {
    std::filesystem::path dir(MEETMIND_TEST_OUT_DIR);
    std::error_code ec;
    std::filesystem::create_directories(dir / std::filesystem::path(relative).parent_path(), ec);
    return (dir / relative).string();
}

/// 内置数据文件路径。
inline std::string dataPath(const std::string& relative) {
    return (std::filesystem::path(MEETMIND_TEST_DATA_DIR) / relative).string();
}

}  // namespace mmtest

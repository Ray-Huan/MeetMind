#include "mm/audio/vad.h"

#include <algorithm>
#include <cmath>

#include "mm/common/logger.h"

namespace mm::audio {
namespace {

double percentileOf(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return -100.0;
    if (p <= 0.0) return sorted.front();
    if (p >= 1.0) return sorted.back();
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

}  // namespace

std::vector<VadFrame> VoiceActivityDetector::frameMetrics(const AudioBuffer& audio,
                                                          int frameLengthMs, int frameShiftMs) {
    std::vector<VadFrame> frames;
    if (audio.samples.empty() || audio.sampleRate <= 0) return frames;

    const int fl = std::max(1, audio.sampleRate * frameLengthMs / 1000);
    const int fs = std::max(1, audio.sampleRate * frameShiftMs / 1000);
    const size_t n = audio.samples.size();
    if (n < static_cast<size_t>(fl)) {
        // 过短的音频仍作单帧处理，避免静音全丢
        frames.push_back(VadFrame{0, -100.0f, 0.0f, false});
        return frames;
    }

    const size_t count = (n - static_cast<size_t>(fl)) / static_cast<size_t>(fs) + 1;
    frames.reserve(count);

    for (size_t f = 0; f < count; ++f) {
        const size_t start = f * static_cast<size_t>(fs);
        double sumSq = 0.0;
        int crossings = 0;
        float prev = audio.samples[start];
        for (int i = 0; i < fl; ++i) {
            const float x = audio.samples[start + static_cast<size_t>(i)];
            sumSq += static_cast<double>(x) * x;
            if ((x >= 0.0f) != (prev >= 0.0f)) ++crossings;
            prev = x;
        }
        const double rms = std::sqrt(sumSq / static_cast<double>(fl));
        VadFrame frame;
        frame.startMs = samplesToMs(start, audio.sampleRate);
        frame.rmsDb = static_cast<float>(rms > 0 ? 20.0 * std::log10(rms) : -100.0);
        frame.zcr = static_cast<float>(crossings) / static_cast<float>(fl);
        frames.push_back(frame);
    }
    return frames;
}

VoiceActivityDetector::VoiceActivityDetector(const VadConfig& config) : config_(config) {}

Result<VadResult> VoiceActivityDetector::process(const AudioBuffer& audio) const {
    VadResult result;
    if (audio.samples.empty()) {
        return fail(ErrorCode::InvalidArgument, "音频为空，无法进行语音活动检测");
    }
    if (audio.sampleRate <= 0) {
        return fail(ErrorCode::InvalidArgument, "采样率非法");
    }

    result.frames = frameMetrics(audio, config_.frameLengthMs, config_.frameShiftMs);
    if (result.frames.empty()) {
        return fail(ErrorCode::InvalidArgument, "音频过短，无法分帧");
    }

    // ---- 噪声底与门限 ----
    std::vector<double> levels;
    levels.reserve(result.frames.size());
    for (const VadFrame& f : result.frames) levels.push_back(static_cast<double>(f.rmsDb));
    std::vector<double> sorted = levels;
    std::sort(sorted.begin(), sorted.end());

    result.noiseFloorDb = percentileOf(sorted, config_.noiseFloorPercentile);
    result.speechLevelDb = percentileOf(sorted, 0.90);

    double delta = config_.thresholdDeltaDb;
    if (delta <= 0.0) {
        const double dynamic = std::max(0.0, result.speechLevelDb - result.noiseFloorDb);
        delta = config_.dynamicFactor * dynamic;
        delta = std::max(config_.minDeltaDb, std::min(config_.maxDeltaDb, delta));
    }
    result.thresholdDb = result.noiseFloorDb + delta;

    // ---- 迟滞判决 ----
    const int enter = std::max(1, config_.enterFrames);
    const int exit = std::max(1, config_.exitFrames);
    int consecutiveAbove = 0;
    int consecutiveBelow = 0;
    bool speaking = false;

    for (VadFrame& f : result.frames) {
        const bool above = static_cast<double>(f.rmsDb) > result.thresholdDb;
        // 过零率仅在能量贴边时参与：抑制持续高频噪声（如风扇、电流声）
        const bool zcrOk =
            (static_cast<double>(f.rmsDb) > result.thresholdDb + 3.0) ||
            (static_cast<double>(f.zcr) <= config_.maxZeroCrossingRate);
        const bool candidate = above && zcrOk;

        if (!speaking) {
            if (candidate) {
                ++consecutiveAbove;
                if (consecutiveAbove >= enter) {
                    speaking = true;
                    consecutiveAbove = 0;
                    consecutiveBelow = 0;
                }
            } else {
                consecutiveAbove = 0;
            }
        } else {
            if (!candidate) {
                ++consecutiveBelow;
                if (consecutiveBelow >= exit) {
                    speaking = false;
                    consecutiveBelow = 0;
                    consecutiveAbove = 0;
                }
            } else {
                consecutiveBelow = 0;
            }
        }
        f.voiced = speaking;
    }

    // ---- 段构建 ----
    const int64_t frameShift = config_.frameShiftMs;
    const int64_t frameLen = config_.frameLengthMs;
    std::vector<SpeechSegment> raw;
    int64_t segStart = -1;
    int64_t segEnd = -1;
    double energyAcc = 0.0;
    int energyCount = 0;

    for (size_t i = 0; i < result.frames.size(); ++i) {
        const VadFrame& f = result.frames[i];
        const int64_t fStart = static_cast<int64_t>(i) * frameShift;
        const int64_t fEnd = fStart + frameLen;
        if (f.voiced) {
            if (segStart < 0) {
                segStart = fStart;
                energyAcc = 0.0;
                energyCount = 0;
            }
            segEnd = fEnd;
            energyAcc += f.rmsDb;
            ++energyCount;
        } else if (segStart >= 0) {
            SpeechSegment s;
            s.startMs = segStart;
            s.endMs = segEnd;
            s.energyDb = static_cast<float>(energyCount > 0 ? energyAcc / energyCount : -100.0);
            raw.push_back(s);
            segStart = -1;
        }
    }
    if (segStart >= 0) {
        SpeechSegment s;
        s.startMs = segStart;
        s.endMs = segEnd;
        s.energyDb = static_cast<float>(energyCount > 0 ? energyAcc / energyCount : -100.0);
        raw.push_back(s);
    }

    // ---- 后处理：合并近邻 → 剔除过短 ----
    std::vector<SpeechSegment> merged;
    for (const SpeechSegment& s : raw) {
        if (!merged.empty() && (s.startMs - merged.back().endMs) < config_.minSilenceMs) {
            SpeechSegment& back = merged.back();
            const int64_t d1 = back.durationMs();
            const int64_t d2 = s.durationMs();
            const int64_t total = std::max<int64_t>(1, d1 + d2);
            back.energyDb = static_cast<float>(
                (back.energyDb * static_cast<double>(d1) + s.energyDb * static_cast<double>(d2)) /
                static_cast<double>(total));
            back.endMs = s.endMs;
        } else {
            merged.push_back(s);
        }
    }

    const int64_t totalMs = audio.durationMs();
    int64_t speechMs = 0;
    for (const SpeechSegment& s : merged) {
        if (s.durationMs() < config_.minSpeechMs) continue;
        SpeechSegment clipped = s;
        clipped.startMs = std::max<int64_t>(0, clipped.startMs);
        clipped.endMs = std::min<int64_t>(totalMs, clipped.endMs);
        if (clipped.durationMs() < config_.minSpeechMs) continue;
        result.segments.push_back(clipped);
        speechMs += clipped.durationMs();
    }

    result.speechRatio =
        totalMs > 0 ? static_cast<double>(speechMs) / static_cast<double>(totalMs) : 0.0;

    MM_LOG_INFO("vad") << "帧数=" << result.frames.size() << " 噪声底="
                       << static_cast<int>(result.noiseFloorDb) << "dB 门限="
                       << static_cast<int>(result.thresholdDb) << "dB 语音段="
                       << result.segments.size() << " 语音占比="
                       << static_cast<int>(result.speechRatio * 100.0) << "%";

    if (result.segments.empty()) {
        // 全静音或信噪比极低：退化为「整段作为单一语音段」，保证后续流程不空转
        MM_LOG_WARN("vad") << "未检测到有效语音段，回退为整段处理";
        SpeechSegment whole;
        whole.startMs = 0;
        whole.endMs = totalMs;
        whole.energyDb = static_cast<float>(result.speechLevelDb);
        if (whole.durationMs() >= config_.minSpeechMs) {
            result.segments.push_back(whole);
            result.speechRatio = 1.0;
        }
    }

    return result;
}

}  // namespace mm::audio

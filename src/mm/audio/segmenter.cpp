#include "mm/audio/segmenter.h"

#include <algorithm>
#include <cmath>

#include "mm/common/logger.h"

namespace mm::audio {
namespace {

/// 计算 [start, end) 区间内采样点的 RMS（dBFS）。
double rmsDb(const AudioBuffer& audio, size_t start, size_t end) {
    if (end <= start || start >= audio.samples.size()) return -100.0;
    end = std::min(end, audio.samples.size());
    double sumSq = 0.0;
    for (size_t i = start; i < end; ++i) {
        const double v = audio.samples[i];
        sumSq += v * v;
    }
    const double n = static_cast<double>(end - start);
    const double rms = std::sqrt(sumSq / n);
    return rms > 0 ? 20.0 * std::log10(rms) : -100.0;
}

}  // namespace

std::vector<TranscriptionUnit> groupSegments(const std::vector<SpeechSegment>& segments,
                                             const GroupConfig& config) {
    std::vector<TranscriptionUnit> units;
    if (segments.empty()) return units;

    TranscriptionUnit current;
    current.startMs = segments.front().startMs;
    current.endMs = segments.front().endMs;
    current.members.push_back(0);

    for (size_t i = 1; i < segments.size(); ++i) {
        const SpeechSegment& s = segments[i];
        const int64_t gap = s.startMs - current.endMs;
        const int64_t merged = s.endMs - current.startMs;
        if (gap <= config.mergeGapMs && merged <= config.maxUnitMs) {
            current.endMs = s.endMs;
            current.members.push_back(i);
        } else {
            units.push_back(current);
            current = TranscriptionUnit{};
            current.startMs = s.startMs;
            current.endMs = s.endMs;
            current.members.push_back(i);
        }
    }
    units.push_back(current);

    MM_LOG_DEBUG("segmenter") << "分组: " << segments.size() << " 段 -> " << units.size()
                              << " 个转写单元";
    return units;
}

SpeechSegmenter::SpeechSegmenter(SegmenterConfig config, int sampleRate)
    : config_(config), sampleRate_(sampleRate > 0 ? sampleRate : 16000) {}

size_t SpeechSegmenter::maxSegmentSamples() const {
    return msToSamples(config_.maxSegmentMs, sampleRate_);
}

size_t SpeechSegmenter::findLowestEnergySplit(const AudioBuffer& audio, size_t startSample,
                                              size_t endSample) const {
    // 在中间 60% 区域内以 100 ms 窗滑动，找能量最低处
    const size_t window = msToSamples(100, sampleRate_);
    const size_t lower = startSample + (endSample - startSample) / 5;
    const size_t upper = endSample - (endSample - startSample) / 5;
    if (upper <= lower + window) {
        return startSample + (endSample - startSample) / 2;
    }
    double best = 1e30;
    size_t bestPos = lower + (upper - lower) / 2;
    for (size_t pos = lower; pos + window <= upper; pos += window / 2) {
        const double db = rmsDb(audio, pos, pos + window);
        if (db < best) {
            best = db;
            bestPos = pos + window / 2;
        }
    }
    return bestPos;
}

std::vector<SpeechSegment> SpeechSegmenter::refine(const std::vector<SpeechSegment>& raw,
                                                   const AudioBuffer& audio) const {
    std::vector<SpeechSegment> out;
    const int64_t totalMs = audio.durationMs();
    const size_t totalSamples = audio.samples.size();

    for (const SpeechSegment& s : raw) {
        if (s.durationMs() < config_.minSegmentMs) continue;

        size_t startSample = msToSamples(s.startMs, sampleRate_);
        size_t endSample = std::min(totalSamples, msToSamples(s.endMs, sampleRate_));
        if (endSample <= startSample) continue;

        // 过长切分（递归处理）
        std::vector<std::pair<size_t, size_t>> pieces;
        std::vector<std::pair<size_t, size_t>> stack{{startSample, endSample}};
        const size_t maxSamples = maxSegmentSamples();
        while (!stack.empty()) {
            auto [a, b] = stack.back();
            stack.pop_back();
            if (b - a <= maxSamples || maxSamples == 0) {
                pieces.emplace_back(a, b);
                continue;
            }
            const size_t mid = findLowestEnergySplit(audio, a, b);
            if (mid <= a || mid >= b) {
                pieces.emplace_back(a, b);
                continue;
            }
            stack.emplace_back(a, mid);
            stack.emplace_back(mid, b);
        }
        std::sort(pieces.begin(), pieces.end());

        for (const auto& [a, b] : pieces) {
            SpeechSegment seg;
            seg.startMs = samplesToMs(a, sampleRate_);
            seg.endMs = std::min<int64_t>(totalMs, samplesToMs(b, sampleRate_));
            seg.energyDb = static_cast<float>(rmsDb(audio, a, b));
            seg.speakerId = s.speakerId;
            if (seg.durationMs() < config_.minSegmentMs) continue;
            out.push_back(seg);
        }
    }

    MM_LOG_DEBUG("segmenter") << "refine: " << raw.size() << " -> " << out.size() << " 段";
    return out;
}

AudioBuffer SpeechSegmenter::slice(const AudioBuffer& audio, const SpeechSegment& segment,
                                   int extraPadMs, int64_t* actualStartMsOut) const {
    AudioBuffer out;
    out.sampleRate = audio.sampleRate;
    if (actualStartMsOut) *actualStartMsOut = segment.startMs;

    const int64_t padMs = config_.padMs + std::max(0, extraPadMs);
    int64_t startMs = segment.startMs - padMs;
    int64_t endMs = segment.endMs + padMs;
    if (startMs < 0) startMs = 0;
    const int64_t totalMs = audio.durationMs();
    if (endMs > totalMs) endMs = totalMs;
    if (endMs <= startMs) return out;

    size_t startSample = msToSamples(startMs, audio.sampleRate);
    size_t endSample = std::min(audio.samples.size(), msToSamples(endMs, audio.sampleRate));
    if (endSample <= startSample) return out;

    if (actualStartMsOut) *actualStartMsOut = samplesToMs(startSample, audio.sampleRate);
    out.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(startSample),
                       audio.samples.begin() + static_cast<std::ptrdiff_t>(endSample));
    return out;
}

}  // namespace mm::audio

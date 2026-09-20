#include "mm/live/online_vad.h"

#include <algorithm>
#include <cmath>

namespace mm::live {
namespace {

constexpr double kFloorDb = -100.0;

/// 帧 RMS（dBFS）。静音返回 kFloorDb。
double frameDb(const float* s, int n) {
    if (n <= 0) return kFloorDb;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = static_cast<double>(s[i]);
        sum += v * v;
    }
    const double rms = std::sqrt(sum / static_cast<double>(n));
    return rms > 1e-9 ? 20.0 * std::log10(rms) : kFloorDb;
}

/// 帧过零率。噪声通常过零率高，用于门限附近的二次判决。
double frameZcr(const float* s, int n) {
    if (n < 2) return 0.0;
    int cross = 0;
    for (int i = 1; i < n; ++i) {
        const bool a = s[i - 1] >= 0.0f;
        const bool b = s[i] >= 0.0f;
        if (a != b) ++cross;
    }
    return static_cast<double>(cross) / static_cast<double>(n - 1);
}

}  // namespace

OnlineVad::OnlineVad(const OnlineVadConfig& cfg) : cfg_(cfg) {
    if (cfg_.frameLengthMs <= 0) cfg_.frameLengthMs = 25;
    if (cfg_.frameShiftMs <= 0) cfg_.frameShiftMs = 10;
    if (cfg_.exitFrames <= 0) cfg_.exitFrames = 60;
    if (cfg_.enterFrames <= 0) cfg_.enterFrames = 3;
}

std::vector<SpeechSegment> OnlineVad::feed(const float* samples, size_t count) {
    std::vector<SpeechSegment> out;
    if (samples == nullptr || count == 0) return out;

    buf_.insert(buf_.end(), samples, samples + count);

    const int frameLen = std::max(1, cfg_.sampleRate * cfg_.frameLengthMs / 1000);
    const int shift = std::max(1, cfg_.sampleRate * cfg_.frameShiftMs / 1000);

    while (static_cast<int64_t>(buf_.size() - pos_) >= frameLen) {
        processFrame(buf_.data() + pos_, frameLen, &out);
        pos_ += static_cast<size_t>(shift);
        analyzedSamples_ += shift;
        ++frameIndex_;
    }

    // 定期压缩缓冲，避免长时间运行时内存无界增长
    if (pos_ >= static_cast<size_t>(frameLen) * 2u) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return out;
}

void OnlineVad::processFrame(const float* frame, int len, std::vector<SpeechSegment>* out) {
    const double db = frameDb(frame, len);
    const double zcr = frameZcr(frame, len);
    const int64_t frameStartMs = frameIndex_ * cfg_.frameShiftMs;
    const int64_t frameEndMs = frameStartMs + cfg_.frameLengthMs;

    // 噪声底：瞬时下降、缓慢回升（≈15 dB/s）；峰值：瞬时上升、缓慢衰减（≈5 dB/s）
    if (db < noiseFloorDb_) {
        noiseFloorDb_ = db;
    } else {
        noiseFloorDb_ = std::min(noiseFloorDb_ + 0.15, db);
    }
    if (db > peakDb_) {
        peakDb_ = db;
    } else {
        peakDb_ = std::max(peakDb_ - 0.05, db);
    }

    const double delta = std::clamp(cfg_.dynamicFactor * (peakDb_ - noiseFloorDb_),
                                    cfg_.minDeltaDb, cfg_.maxDeltaDb);
    thresholdDb_ = noiseFloorDb_ + delta;

    bool voiced = db > thresholdDb_;
    // 能量刚好压线且过零率很高 → 视为噪声而非人声（清音/摩擦音通常伴随更高能量）
    if (voiced && (db - thresholdDb_) < 3.0 && zcr > cfg_.maxZeroCrossingRate) {
        voiced = false;
    }

    if (!inSpeech_) {
        if (!voiced) {
            voicedRun_ = 0;
            return;
        }
        if (++voicedRun_ < cfg_.enterFrames) return;

        inSpeech_ = true;
        unvoicedRun_ = 0;
        // 回退到「首个有声帧」的起点，避免丢掉字头
        int64_t start = frameStartMs - static_cast<int64_t>(cfg_.enterFrames - 1) * cfg_.frameShiftMs;
        speechStartMs_ = start > 0 ? start : 0;
        lastVoicedEndMs_ = frameEndMs;
        energySum_ = db;
        energyFrames_ = 1;
        return;
    }

    // 已在语音中
    if (voiced) {
        lastVoicedEndMs_ = frameEndMs;
        energySum_ += db;
        ++energyFrames_;
        unvoicedRun_ = 0;
        return;
    }

    if (++unvoicedRun_ >= cfg_.exitFrames) {
        closeSegment(lastVoicedEndMs_, out);
    }
}

void OnlineVad::closeSegment(int64_t endMs, std::vector<SpeechSegment>* out) {
    const int64_t start = speechStartMs_;
    const double energy = energyFrames_ > 0 ? energySum_ / static_cast<double>(energyFrames_) : kFloorDb;

    inSpeech_ = false;
    speechStartMs_ = -1;
    lastVoicedEndMs_ = -1;
    voicedRun_ = 0;
    unvoicedRun_ = 0;
    energySum_ = 0.0;
    energyFrames_ = 0;

    if (start < 0 || endMs <= start) return;
    if (endMs - start < cfg_.minSpeechMs) return;   // 太短，按噪声丢弃

    SpeechSegment seg;
    seg.startMs = start;
    seg.endMs = endMs;
    seg.energyDb = static_cast<float>(energy);
    if (out != nullptr) out->push_back(seg);
}

bool OnlineVad::forceClose(SpeechSegment* out) {
    if (!inSpeech_) return false;
    // 以「最后一个有声帧的结束」为段尾，避免把已进来的静音尾巴并入
    const int64_t end = lastVoicedEndMs_ > 0 ? lastVoicedEndMs_ : elapsedMs();
    std::vector<SpeechSegment> tmp;
    closeSegment(end, &tmp);
    if (tmp.empty()) return false;
    if (out != nullptr) *out = tmp.front();
    return true;
}

int64_t OnlineVad::currentSpeechMs() const {
    if (!inSpeech_ || speechStartMs_ < 0) return 0;
    const int64_t end = lastVoicedEndMs_ > 0 ? lastVoicedEndMs_ : elapsedMs();
    return end > speechStartMs_ ? end - speechStartMs_ : 0;
}

int64_t OnlineVad::elapsedMs() const {
    if (cfg_.sampleRate <= 0) return 0;
    return analyzedSamples_ * 1000 / cfg_.sampleRate;
}

}  // namespace mm::live

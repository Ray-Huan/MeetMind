#include "mm/live/audio_source.h"

#include <algorithm>
#include <chrono>

#include "mm/audio/resampler.h"
#include "mm/audio/wav_io.h"
#include "mm/common/logger.h"

namespace mm::live {

FileAudioSource::FileAudioSource(std::string path, double speed, int chunkMs)
    : path_(std::move(path)),
      speed_(speed > 0.0 ? speed : 1.0),
      chunkMs_(chunkMs > 0 ? chunkMs : 100) {
    Result<AudioBuffer> decoded = wav::read(path_);
    if (!decoded.ok()) {
        loadError_ = decoded.message();
        return;
    }

    AudioBuffer buffer = std::move(decoded.value());
    sourceSampleRate_ = buffer.sampleRate;

    if (buffer.sampleRate != 16000) {
        Result<AudioBuffer> resampled = audio::Resampler::resample(buffer, 16000, 1);
        if (!resampled.ok()) {
            loadError_ = "重采样失败: " + resampled.message();
            return;
        }
        buffer = std::move(resampled.value());
    }

    samples_ = std::move(buffer.samples);
    sampleRate_ = 16000;
    durationMs_ = sampleRate_ > 0
                      ? static_cast<int64_t>(samples_.size()) * 1000 / sampleRate_
                      : 0;
}

FileAudioSource::~FileAudioSource() { stop(); }

Result<void> FileAudioSource::start(AudioChunkCallback onChunk) {
    if (!loadError_.empty()) {
        return fail(ErrorCode::DecodeFailed, "音频载入失败: " + loadError_);
    }
    if (samples_.empty()) {
        return fail(ErrorCode::InvalidArgument, "音频为空: " + path_);
    }
    if (running_.load(std::memory_order_relaxed)) {
        return fail(ErrorCode::InvalidArgument, "音频源已在运行");
    }

    cb_ = std::move(onChunk);
    stop_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&FileAudioSource::run, this);

    MM_LOG_INFO("live.file") << "开始按 " << speed_ << "× 实时速率推送音频: " << path_
                             << "（时长 " << durationMs_ << " ms）";
    return okStatus();
}

void FileAudioSource::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_relaxed);
}

void FileAudioSource::run() {
    const size_t chunkSamples =
        static_cast<size_t>(sampleRate_) * static_cast<size_t>(chunkMs_) / 1000u;
    const int64_t chunkRealUs =
        static_cast<int64_t>(static_cast<double>(chunkMs_) * 1000.0 / speed_ + 0.5);

    size_t pos = 0;
    auto next = std::chrono::steady_clock::now();

    while (!stop_.load(std::memory_order_relaxed) && pos < samples_.size()) {
        const size_t n = std::min(chunkSamples, samples_.size() - pos);
        if (cb_) cb_(samples_.data() + pos, n);
        pos += n;

        // 按实时速率推进：以「绝对时刻」对齐，避免逐块累积漂移
        next += std::chrono::microseconds(chunkRealUs);
        std::this_thread::sleep_until(next);
    }

    running_.store(false, std::memory_order_relaxed);
    MM_LOG_INFO("live.file") << "音频推送结束";
}

}  // namespace mm::live

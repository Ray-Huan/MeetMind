// MeetMind — 实时音频源抽象
// 上层（实时转写器 / 界面）只依赖 IAudioSource；新增来源（麦克风、网络流、
// 环形缓冲回放）无需改动调用方代码。
// 约定：所有来源统一产出「16 kHz / 单声道 / float32 / [-1,1]」音频块。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "mm/common/result.h"

namespace mm::live {

/// 音频块回调：samples 为 16 kHz 单声道浮点，count 个样本。
/// 回调在采集线程内同步调用，实现方应尽快返回（重活交给后续线程）。
using AudioChunkCallback = std::function<void(const float* samples, size_t count)>;

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    /// 来源标识："file" / "mic"
    virtual std::string id() const = 0;
    virtual int sampleRate() const = 0;

    /// 开始推送音频块；返回后回调线程已在运行。
    virtual Result<void> start(AudioChunkCallback onChunk) = 0;
    /// 停止并回收采集线程；可重复调用。
    virtual void stop() = 0;
    virtual bool running() const = 0;
};

/// 文件音频源：把 WAV 文件按「实时速率 × speed」分块推送。
/// 用途：在没有麦克风的环境（CI / 沙箱）下模拟实时流，使实时链路可测、可回归。
/// speed 也可用来压测：speed=10 表示以 10 倍实时速率灌入。
class FileAudioSource final : public IAudioSource {
public:
    explicit FileAudioSource(std::string path, double speed = 1.0, int chunkMs = 100);
    ~FileAudioSource() override;

    FileAudioSource(const FileAudioSource&) = delete;
    FileAudioSource& operator=(const FileAudioSource&) = delete;

    std::string id() const override { return "file"; }
    int sampleRate() const override { return sampleRate_; }

    Result<void> start(AudioChunkCallback onChunk) override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_relaxed); }

    /// 载入是否成功（构造时即已尝试解码）。
    bool loaded() const { return loadError_.empty(); }
    const std::string& loadError() const { return loadError_; }
    /// 音频总时长（毫秒）。
    int64_t durationMs() const { return durationMs_; }
    /// 实际使用的采样率（内部已重采样到 16 kHz）。
    int sourceSampleRate() const { return sourceSampleRate_; }

private:
    void run();

    std::string path_;
    double speed_ = 1.0;
    int chunkMs_ = 100;
    int sampleRate_ = 16000;
    int sourceSampleRate_ = 0;
    int64_t durationMs_ = 0;
    std::vector<float> samples_;
    std::string loadError_;

    AudioChunkCallback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
};

}  // namespace mm::live

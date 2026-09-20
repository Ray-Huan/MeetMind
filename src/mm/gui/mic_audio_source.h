// MeetMind — 麦克风音频源（Qt Multimedia）
// 实现核心库的 live::IAudioSource，把采集到的音频统一转成
// 「16 kHz / 单声道 / float32 / [-1,1]」后推送给实时转写器。
//
// 采样率说明：本类不做重采样，而是直接向系统请求 16 kHz。
// Windows 的共享模式音频引擎通常会替我们完成采样率转换，因此 16 kHz 一般可直接成功；
// 若设备明确不支持，会返回**清晰错误**，而不是静默送出采样率不符的数据
// （采样率不对会让识别结果整体错乱，这是最难排查的一类问题）。
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <QAudioFormat>
#include <QObject>

#include "mm/live/audio_source.h"

class QAudioSource;
class QIODevice;

namespace mm::gui {

class MicAudioSource final : public QObject, public live::IAudioSource {
    Q_OBJECT

public:
    explicit MicAudioSource(QObject* parent = nullptr);
    ~MicAudioSource() override;

    std::string id() const override { return "mic"; }
    int sampleRate() const override { return 16000; }

    Result<void> start(live::AudioChunkCallback onChunk) override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_relaxed); }

    /// 实际协商到的采集采样率（便于界面显示）。
    int captureRate() const { return captureRate_; }
    const std::string& lastError() const { return lastError_; }

private slots:
    void onReadyRead();

private:
    QAudioSource* input_ = nullptr;
    QIODevice* device_ = nullptr;
    live::AudioChunkCallback cb_;
    QAudioFormat format_;
    int captureRate_ = 0;
    bool floatFormat_ = false;
    std::atomic<bool> running_{false};
    std::string lastError_;
    std::vector<float> pending_;   ///< 不足 100 ms 的残余样本
};

}  // namespace mm::gui

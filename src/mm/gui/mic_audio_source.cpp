#include "mm/gui/mic_audio_source.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

#include "mm/audio/audio_types.h"
#include "mm/common/logger.h"

namespace mm::gui {

namespace {
/// 每次凑够 100 ms 再推给转写器：块太小会增加调度开销，太大则抬高时延。
constexpr int kChunkMs = 100;
constexpr int kSampleRate = 16000;
}  // namespace

MicAudioSource::MicAudioSource(QObject* parent) : QObject(parent) {}
MicAudioSource::~MicAudioSource() { stop(); }

Result<void> MicAudioSource::start(live::AudioChunkCallback onChunk) {
    if (running_.load(std::memory_order_relaxed)) {
        return fail(ErrorCode::InvalidArgument, "麦克风已在采集");
    }

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        lastError_ = "未找到可用的录音设备（请检查系统麦克风与隐私权限）";
        return fail(ErrorCode::InvalidArgument, lastError_);
    }

    // 优先 int16（带宽小），不支持则试 float
    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    floatFormat_ = false;
    if (!device.isFormatSupported(fmt)) {
        fmt.setSampleFormat(QAudioFormat::Float);
        floatFormat_ = true;
        if (!device.isFormatSupported(fmt)) {
            const QAudioFormat preferred = device.preferredFormat();
            lastError_ = "录音设备不支持 16 kHz 单声道（设备首选: " +
                         std::to_string(preferred.sampleRate()) + " Hz / " +
                         std::to_string(preferred.channelCount()) + " 声道）。"
                         "请改用文件模拟模式，或在系统音频设置中把设备设为 16 kHz。";
            return fail(ErrorCode::FormatUnsupported, lastError_);
        }
    }

    captureRate_ = fmt.sampleRate();
    format_ = fmt;
    cb_ = std::move(onChunk);

    input_ = new QAudioSource(device, fmt, this);
    device_ = input_->start();
    if (device_ == nullptr) {
        // Qt6 的 QAudioSource 没有 errorString()，只有 QAudio::Error 枚举
        lastError_ = "麦克风启动失败（QAudio::Error 码 " +
                     std::to_string(static_cast<int>(input_->error())) +
                     "），请检查系统麦克风是否被其它程序占用";
        input_->deleteLater();
        input_ = nullptr;
        return fail(ErrorCode::IoError, lastError_);
    }

    connect(device_, &QIODevice::readyRead, this, &MicAudioSource::onReadyRead);
    running_.store(true, std::memory_order_relaxed);
    lastError_.clear();
    MM_LOG_INFO("live.mic") << "麦克风采集开始: " << device.description().toStdString()
                            << " @" << captureRate_ << " Hz 单声道";
    return okStatus();
}

void MicAudioSource::stop() {
    if (input_ != nullptr) {
        input_->stop();
        input_->deleteLater();
        input_ = nullptr;
    }
    device_ = nullptr;
    running_.store(false, std::memory_order_relaxed);
}

void MicAudioSource::onReadyRead() {
    if (device_ == nullptr || !cb_) return;

    const QByteArray data = device_->readAll();
    if (data.isEmpty()) return;

    // 统一转成 float32 [-1,1]
    const int sampleBytes = floatFormat_ ? 4 : 2;
    const int count = static_cast<int>(data.size()) / sampleBytes;
    if (count <= 0) return;

    pending_.reserve(pending_.size() + static_cast<size_t>(count));
    if (floatFormat_) {
        const float* p = reinterpret_cast<const float*>(data.constData());
        for (int i = 0; i < count; ++i) {
            float v = p[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pending_.push_back(v);
        }
    } else {
        const int16_t* p = reinterpret_cast<const int16_t*>(data.constData());
        for (int i = 0; i < count; ++i) {
            pending_.push_back(static_cast<float>(p[i]) / 32768.0f);
        }
    }

    const size_t chunk = static_cast<size_t>(kSampleRate) * kChunkMs / 1000u;
    size_t offset = 0;
    while (pending_.size() - offset >= chunk) {
        cb_(pending_.data() + offset, chunk);
        offset += chunk;
    }
    if (offset > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

}  // namespace mm::gui

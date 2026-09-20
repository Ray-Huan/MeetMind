// MeetMind — 实时转写面板
// 边进音频边出稿：音频源（麦克风 / 文件模拟实时流）→ 实时转写器 → 逐句追加到界面。
//
// 线程模型：
//   * 工作线程：加载模型、创建并驱动 RealtimeTranscriber（推理在这里跑，不阻塞界面）；
//   * 界面线程：拥有音频源（QAudioSource 需要 Qt 事件循环），把音频喂给转写器；
//   * 两边只通过「互斥保护的 pushFn_ + 待渲染文本队列」交互，界面用定时器轮询，
//     避免跨线程直接操作控件。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QWidget>

#include "mm/common/config.h"
#include "mm/live/audio_source.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace mm::gui {

class RealtimePanel final : public QWidget {
    Q_OBJECT

public:
    explicit RealtimePanel(Config config, QWidget* parent = nullptr);
    ~RealtimePanel() override;

    /// 主窗口打开文件时联动：预填路径并切到文件模式。
    void setFilePath(const QString& path);
    /// 直接以文件模拟实时流启动（供界面自检脚本使用；speed 为模拟倍速）。
    void startWithFile(const QString& path, double speed = 1.0);
    /// 同步推理设备开关（供主窗口工具栏切换 GPU/CPU 时联动）。
    void setUseGpu(bool useGpu);
    bool isRunning() const { return running_; }

signals:
    void logMessage(const QString& text);

private slots:
    void onBrowse();
    void onStart();
    void onStop();
    void onTick();

private:
    void buildUi();
    void appendLine(const QString& line);
    void setRunning(bool running);
    bool micAvailable() const;
    void workerMain(std::string path, bool useMic, bool useGpu);

    Config config_;

    QComboBox* sourceBox_ = nullptr;
    QCheckBox* gpuBox_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* stopBtn_ = nullptr;
    QPlainTextEdit* live_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QLabel* deviceLabel_ = nullptr;
    QTimer* tick_ = nullptr;

    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> engineReady_{false};
    std::atomic<bool> workerDone_{false};

    std::mutex mu_;
    std::function<void(const float*, size_t)> pushFn_;
    std::vector<std::string> pendingLines_;
    std::string statsText_;
    std::string deviceText_;

    std::unique_ptr<live::IAudioSource> source_;
    bool sourceStarted_ = false;
    bool running_ = false;
    int droppedChunks_ = 0;
    double speed_ = 1.0;

    // 与 CLI 保持一致的默认值（判句停顿 / 单段上限）
    static constexpr int kSilenceMs = 600;
    static constexpr int kSegmentMs = 12000;
    static constexpr double kSpeed = 1.0;
};

}  // namespace mm::gui

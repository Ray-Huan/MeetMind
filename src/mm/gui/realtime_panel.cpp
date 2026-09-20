#include "mm/gui/realtime_panel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include "mm/asr/asr_factory.h"
#include "mm/asr/whisper_engine.h"
#include "mm/common/logger.h"
#include "mm/live/realtime_transcriber.h"

#if defined(MM_GUI_HAS_MULTIMEDIA)
#include "mm/gui/mic_audio_source.h"
#endif

namespace mm::gui {

namespace {
QString clockText(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t total = ms / 1000;
    return QString::asprintf("%02lld:%02lld", static_cast<long long>(total / 60),
                             static_cast<long long>(total % 60));
}
}  // namespace

RealtimePanel::RealtimePanel(Config config, QWidget* parent)
    : QWidget(parent), config_(std::move(config)) {
    buildUi();
}

RealtimePanel::~RealtimePanel() {
    stopRequested_.store(true);
    if (source_) source_->stop();
    if (worker_.joinable()) worker_.join();
}

bool RealtimePanel::micAvailable() const {
#if defined(MM_GUI_HAS_MULTIMEDIA)
    return true;
#else
    return false;
#endif
}

void RealtimePanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    // ---- 顶栏：来源选择 ----
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QString::fromUtf8("音频来源:")));

    sourceBox_ = new QComboBox;
    sourceBox_->addItem(QString::fromUtf8("麦克风（实时）"));
    sourceBox_->addItem(QString::fromUtf8("文件（模拟实时流）"));
    if (!micAvailable()) {
        sourceBox_->setItemData(0, QVariant(0), Qt::UserRole - 1);
        sourceBox_->setCurrentIndex(1);
    }
    top->addWidget(sourceBox_);

    pathEdit_ = new QLineEdit;
    pathEdit_->setPlaceholderText(QString::fromUtf8("选择 WAV 文件…"));
    top->addWidget(pathEdit_, 1);

    browseBtn_ = new QPushButton(QString::fromUtf8("浏览"));
    top->addWidget(browseBtn_);

    startBtn_ = new QPushButton(QString::fromUtf8("开始"));
    stopBtn_ = new QPushButton(QString::fromUtf8("停止"));
    stopBtn_->setEnabled(false);
    top->addWidget(startBtn_);
    top->addWidget(stopBtn_);
    root->addLayout(top);

    // GPU 开关：仅在构建含 GPU 后端时默认勾选（实时场景下 GPU 余量更充足）
    auto* gpuRow = new QHBoxLayout;
    gpuBox_ = new QCheckBox(QString::fromUtf8("使用 GPU 加速（实时建议开启）"));
    const bool gpuReady = asr::WhisperCppEngine::gpuBackendAvailable();
    // 与「设置 → 识别」里的开关保持同一口径：都反映配置里的 useGpu。
    // 若这里只看"后端是否可用"，用户在设置里关掉 GPU 后本面板仍会显示勾选，自相矛盾。
    gpuBox_->setChecked(gpuReady && config_.useGpu);
    gpuBox_->setEnabled(gpuReady);
    if (!gpuReady) {
        gpuBox_->setToolTip(QString::fromUtf8(
            "本构建未编入 CUDA 后端。启用方法：bash tools/build-whisper-cuda.sh 后以 "
            "-DMEETMIND_WHISPER_CUDA_DLL=ON 重新构建。"));
    }
    gpuRow->addWidget(gpuBox_);
    gpuRow->addStretch(1);
    root->addLayout(gpuRow);

    deviceLabel_ = new QLabel;
    deviceLabel_->setStyleSheet(QStringLiteral("color:#475569;font-size:12px;"));
    root->addWidget(deviceLabel_);

    statsLabel_ = new QLabel;
    statsLabel_->setStyleSheet(QStringLiteral("color:#64748b;font-size:12px;"));
    statsLabel_->setWordWrap(true);
    root->addWidget(statsLabel_);

    live_ = new QPlainTextEdit;
    live_->setReadOnly(true);
    live_->setStyleSheet(
        QStringLiteral("font-family:'Microsoft YaHei',sans-serif;font-size:14px;"
                       "background:#fafafa;"));
    live_->setPlaceholderText(
        QString::fromUtf8("点击「开始」后，识别结果会边听边逐句出现在这里…"));
    root->addWidget(live_, 1);

    auto* hint = new QLabel(QString::fromUtf8(
        "判句停顿 600 ms · 单段上限 12 s · 文本已做繁转简 / ITN / 标点恢复。"
        "麦克风模式需系统允许应用访问麦克风。"));
    hint->setStyleSheet(QStringLiteral("color:#94a3b8;font-size:11px;"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    connect(browseBtn_, &QPushButton::clicked, this, &RealtimePanel::onBrowse);
    connect(startBtn_, &QPushButton::clicked, this, &RealtimePanel::onStart);
    connect(stopBtn_, &QPushButton::clicked, this, &RealtimePanel::onStop);
    connect(sourceBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool file = index == 1;
        pathEdit_->setEnabled(file);
        browseBtn_->setEnabled(file);
    });
    pathEdit_->setEnabled(false);
    browseBtn_->setEnabled(false);

    tick_ = new QTimer(this);
    tick_->setInterval(100);
    connect(tick_, &QTimer::timeout, this, &RealtimePanel::onTick);
}

void RealtimePanel::setFilePath(const QString& path) {
    if (path.isEmpty()) return;
    pathEdit_->setText(path);
    sourceBox_->setCurrentIndex(1);   // 有文件时默认用文件模式，便于复现
    pathEdit_->setEnabled(true);
    browseBtn_->setEnabled(true);
}

void RealtimePanel::startWithFile(const QString& path, double speed) {
    if (running_) return;
    speed_ = speed > 0.0 ? speed : 1.0;
    setFilePath(path);
    onStart();
}

void RealtimePanel::setUseGpu(bool useGpu) {
    config_.useGpu = useGpu;
    if (gpuBox_ != nullptr) gpuBox_->setChecked(useGpu);
}

void RealtimePanel::appendLine(const QString& line) {
    live_->appendPlainText(line);
    auto* bar = live_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void RealtimePanel::setRunning(bool running) {
    running_ = running;
    startBtn_->setEnabled(!running);
    stopBtn_->setEnabled(running);
    sourceBox_->setEnabled(!running);
    pathEdit_->setEnabled(!running && sourceBox_->currentIndex() == 1);
    browseBtn_->setEnabled(!running && sourceBox_->currentIndex() == 1);
}

void RealtimePanel::onBrowse() {
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("选择音频文件"), QString(),
        QString::fromUtf8("音频文件 (*.wav);;所有文件 (*)"));
    if (!path.isEmpty()) pathEdit_->setText(path);
}

void RealtimePanel::onStart() {
    if (running_) return;

    const bool useMic = sourceBox_->currentIndex() == 0;
    const std::string path = pathEdit_->text().trimmed().toStdString();

    if (useMic && !micAvailable()) {
        appendLine(QString::fromUtf8("本构建未启用 Qt Multimedia，无法使用麦克风。"));
        return;
    }
    if (!useMic && path.empty()) {
        appendLine(QString::fromUtf8("请先选择音频文件（或切换到麦克风模式）。"));
        return;
    }

    // 音频源在界面线程创建：MicAudioSource 依赖 Qt 事件循环
    if (useMic) {
#if defined(MM_GUI_HAS_MULTIMEDIA)
        source_ = std::make_unique<MicAudioSource>(nullptr);
#else
        return;
#endif
    } else {
        auto file = std::make_unique<live::FileAudioSource>(path, speed_, 100);
        if (!file->loaded()) {
            appendLine(QString::fromUtf8("音频载入失败: ") +
                       QString::fromStdString(file->loadError()));
            source_.reset();
            return;
        }
        source_ = std::move(file);
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        pendingLines_.clear();
        statsText_.clear();
        deviceText_.clear();
        pushFn_ = nullptr;
    }
    live_->clear();
    deviceLabel_->clear();
    statsLabel_->clear();
    droppedChunks_ = 0;
    sourceStarted_ = false;
    stopRequested_.store(false);
    engineReady_.store(false);
    workerDone_.store(false);

    setRunning(true);
    appendLine(QString::fromUtf8("正在加载模型…"));
    const bool useGpu = gpuBox_->isChecked();
    gpuBox_->setEnabled(false);
    worker_ = std::thread(&RealtimePanel::workerMain, this, path, useMic, useGpu);
    tick_->start(100);
}

void RealtimePanel::onStop() {
    if (!running_) return;
    stopRequested_.store(true);
    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(false);
    appendLine(QString::fromUtf8("正在收尾（冲刷尾部语音段）…"));
}

void RealtimePanel::workerMain(std::string path, bool useMic, bool useGpu) {
    Config cfg = config_;
    if (!useMic) cfg.inputPath = path;
    cfg.useGpu = useGpu;
    // 与批处理一致：未显式配置时自动探测 models/ 下的模型；
    // 否则后端会静默退化成回放引擎（产出永远是 0 段）。
    if (cfg.modelPath.empty()) {
        const std::string detected = asr::AsrFactory::detectDefaultModel();
        if (!detected.empty()) cfg.modelPath = detected;
    }

    Result<asr::EngineSelection> selection = asr::AsrFactory::create(cfg);
    if (!selection.ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        pendingLines_.push_back("后端初始化失败: " + selection.message());
        workerDone_.store(true);
        return;
    }

    std::string device = std::string("后端: ") + selection->engine->displayName();
    if (auto* w = dynamic_cast<asr::WhisperCppEngine*>(selection->engine.get())) {
        device += w->usingGpu() ? "  ｜ 推理设备: GPU（CUDA）" : "  ｜ 推理设备: CPU";
        if (!w->gpuWarning().empty()) device += "  ｜ " + w->gpuWarning();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        deviceText_ = device;
    }

    live::RealtimeConfig rtCfg;
    rtCfg.vad.sampleRate = 16000;
    rtCfg.vad.exitFrames = std::max(1, kSilenceMs / rtCfg.vad.frameShiftMs);
    rtCfg.maxSegmentMs = kSegmentMs;
    rtCfg.padMs = 200;

    live::RealtimeTranscriber transcriber(selection->engine.get(), rtCfg);
    transcriber.setCallback([this](const live::RealtimeEvent& ev) {
        const QString line =
            QString("[%1] %2    [延迟 %3 ms]")
                .arg(clockText(ev.segment.startMs),
                     QString::fromStdString(ev.segment.text))
                .arg(static_cast<long long>(ev.latencyMs));
        std::lock_guard<std::mutex> lk(mu_);
        pendingLines_.push_back(line.toStdString());
    });

    Result<void> started = transcriber.start();
    if (!started.ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        pendingLines_.push_back("实时转写器启动失败: " + started.message());
        workerDone_.store(true);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        pushFn_ = [&transcriber](const float* samples, size_t count) {
            transcriber.push(samples, count);
        };
    }
    engineReady_.store(true);

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const live::RealtimeStats st = transcriber.stats();
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "已听 %.1f s ｜ 出稿 %lld 段 ｜ 累计推理 %.1f s ｜ 推理实时倍率 %.1fx "
                      "｜ 平均/峰值延迟 %.0f / %.0f ms",
                      st.audioMs / 1000.0, static_cast<long long>(st.segments),
                      st.transcribeMs / 1000.0, st.realTimeFactor, st.avgLatencyMs,
                      st.maxLatencyMs);
        std::lock_guard<std::mutex> lk(mu_);
        statsText_ = buf;
    }

    transcriber.flush();
    transcriber.stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        pushFn_ = nullptr;
    }
    workerDone_.store(true);
}

void RealtimePanel::onTick() {
    // 1) 渲染工作线程产出的文本与统计
    std::vector<std::string> lines;
    std::string stats;
    std::string device;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lines.swap(pendingLines_);
        stats = statsText_;
        device = deviceText_;
    }
    for (const std::string& l : lines) appendLine(QString::fromStdString(l));
    if (!device.empty() && deviceLabel_->text().isEmpty()) {
        deviceLabel_->setText(QString::fromStdString(device));
    }
    if (!stats.empty()) statsLabel_->setText(QString::fromStdString(stats));

    // 2) 引擎就绪后启动音频源（只启动一次）
    if (engineReady_.load() && !sourceStarted_ && source_) {
        Result<void> r = source_->start([this](const float* samples, size_t count) {
            std::function<void(const float*, size_t)> fn;
            {
                std::lock_guard<std::mutex> lk(mu_);
                fn = pushFn_;   // 模型尚未就绪时丢弃，避免把音频塞给未初始化的后端
            }
            if (fn) {
                fn(samples, count);
            } else {
                ++droppedChunks_;
            }
        });
        engineReady_.store(false);
        if (r.ok()) {
            sourceStarted_ = true;
        } else {
            appendLine(QString::fromUtf8("音频源启动失败: ") +
                       QString::fromStdString(r.message()));
            stopRequested_.store(true);
        }
    }

    // 3) 文件模式播完后自动收尾
    if (sourceStarted_ && source_ && !source_->running() && !stopRequested_.load()) {
        appendLine(QString::fromUtf8("音频已播完，正在收尾…"));
        stopRequested_.store(true);
    }

    // 4) 工作线程收尾完成 → 清理界面状态
    if (workerDone_.load()) {
        tick_->stop();
        if (source_) source_->stop();
        source_.reset();
        sourceStarted_ = false;
        workerDone_.store(false);
        setRunning(false);
        gpuBox_->setEnabled(asr::WhisperCppEngine::gpuBackendAvailable());
        if (worker_.joinable()) worker_.join();
        if (droppedChunks_ > 0) {
            appendLine(QString::fromUtf8("[提示] 模型加载期间丢弃了 %1 个音频块（约 %2 秒）")
                           .arg(droppedChunks_)
                           .arg(droppedChunks_ / 10));
        }
        appendLine(QString::fromUtf8("—— 已停止 ——"));
    }
}

}  // namespace mm::gui

#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QThread>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "mm/asr/whisper_engine.h"
#include "mm/common/logger.h"
#include "mm/common/path_utils.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "mm/storage/session_store.h"
#include "realtime_panel.h"
#include "session_worker.h"
#include "settings_dialog.h"
#include "transcript_model.h"
#include "waveform_view.h"

namespace mm::gui {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

/// 转义 HTML 特殊字符
QString esc(const std::string& s) {
    QString t = qs(s);
    t.replace('&', "&amp;");
    t.replace('<', "&lt;");
    t.replace('>', "&gt;");
    return t;
}

/// 为 QTextBrowser 生成纪要 HTML（不含 flex 布局，兼容富文本引擎）
QString minutesHtml(const pipeline::PipelineResult& r) {
    QString html;
    html += QStringLiteral("<html><body style='font-family:\"Microsoft YaHei\",sans-serif;"
                           "font-size:13px;color:#1e293b;'>");
    html += QStringLiteral("<h2 style='margin:0 0 4px'>") + esc(r.title) + QStringLiteral("</h2>");
    html += QStringLiteral("<p style='color:#64748b;margin:0 0 12px'>会议日期 ") +
            esc(r.meetingDate) + QStringLiteral(" · 时长 ") +
            qs(timeutil::formatDuration(r.quality.durationMs)) + QStringLiteral(" · 说话人 ") +
            QString::number(r.minutes.stats.speakerCount) + QStringLiteral(" 人 · 转写 ") +
            QString::number(r.minutes.stats.cjkCharacters) + QStringLiteral(" 字</p>");

    if (!r.minutes.overview.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>会议概览</h3><p>") +
                esc(r.minutes.overview) + QStringLiteral("</p>");
    }

    if (!r.minutes.topics.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>议题脉络</h3><ol>");
        for (const auto& t : r.minutes.topics) {
            html += QStringLiteral("<li><b>") + esc(t.title) + QStringLiteral("</b>");
            if (t.startMs >= 0) {
                html += QStringLiteral(" <span style='color:#94a3b8'>") +
                        qs(timeutil::formatDuration(t.startMs)) + QStringLiteral("</span>");
            }
            html += QStringLiteral("</li>");
        }
        html += QStringLiteral("</ol>");
    }

    if (!r.minutes.keyPoints.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>关键结论</h3><ul>");
        for (const auto& s : r.minutes.keyPoints) {
            html += QStringLiteral("<li>");
            if (s.startMs >= 0) {
                html += QStringLiteral("<span style='color:#94a3b8'>[") +
                        qs(timeutil::formatDuration(s.startMs)) + QStringLiteral("]</span> ");
            }
            html += esc(s.text) + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul>");
    }

    if (!r.minutes.decisions.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>会议决议</h3><ul>");
        for (const auto& d : r.minutes.decisions) {
            html += QStringLiteral("<li><b>[") + esc(d.kind) + QStringLiteral("]</b> ") +
                    esc(d.text) + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul>");
    }

    if (!r.minutes.actionItems.empty()) {
        html += QStringLiteral(
            "<h3 style='color:#2563eb'>待办事项</h3>"
            "<table border='1' cellspacing='0' cellpadding='5' width='100%' "
            "style='border-collapse:collapse;border-color:#e2e8f0'>"
            "<tr style='background:#f1f5f9'><th>#</th><th>任务</th><th>责任人</th>"
            "<th>截止</th><th>优先级</th></tr>");
        int i = 1;
        for (const auto& a : r.minutes.actionItems) {
            QString owner = a.owner.empty() ? QStringLiteral("<span style='color:#b45309'>待指定</span>")
                                            : esc(a.owner);
            QString due = a.dueDate.empty()
                              ? (a.dueText.empty() ? QStringLiteral("待定") : esc(a.dueText))
                              : esc(a.dueDate);
            html += QStringLiteral("<tr><td>") + QString::number(i++) + QStringLiteral("</td><td>") +
                    esc(a.task) + QStringLiteral("</td><td>") + owner + QStringLiteral("</td><td>") +
                    due + QStringLiteral("</td><td>") + esc(a.priority) +
                    QStringLiteral("</td></tr>");
        }
        html += QStringLiteral("</table>");
    }

    if (!r.minutes.keywords.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>关键词</h3><p>");
        for (size_t i = 0; i < r.minutes.keywords.size(); ++i) {
            if (i) html += QStringLiteral(" · ");
            html += esc(r.minutes.keywords[i].word) +
                    QStringLiteral("<span style='color:#94a3b8'>(") +
                    QString::number(r.minutes.keywords[i].frequency) + QStringLiteral(")</span>");
        }
        html += QStringLiteral("</p>");
    }

    if (!r.minutes.risks.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>风险与待确认</h3>");
        for (const std::string& s : r.minutes.risks) {
            html += QStringLiteral("<p style='background:#fff7ed;padding:6px 10px;"
                                   "border-left:3px solid #f59e0b'>") + esc(s) +
                    QStringLiteral("</p>");
        }
    }

    // 处理报告
    html += QStringLiteral("<h3 style='color:#2563eb'>处理报告</h3>"
                           "<table border='1' cellspacing='0' cellpadding='5' width='100%' "
                           "style='border-collapse:collapse;border-color:#e2e8f0'>");
    for (const auto& t : r.report.timings) {
        html += QStringLiteral("<tr><td width='140'>") +
                qs(pipeline::stageLabelCn(t.stage)) + QStringLiteral("</td><td>") +
                QString::number(t.elapsedMs) + QStringLiteral(" ms</td></tr>");
    }
    html += QStringLiteral("<tr><td><b>总计</b></td><td><b>") +
            QString::number(r.report.totalMs) + QStringLiteral(" ms</b></td></tr>");
    html += QStringLiteral("<tr><td>识别后端</td><td>") + esc(r.report.asrBackend) +
            QStringLiteral("（") + esc(r.report.asrBackendReason) + QStringLiteral("）</td></tr>");
    if (r.report.overallRealTimeFactor > 0) {
        html += QStringLiteral("<tr><td>整体实时倍率</td><td>") +
                QString::number(r.report.overallRealTimeFactor, 'f', 2) + QStringLiteral("×</td></tr>");
    }
    html += QStringLiteral("</table>");

    if (!r.exportedFiles.empty()) {
        html += QStringLiteral("<h3 style='color:#2563eb'>已导出文件</h3><ul>");
        for (const std::string& f : r.exportedFiles) {
            html += QStringLiteral("<li>") + esc(f) + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul>");
    }

    html += QStringLiteral("<p style='color:#94a3b8;margin-top:20px'>由 MeetMind 端侧语音转写与"
                           "智能会议纪要工具生成 · 全流程离线运行</p></body></html>");
    return html;
}

}  // namespace

MainWindow::MainWindow(Config config, QWidget* parent)
    : QMainWindow(parent), config_(std::move(config)) {
    qRegisterMetaType<std::shared_ptr<pipeline::PipelineResult>>("SharedResult");
    setWindowTitle(QString::fromUtf8("MeetMind — 端侧语音转写与智能会议纪要"));
    setAcceptDrops(true);
    resize(1280, 820);
    buildUi();
    buildMenus();
    applyConfigToUi();
    syncDeviceUi();
    refreshSessionList();

    // 日志转发到界面
    Logger::instance().setSink([this](LogLevel level, const std::string& line) {
        if (level < LogLevel::Info) return;
        const QString text = QString::fromUtf8(line.c_str());
        QMetaObject::invokeMethod(this, [this, text]() { appendLog(text); },
                                  Qt::QueuedConnection);
    });
}

MainWindow::~MainWindow() {
    Logger::instance().setSink(nullptr);
    if (thread_) {
        if (worker_) worker_->requestCancel();
        thread_->quit();
        thread_->wait(5000);
    }
}

void MainWindow::buildUi() {
    // ---- 左侧：会话历史 ----
    auto* leftPanel = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 8, 4, 8);
    auto* leftTitle = new QLabel(QString::fromUtf8("会话历史"));
    leftTitle->setStyleSheet(QStringLiteral("font-weight:600;color:#334155;"));
    leftLayout->addWidget(leftTitle);
    sessionList_ = new QListWidget;
    sessionList_->setAlternatingRowColors(true);
    leftLayout->addWidget(sessionList_, 1);
    auto* leftHint = new QLabel(QString::fromUtf8("双击会话可查看快照"));
    leftHint->setStyleSheet(QStringLiteral("color:#94a3b8;font-size:11px;"));
    leftHint->setWordWrap(true);
    leftLayout->addWidget(leftHint);

    // ---- 右上：波形 ----
    waveform_ = new WaveformView;

    // ---- 右下：标签页 ----
    tabs_ = new QTabWidget;

    transcriptModel_ = new TranscriptModel(this);
    transcript_ = new QTableView;
    transcript_->setModel(transcriptModel_);
    transcript_->setSelectionBehavior(QAbstractItemView::SelectRows);
    transcript_->setSelectionMode(QAbstractItemView::SingleSelection);
    transcript_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transcript_->setAlternatingRowColors(true);
    transcript_->verticalHeader()->setVisible(false);
    transcript_->horizontalHeader()->setStretchLastSection(false);
    transcript_->horizontalHeader()->setSectionResizeMode(TranscriptModel::ColumnTime,
                                                          QHeaderView::ResizeToContents);
    transcript_->horizontalHeader()->setSectionResizeMode(TranscriptModel::ColumnSpeaker,
                                                          QHeaderView::ResizeToContents);
    transcript_->horizontalHeader()->setSectionResizeMode(TranscriptModel::ColumnText,
                                                          QHeaderView::Stretch);
    transcript_->horizontalHeader()->setSectionResizeMode(TranscriptModel::ColumnConfidence,
                                                          QHeaderView::ResizeToContents);
    tabs_->addTab(transcript_, QString::fromUtf8("转写"));

    minutes_ = new QTextBrowser;
    minutes_->setOpenExternalLinks(true);
    tabs_->addTab(minutes_, QString::fromUtf8("纪要"));

    // 实时转写：边进音频边出稿（麦克风 / 文件模拟实时流）
    realtime_ = new RealtimePanel(config_);
    connect(realtime_, &RealtimePanel::logMessage, this, [this](const QString& text) {
        appendLog(text);
    });
    tabs_->addTab(realtime_, QString::fromUtf8("实时转写"));

    logView_ = new QTextBrowser;
    logView_->setStyleSheet(QStringLiteral("font-family:Consolas,monospace;font-size:11px;"));
    tabs_->addTab(logView_, QString::fromUtf8("运行日志"));

    auto* rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->addWidget(waveform_);
    rightSplitter->addWidget(tabs_);
    rightSplitter->setStretchFactor(0, 0);
    rightSplitter->setStretchFactor(1, 1);
    rightSplitter->setSizes({190, 520});

    auto* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({230, 1050});

    setCentralWidget(mainSplitter);

    // ---- 状态栏 ----
    statusLabel_ = new QLabel(QString::fromUtf8("就绪 · 拖入 WAV 文件开始"));
    summaryLabel_ = new QLabel;
    summaryLabel_->setStyleSheet(QStringLiteral("color:#64748b;"));
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedWidth(220);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(summaryLabel_);
    statusBar()->addPermanentWidget(progress_);

    connect(transcript_, &QTableView::clicked, this, &MainWindow::onTranscriptClicked);
    connect(waveform_, &WaveformView::seekRequested, this, &MainWindow::onWaveformSeek);
    connect(sessionList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { onSessionActivated(); });
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QString::fromUtf8("文件"));
    actOpen_ = fileMenu->addAction(QString::fromUtf8("打开音频…"), this,
                                   &MainWindow::onOpenAudio);
    actOpen_->setShortcut(QKeySequence::Open);
    actExport_ = fileMenu->addAction(QString::fromUtf8("导出结果…"), this,
                                     &MainWindow::onExport);
    actExport_->setEnabled(false);
    fileMenu->addSeparator();
    QAction* actOpenDir = fileMenu->addAction(QString::fromUtf8("打开导出目录"));
    connect(actOpenDir, &QAction::triggered, this, [this]() {
        std::string dir = config_.outputDir;
        if (dir.empty() && !config_.inputPath.empty()) {
            dir = pathutil::parentPath(config_.inputPath);
        }
        if (!dir.empty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
        }
    });
    fileMenu->addSeparator();
    QAction* actQuit = fileMenu->addAction(QString::fromUtf8("退出"));
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    QMenu* processMenu = menuBar()->addMenu(QString::fromUtf8("处理"));
    actStart_ = processMenu->addAction(QString::fromUtf8("开始处理"), this,
                                       &MainWindow::onStartProcess);
    actStart_->setShortcut(Qt::Key_F5);
    actCancel_ = processMenu->addAction(QString::fromUtf8("取消"), this,
                                        &MainWindow::onCancelProcess);
    actCancel_->setEnabled(false);
    processMenu->addSeparator();
    actSettings_ = processMenu->addAction(QString::fromUtf8("设置…"), this,
                                          &MainWindow::onSettings);

    QMenu* helpMenu = menuBar()->addMenu(QString::fromUtf8("帮助"));
    helpMenu->addAction(QString::fromUtf8("关于 MeetMind"), this, &MainWindow::onAbout);

    auto* toolbar = addToolBar(QString::fromUtf8("主工具栏"));
    toolbar->setMovable(false);
    toolbar->addAction(actOpen_);
    toolbar->addAction(actStart_);
    toolbar->addAction(actCancel_);
    toolbar->addSeparator();
    toolbar->addAction(actSettings_);

    // 首页直接切换推理设备：GPU（默认）/ CPU，与设置页、实时面板同一口径。
    // 无 CUDA 后端的构建里 GPU 项置灰，保证「选了却用不上」能被看见。
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addWidget(new QLabel(QString::fromUtf8("推理设备:")));
    deviceBox_ = new QComboBox;
    deviceBox_->addItem(QString::fromUtf8("GPU（默认）"));
    deviceBox_->addItem(QString::fromUtf8("CPU"));
    const bool gpuReady = asr::WhisperCppEngine::gpuBackendAvailable();
    if (!gpuReady) {
        deviceBox_->setItemData(0, false, Qt::UserRole - 1);   // 禁用 GPU 项
        deviceBox_->setToolTip(QString::fromUtf8(
            "本构建未编入 CUDA 后端。启用方法：tools/build-whisper-cuda.sh 后用 "
            "tools/build.sh gpu 重建。"));
    }
    toolbar->addWidget(deviceBox_);
    connect(deviceBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                config_.useGpu = (index == 0);
                syncDeviceUi();
            });
}

void MainWindow::applyConfigToUi() {
    Logger::instance().setLevel(config_.logLevel);
}

void MainWindow::syncDeviceUi() {
    if (deviceBox_ == nullptr) return;
    const bool gpuReady = asr::WhisperCppEngine::gpuBackendAvailable();
    {
        QSignalBlocker block(deviceBox_);   // 避免 setCurrentIndex 触发信号回环
        deviceBox_->setCurrentIndex(config_.useGpu && gpuReady ? 0 : 1);
    }
    if (realtime_ != nullptr) realtime_->setUseGpu(config_.useGpu);
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    actStart_->setEnabled(!busy && !config_.inputPath.empty());
    actOpen_->setEnabled(!busy);
    actSettings_->setEnabled(!busy);
    actCancel_->setEnabled(busy);
    actExport_->setEnabled(!busy && result_ != nullptr);
}

void MainWindow::appendLog(const QString& text) {
    if (!logView_) return;
    logView_->append(text);
}

void MainWindow::refreshSessionList() {
    sessionList_->clear();
    std::string root = config_.outputDir;
    if (root.empty() && !config_.inputPath.empty()) {
        root = pathutil::join(pathutil::parentPath(config_.inputPath), "sessions");
    }
    storage::SessionStore store(root);
    const auto sessions = store.list();
    for (const auto& s : sessions) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2 · %3")
                .arg(qs(s.title.empty() ? s.id : s.title))
                .arg(qs(s.createdAt))
                .arg(qs(timeutil::formatDuration(s.durationMs))));
        item->setData(Qt::UserRole, qs(s.id));
        item->setToolTip(QString::fromUtf8("关键词：%1\n文件：%2")
                             .arg(qs(mm::str::join(s.keywords, "、")))
                             .arg(qs(s.inputPath)));
        sessionList_->addItem(item);
    }
    if (sessions.empty()) {
        auto* item = new QListWidgetItem(QString::fromUtf8("（暂无历史记录）"));
        item->setFlags(Qt::NoItemFlags);
        sessionList_->addItem(item);
    }
}

void MainWindow::onOpenAudio() {
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("选择会议音频"), QString(),
        QString::fromUtf8("WAV 音频 (*.wav);;所有文件 (*)"));
    if (path.isEmpty()) return;
    config_.inputPath = path.toStdString();
    clearResult();
    waveform_->loadAudio(path);
    summaryLabel_->setText(QString::fromUtf8("已载入 %1").arg(QFileInfo(path).fileName()));
    statusLabel_->setText(QString::fromUtf8("已载入音频，点击「开始处理」"));
    setBusy(false);
}

void MainWindow::selectTab(int index) {
    if (tabs_ == nullptr) return;
    if (index < 0 || index >= tabs_->count()) return;
    tabs_->setCurrentIndex(index);
}

int MainWindow::tabCount() const { return tabs_ == nullptr ? 0 : tabs_->count(); }

void MainWindow::startRealtime(const QString& path, double speed) {
    if (realtime_ == nullptr) return;
    selectTab(2);   // 实时转写
    realtime_->startWithFile(path, speed);
}

void MainWindow::openAndProcess(const QString& path) {
    config_.inputPath = path.toStdString();
    waveform_->loadAudio(path);
    if (realtime_ != nullptr) realtime_->setFilePath(path);   // 同步到实时面板，便于复现
    onStartProcess();
}

void MainWindow::onStartProcess() {
    if (busy_) return;
    if (config_.inputPath.empty()) {
        QMessageBox::information(this, QString::fromUtf8("提示"),
                                 QString::fromUtf8("请先打开一个 WAV 音频文件。"));
        return;
    }
    if (config_.modelPath.empty()) {
        const std::string detected = asr::AsrFactory::detectDefaultModel();
        if (!detected.empty()) config_.modelPath = detected;
    }

    // 校验模型可用性，避免用户长时间等待后才失败
    std::string reason;
    const AsrBackend resolved = asr::AsrFactory::resolveBackend(config_, &reason);
    if (resolved == AsrBackend::Replay && config_.backend == AsrBackend::Auto) {
        QMessageBox::information(
            this, QString::fromUtf8("提示"),
            QString::fromUtf8("未配置可用的 whisper 模型，将以「回放引擎」运行。\n\n"
                              "该模式不会真正识别语音，仅用于验证流程与回归测试。\n"
                              "如需真实转写，请在「设置」中指定 ggml 模型文件。\n\n原因：%1")
                .arg(qs(reason)));
    }

    clearResult();
    setBusy(true);
    progress_->setValue(0);
    statusLabel_->setText(QString::fromUtf8("准备中…"));
    appendLog(QString::fromUtf8("[GUI] 开始处理：%1").arg(qs(config_.inputPath)));

    thread_ = new QThread(this);
    worker_ = new PipelineWorker(config_);
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started, worker_, &PipelineWorker::process);
    connect(worker_, &PipelineWorker::progressChanged, this, &MainWindow::onWorkerProgress);
    connect(worker_, &PipelineWorker::finished, this, &MainWindow::onWorkerFinished);
    connect(worker_, &PipelineWorker::failed, this, &MainWindow::onWorkerFailed);
    connect(worker_, &PipelineWorker::finished, thread_, &QThread::quit);
    connect(worker_, &PipelineWorker::failed, thread_, &QThread::quit);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    thread_->start();
}

void MainWindow::onCancelProcess() {
    if (!busy_ || !worker_) return;
    statusLabel_->setText(QString::fromUtf8("正在取消…"));
    worker_->requestCancel();
}

void MainWindow::onWorkerProgress(int stageIndex, int stageCount, int percent,
                                  const QString& message) {
    progress_->setValue(percent);
    statusLabel_->setText(QString::fromUtf8("阶段 %1/%2 · %3")
                              .arg(stageIndex + 1)
                              .arg(stageCount)
                              .arg(message));
}

void MainWindow::onWorkerFinished(const std::shared_ptr<pipeline::PipelineResult>& result) {
    if (thread_) {
        thread_->wait(3000);
        thread_ = nullptr;
    }
    worker_ = nullptr;
    setBusy(false);
    showResult(result);
    progress_->setValue(100);
    statusLabel_->setText(QString::fromUtf8("处理完成"));
    refreshSessionList();
}

void MainWindow::onWorkerFailed(const QString& message, int errorCode) {
    if (thread_) {
        thread_->wait(3000);
        thread_ = nullptr;
    }
    worker_ = nullptr;
    setBusy(false);
    progress_->setValue(0);
    statusLabel_->setText(QString::fromUtf8("处理失败"));
    appendLog(QString::fromUtf8("[GUI] 失败(%1)：%2").arg(errorCode).arg(message));
    QMessageBox::warning(this, QString::fromUtf8("处理失败"), message);
}

void MainWindow::showResult(const std::shared_ptr<pipeline::PipelineResult>& result) {
    result_ = result;
    transcriptModel_->setResult(result);
    minutes_->setHtml(minutesHtml(*result));
    waveform_->setResult(result);
    actExport_->setEnabled(true);

    summaryLabel_->setText(QString::fromUtf8("说话人 %1 · 关键词 %2 · 待办 %3 · 决策 %4")
                               .arg(result->minutes.stats.speakerCount)
                               .arg(result->minutes.keywords.size())
                               .arg(result->minutes.actionItems.size())
                               .arg(result->minutes.decisions.size()));
    tabs_->setCurrentIndex(1);

    if (!result->exportedFiles.empty()) {
        appendLog(QString::fromUtf8("[GUI] 已导出 %1 个文件").arg(result->exportedFiles.size()));
    }
}

void MainWindow::clearResult() {
    result_.reset();
    transcriptModel_->clear();
    minutes_->clear();
    waveform_->setResult(nullptr);
    actExport_->setEnabled(false);
    summaryLabel_->clear();
}

void MainWindow::onTranscriptClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    const int64_t ms = transcriptModel_->startMsAt(index.row());
    if (ms >= 0) waveform_->setCursorMs(ms);
}

void MainWindow::onWaveformSeek(int64_t ms) {
    if (!result_) return;
    // 定位到包含该时间点的转写行
    const auto& segs = result_->asr.segments;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (ms >= segs[i].startMs && ms <= segs[i].endMs) {
            const QModelIndex idx = transcriptModel_->index(static_cast<int>(i),
                                                            TranscriptModel::ColumnText);
            transcript_->selectRow(static_cast<int>(i));
            transcript_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}

void MainWindow::onSessionActivated() {
    QListWidgetItem* item = sessionList_->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;

    std::string root = config_.outputDir;
    if (root.empty() && !config_.inputPath.empty()) {
        root = pathutil::join(pathutil::parentPath(config_.inputPath), "sessions");
    }
    storage::SessionStore store(root);
    Result<Json> snapshot = store.load(id.toStdString());
    if (!snapshot.ok()) {
        QMessageBox::warning(this, QString::fromUtf8("载入失败"), qs(snapshot.message()));
        return;
    }
    const Json& j = snapshot.value();
    QString html = QString::fromUtf8("<p style='color:#64748b'>历史会话快照：%1</p>")
                       .arg(qs(j.getString("title")));
    html += QString::fromUtf8("<h3>%1</h3>").arg(qs(j.getString("title")));
    html += QString::fromUtf8("<p>会议日期：%1<br/>识别后端：%2</p>")
                .arg(qs(j.getString("meetingDate")))
                .arg(qs(j.get("report").getString("asrBackend")));
    const Json& minutes = j.get("minutes");
    html += QString::fromUtf8("<h4>概览</h4><p>%1</p>").arg(esc(minutes.getString("overview")));
    const Json& kws = minutes.get("keywords");
    if (kws.size() > 0) {
        QStringList words;
        for (size_t i = 0; i < kws.size(); ++i) words << qs(kws.at(i).getString("word"));
        html += QString::fromUtf8("<h4>关键词</h4><p>%1</p>").arg(words.join(QStringLiteral(" · ")));
    }
    const Json& actions = minutes.get("actionItems");
    if (actions.size() > 0) {
        html += QString::fromUtf8("<h4>待办</h4><ul>");
        for (size_t i = 0; i < actions.size(); ++i) {
            html += QString::fromUtf8("<li>%1（%2）</li>")
                        .arg(esc(actions.at(i).getString("task")))
                        .arg(esc(actions.at(i).getString("owner").empty()
                                     ? std::string("待指定")
                                     : actions.at(i).getString("owner")));
        }
        html += QString::fromUtf8("</ul>");
    }
    html += QString::fromUtf8("<p style='color:#94a3b8'>完整数据见 %1.json</p>").arg(id);
    minutes_->setHtml(html);
    tabs_->setCurrentIndex(1);
    statusLabel_->setText(QString::fromUtf8("已载入历史会话 %1").arg(id));
}

void MainWindow::onExport() {
    if (!result_) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选择导出目录"),
        qs(config_.outputDir.empty() ? std::string(".") : config_.outputDir));
    if (dir.isEmpty()) return;

    pipeline::ExportOptions opts;
    opts.outputDir = dir.toStdString();
    Result<std::vector<std::string>> files =
        pipeline::Exporter::exportAll(*result_, config_, opts);
    if (!files.ok()) {
        QMessageBox::warning(this, QString::fromUtf8("导出失败"), qs(files.message()));
        return;
    }
    QStringList list;
    for (const std::string& f : files.value()) list << qs(f);
    appendLog(QString::fromUtf8("[GUI] 导出到 %1").arg(dir));
    QMessageBox::information(this, QString::fromUtf8("导出完成"),
                             QString::fromUtf8("已导出 %1 个文件：\n\n%2")
                                 .arg(list.size())
                                 .arg(list.join(QStringLiteral("\n"))));
}

void MainWindow::onSettings() {
    SettingsDialog dlg(config_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    config_ = dlg.result();
    Logger::instance().setLevel(config_.logLevel);
    syncDeviceUi();   // 设置里的 GPU 开关变化要同步到首页下拉框与实时面板
    if (!config_.outputDir.empty()) refreshSessionList();
}

void MainWindow::onAbout() {
    const bool whisper = asr::WhisperCppEngine::compiledIn();
    QMessageBox::about(
        this, QString::fromUtf8("关于 MeetMind"),
        QString::fromUtf8(
            "<h3>MeetMind %1</h3>"
            "<p>端侧语音转写与智能会议纪要工具</p>"
            "<p>全部处理在本机完成，不产生任何网络请求。</p>"
            "<p><b>流水线：</b>音频解码 → 重采样 → 语音检测 → 转写 → 说话人分离 → "
            "文本规范化 → 纪要生成 → 多格式导出</p>"
            "<p><b>识别后端：</b>%2</p>"
            "<p style='color:#64748b'>C++17 · Qt6 · 零第三方运行时依赖</p>")
            .arg(QString::fromUtf8(MEETMIND_VERSION))
            .arg(whisper ? QString::fromUtf8("whisper.cpp（端侧推理已启用）")
                         : QString::fromUtf8("回放引擎（本构建未编译 whisper.cpp）")));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.toLocalFile().endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (!path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) continue;
        config_.inputPath = path.toStdString();
        clearResult();
        waveform_->loadAudio(path);
        statusLabel_->setText(QString::fromUtf8("已载入 %1，按 F5 或点击「开始处理」")
                                  .arg(QFileInfo(path).fileName()));
        summaryLabel_->setText(QFileInfo(path).fileName());
        setBusy(false);
        event->acceptProposedAction();
        return;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (busy_) {
        const auto ret = QMessageBox::question(
            this, QString::fromUtf8("确认退出"),
            QString::fromUtf8("处理尚未完成，确定要退出吗？"));
        if (ret != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        if (worker_) worker_->requestCancel();
    }
    event->accept();
}

}  // namespace mm::gui

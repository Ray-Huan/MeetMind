// MeetMind — 主窗口
#pragma once

#include <QMainWindow>
#include <memory>

#include "mm/common/config.h"
#include "mm/pipeline/pipeline_types.h"

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QTabWidget;
class QTableView;
class QTextBrowser;
class QThread;

namespace mm::gui {

class ParticleOverlay;
class PipelineWorker;
class RealtimePanel;
class TranscriptModel;
class WaveformView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Config config, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 载入音频并立即开始处理（供命令行传参使用）。
    void openAndProcess(const QString& path);

    /// 切换到指定标签页（供界面自检截图使用）。
    void selectTab(int index);

    /// 直接以文件模拟实时流启动实时转写（供界面自检使用）。
    void startRealtime(const QString& path, double speed);

    /// 标签页数量（自检脚本校验用）。
    int tabCount() const;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onOpenAudio();
    void onStartProcess();
    void onCancelProcess();
    void onExport();
    void onOpenReview();
    void onSettings();
    void onAbout();
    void onWorkerProgress(int stageIndex, int stageCount, int percent, const QString& message);
    void onWorkerFinished(const std::shared_ptr<pipeline::PipelineResult>& result);
    void onWorkerFailed(const QString& message, int errorCode);
    void onTranscriptClicked(const QModelIndex& index);
    void onWaveformSeek(int64_t ms);
    void onSessionActivated();

private:
    void buildUi();
    void buildMenus();
    void applyConfigToUi();
    /// 把 config_.useGpu 同步到工具栏下拉框与实时面板（三者口径统一）。
    void syncDeviceUi();
    void setBusy(bool busy);
    void refreshSessionList();
    void showResult(const std::shared_ptr<pipeline::PipelineResult>& result);
    void clearResult();
    void appendLog(const QString& text);

    Config config_;
    std::shared_ptr<pipeline::PipelineResult> result_;

    WaveformView* waveform_ = nullptr;
    QTableView* transcript_ = nullptr;
    TranscriptModel* transcriptModel_ = nullptr;
    QTextBrowser* minutes_ = nullptr;
    QTextBrowser* logView_ = nullptr;
    RealtimePanel* realtime_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QListWidget* sessionList_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;

    QAction* actOpen_ = nullptr;
    QAction* actStart_ = nullptr;
    QAction* actCancel_ = nullptr;
    QAction* actExport_ = nullptr;
    QAction* actReview_ = nullptr;   ///< 打开审核网页（处理完成后可用）
    QAction* actSettings_ = nullptr;
    QComboBox* deviceBox_ = nullptr;   ///< 首页工具栏「推理设备」选择（GPU / CPU）
    ParticleOverlay* particles_ = nullptr;  ///< 粒子特效覆盖层（二次元装饰）

    QThread* thread_ = nullptr;
    PipelineWorker* worker_ = nullptr;
    bool busy_ = false;
};

}  // namespace mm::gui

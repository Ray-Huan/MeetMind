#include "settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "mm/asr/whisper_engine.h"
#include "mm/common/logger.h"

namespace mm::gui {

SettingsDialog::SettingsDialog(const Config& config, QWidget* parent)
    : QDialog(parent), config_(config) {
    setWindowTitle(QString::fromUtf8("设置"));
    setMinimumWidth(600);

    auto* tabs = new QTabWidget(this);

    // ---------------- 识别 ----------------
    {
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);

        backend_ = new QComboBox;
        backend_->addItem(QString::fromUtf8("自动（推荐）"), static_cast<int>(AsrBackend::Auto));
        backend_->addItem(QString::fromUtf8("whisper.cpp（端侧推理）"),
                          static_cast<int>(AsrBackend::Whisper));
        backend_->addItem(QString::fromUtf8("回放引擎（脚本驱动）"),
                          static_cast<int>(AsrBackend::Replay));
        backend_->addItem(QString::fromUtf8("空引擎（跳过识别）"),
                          static_cast<int>(AsrBackend::Null));
        const int idx = backend_->findData(static_cast<int>(config.backend));
        backend_->setCurrentIndex(idx >= 0 ? idx : 0);
        form->addRow(QString::fromUtf8("识别后端"), backend_);

        auto* modelRow = new QWidget;
        auto* modelLayout = new QHBoxLayout(modelRow);
        modelLayout->setContentsMargins(0, 0, 0, 0);
        modelPath_ = new QLineEdit(QString::fromStdString(config.modelPath));
        modelPath_->setPlaceholderText(QString::fromUtf8("ggml 模型文件路径（如 models/ggml-small.bin）"));
        auto* browse = new QPushButton(QString::fromUtf8("浏览…"));
        modelLayout->addWidget(modelPath_, 1);
        modelLayout->addWidget(browse);
        form->addRow(QString::fromUtf8("模型文件"), modelRow);
        connect(browse, &QPushButton::clicked, this, &SettingsDialog::browseModel);

        language_ = new QComboBox;
        language_->addItem(QString::fromUtf8("自动检测"), QStringLiteral("auto"));
        language_->addItem(QString::fromUtf8("中文"), QStringLiteral("zh"));
        language_->addItem(QString::fromUtf8("英文"), QStringLiteral("en"));
        const int li = language_->findData(QString::fromStdString(config.language));
        language_->setCurrentIndex(li >= 0 ? li : 0);
        form->addRow(QString::fromUtf8("语言"), language_);

        threads_ = new QSpinBox;
        threads_->setRange(0, 64);
        threads_->setSpecialValueText(QString::fromUtf8("自动"));
        threads_->setValue(config.threads);
        form->addRow(QString::fromUtf8("推理线程数"), threads_);

        // GPU 加速：是否可用取决于「构建时有没有编入 CUDA 后端」，这里如实反映。
        gpu_ = new QCheckBox(QString::fromUtf8("使用 GPU 推理（CUDA）"));
        {
            const bool gpuReady = asr::WhisperCppEngine::gpuBackendAvailable();
            gpu_->setChecked(gpuReady && config.useGpu);
            gpu_->setEnabled(gpuReady);
            if (gpuReady) {
                gpu_->setToolTip(QString::fromUtf8(
                    "使用 NVIDIA CUDA 推理。实测 whisper small 约 40× 实时（比 CPU 快约 16 倍）；"
                    "结果与 CPU 一致，仅个别同义词因浮点精度略有差异。"));
            } else {
                gpu_->setToolTip(QString::fromUtf8(
                    "本构建未编入 CUDA 后端，GPU 推理不可用。\n"
                    "启用方法：先运行 tools/build-whisper-cuda.sh，再用 tools/build.sh gpu 重建。"));
            }
        }
        form->addRow(gpu_);

        if (!asr::WhisperCppEngine::compiledIn()) {
            auto* warn = new QLabel(QString::fromUtf8(
                "当前构建未包含 whisper.cpp，选择「whisper」后端将直接报错；"
                "使用 -DMEETMIND_WITH_WHISPER=ON 重新构建后可用。"));
            warn->setWordWrap(true);
            warn->setStyleSheet(QStringLiteral("color:#b45309;"));
            form->addRow(warn);
        } else {
            auto* tip = new QLabel(QString::fromUtf8(
                "端侧推理完全离线运行，模型文件不会被上传。建议使用 ggml-small 以兼顾中文准确率与速度。"));
            tip->setWordWrap(true);
            tip->setStyleSheet(QStringLiteral("color:#64748b;"));
            form->addRow(tip);
        }

        tabs->addTab(page, QString::fromUtf8("识别"));
    }

    // ---------------- 处理 ----------------
    {
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);

        diarization_ = new QCheckBox(QString::fromUtf8("启用说话人分离"));
        diarization_->setChecked(config.enableDiarization);
        form->addRow(diarization_);

        autoSpeakers_ = new QCheckBox(QString::fromUtf8("自动估计说话人数"));
        autoSpeakers_->setChecked(config.speakerMode != SpeakerMode::Fixed);
        form->addRow(autoSpeakers_);

        speakers_ = new QSpinBox;
        speakers_->setRange(1, 8);
        speakers_->setValue(config.speakerCount > 0 ? config.speakerCount : 2);
        speakers_->setEnabled(!autoSpeakers_->isChecked());
        form->addRow(QString::fromUtf8("说话人数"), speakers_);
        connect(autoSpeakers_, &QCheckBox::toggled, speakers_,
                [this](bool on) { speakers_->setEnabled(!on); });

        mergeThreshold_ = new QDoubleSpinBox;
        mergeThreshold_->setRange(0.05, 0.90);
        mergeThreshold_->setSingleStep(0.05);
        mergeThreshold_->setDecimals(2);
        mergeThreshold_->setValue(config.diarMergeThreshold);
        form->addRow(QString::fromUtf8("声纹合并阈值"), mergeThreshold_);
        auto* hint = new QLabel(QString::fromUtf8(
            "阈值越小越容易判为同一人；自动估计不准确时可手动指定说话人数。"));
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#64748b;"));
        form->addRow(hint);

        minSpeech_ = new QSpinBox;
        minSpeech_->setRange(50, 3000);
        minSpeech_->setSingleStep(50);
        minSpeech_->setSuffix(QStringLiteral(" ms"));
        minSpeech_->setValue(config.minSpeechMs);
        form->addRow(QString::fromUtf8("最短语音段"), minSpeech_);

        maxSegment_ = new QSpinBox;
        maxSegment_->setRange(2000, 120000);
        maxSegment_->setSingleStep(1000);
        maxSegment_->setSuffix(QStringLiteral(" ms"));
        maxSegment_->setValue(config.maxSegmentMs);
        form->addRow(QString::fromUtf8("单段转写上限"), maxSegment_);

        tabs->addTab(page, QString::fromUtf8("处理"));
    }

    // ---------------- 纪要 ----------------
    {
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);

        summarization_ = new QCheckBox(QString::fromUtf8("生成摘要与关键词"));
        summarization_->setChecked(config.enableSummarization);
        form->addRow(summarization_);

        summaryRatio_ = new QDoubleSpinBox;
        summaryRatio_->setRange(0.05, 1.0);
        summaryRatio_->setSingleStep(0.05);
        summaryRatio_->setDecimals(2);
        summaryRatio_->setValue(config.summaryRatio);
        form->addRow(QString::fromUtf8("摘要压缩比"), summaryRatio_);

        keywords_ = new QSpinBox;
        keywords_->setRange(3, 30);
        keywords_->setValue(config.maxKeywords);
        form->addRow(QString::fromUtf8("关键词数量"), keywords_);

        actionItems_ = new QSpinBox;
        actionItems_->setRange(1, 100);
        actionItems_->setValue(config.maxActionItems);
        form->addRow(QString::fromUtf8("待办数量上限"), actionItems_);

        tabs->addTab(page, QString::fromUtf8("纪要"));
    }

    // ---------------- 输出 ----------------
    {
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);

        auto* dirRow = new QWidget;
        auto* dirLayout = new QHBoxLayout(dirRow);
        dirLayout->setContentsMargins(0, 0, 0, 0);
        outputDir_ = new QLineEdit(QString::fromStdString(config.outputDir));
        outputDir_->setPlaceholderText(QString::fromUtf8("留空 = 与音频文件同目录"));
        auto* browseDir = new QPushButton(QString::fromUtf8("浏览…"));
        dirLayout->addWidget(outputDir_, 1);
        dirLayout->addWidget(browseDir);
        form->addRow(QString::fromUtf8("导出目录"), dirRow);
        connect(browseDir, &QPushButton::clicked, this, &SettingsDialog::browseOutputDir);

        auto* fmtBox = new QGroupBox(QString::fromUtf8("导出格式"));
        auto* fmtLayout = new QVBoxLayout(fmtBox);
        exportMd_ = new QCheckBox(QString::fromUtf8("Markdown（推荐）"));
        exportJson_ = new QCheckBox(QString::fromUtf8("JSON（结构化，供二次开发）"));
        exportSrt_ = new QCheckBox(QString::fromUtf8("SRT（字幕）"));
        exportTxt_ = new QCheckBox(QString::fromUtf8("纯文本"));
        exportHtml_ = new QCheckBox(QString::fromUtf8("HTML（自包含）"));
        exportMd_->setChecked(config.exportMarkdown);
        exportJson_->setChecked(config.exportJson);
        exportSrt_->setChecked(config.exportSrt);
        exportTxt_->setChecked(config.exportText);
        exportHtml_->setChecked(config.exportHtml);
        fmtLayout->addWidget(exportMd_);
        fmtLayout->addWidget(exportJson_);
        fmtLayout->addWidget(exportSrt_);
        fmtLayout->addWidget(exportTxt_);
        fmtLayout->addWidget(exportHtml_);
        form->addRow(fmtBox);

        logLevel_ = new QComboBox;
        logLevel_->addItem(QString::fromUtf8("错误"), static_cast<int>(LogLevel::Error));
        logLevel_->addItem(QString::fromUtf8("警告"), static_cast<int>(LogLevel::Warn));
        logLevel_->addItem(QString::fromUtf8("信息"), static_cast<int>(LogLevel::Info));
        logLevel_->addItem(QString::fromUtf8("调试"), static_cast<int>(LogLevel::Debug));
        const int lgi = logLevel_->findData(static_cast<int>(config.logLevel));
        logLevel_->setCurrentIndex(lgi >= 0 ? lgi : 2);
        form->addRow(QString::fromUtf8("日志级别"), logLevel_);

        tabs->addTab(page, QString::fromUtf8("输出"));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    onBackendChanged();
    connect(backend_, &QComboBox::currentIndexChanged, this, &SettingsDialog::onBackendChanged);
}

void SettingsDialog::onBackendChanged() {
    const auto backend = static_cast<AsrBackend>(backend_->currentData().toInt());
    const bool needsModel = (backend == AsrBackend::Whisper || backend == AsrBackend::Auto);
    modelPath_->setEnabled(needsModel);
}

void SettingsDialog::browseModel() {
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("选择 whisper ggml 模型"), modelPath_->text(),
        QString::fromUtf8("GGML 模型 (*.bin);;所有文件 (*)"));
    if (!path.isEmpty()) modelPath_->setText(path);
}

void SettingsDialog::browseOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选择导出目录"), outputDir_->text());
    if (!dir.isEmpty()) outputDir_->setText(dir);
}

Config SettingsDialog::result() const {
    Config c = config_;
    c.backend = static_cast<AsrBackend>(backend_->currentData().toInt());
    c.modelPath = modelPath_->text().toStdString();
    c.language = language_->currentData().toString().toStdString();
    c.threads = threads_->value();
    // 仅当开关可用（本构建含 GPU 后端）时才写回。
    // 否则会把「本构建不可用」这个临时状态固化成 useGpu=false，
    // 之后换成 GPU 版构建仍会停在 CPU，且用户不易察觉。
    if (gpu_->isEnabled()) c.useGpu = gpu_->isChecked();
    c.enableDiarization = diarization_->isChecked();
    if (autoSpeakers_->isChecked()) {
        c.speakerMode = SpeakerMode::Auto;
        c.speakerCount = 0;
    } else {
        c.speakerMode = SpeakerMode::Fixed;
        c.speakerCount = speakers_->value();
    }
    c.diarMergeThreshold = mergeThreshold_->value();
    c.minSpeechMs = minSpeech_->value();
    c.maxSegmentMs = maxSegment_->value();
    c.enableSummarization = summarization_->isChecked();
    c.summaryRatio = summaryRatio_->value();
    c.maxKeywords = keywords_->value();
    c.maxActionItems = actionItems_->value();
    c.outputDir = outputDir_->text().toStdString();
    c.exportMarkdown = exportMd_->isChecked();
    c.exportJson = exportJson_->isChecked();
    c.exportSrt = exportSrt_->isChecked();
    c.exportText = exportTxt_->isChecked();
    c.exportHtml = exportHtml_->isChecked();
    c.logLevel = static_cast<LogLevel>(logLevel_->currentData().toInt());
    return c;
}

}  // namespace mm::gui

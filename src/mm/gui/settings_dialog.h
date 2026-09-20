// MeetMind — 设置对话框
#pragma once

#include <QDialog>

#include "mm/common/config.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;

namespace mm::gui {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const Config& config, QWidget* parent = nullptr);

    /// 读取用户修改后的配置（合并回传入的配置对象）。
    Config result() const;

private slots:
    void browseModel();
    void browseOutputDir();
    void onBackendChanged();

private:
    Config config_;

    QComboBox* backend_ = nullptr;
    QLineEdit* modelPath_ = nullptr;
    QComboBox* language_ = nullptr;
    QSpinBox* threads_ = nullptr;
    QCheckBox* gpu_ = nullptr;
    QSpinBox* speakers_ = nullptr;
    QCheckBox* autoSpeakers_ = nullptr;
    QDoubleSpinBox* mergeThreshold_ = nullptr;
    QCheckBox* diarization_ = nullptr;
    QCheckBox* summarization_ = nullptr;
    QDoubleSpinBox* summaryRatio_ = nullptr;
    QSpinBox* keywords_ = nullptr;
    QSpinBox* actionItems_ = nullptr;
    QSpinBox* minSpeech_ = nullptr;
    QSpinBox* maxSegment_ = nullptr;
    QLineEdit* outputDir_ = nullptr;
    QLineEdit* lexiconPath_ = nullptr;
    QCheckBox* exportMd_ = nullptr;
    QCheckBox* exportJson_ = nullptr;
    QCheckBox* exportSrt_ = nullptr;
    QCheckBox* exportTxt_ = nullptr;
    QCheckBox* exportHtml_ = nullptr;
    QComboBox* logLevel_ = nullptr;
};

}  // namespace mm::gui

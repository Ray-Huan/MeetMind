// MeetMind — 波形与时间轴视图
// 自绘控件：显示波形包络、VAD 语音段、说话人色带与时间刻度。
// 包络在载入时一次性计算（200 点/秒），避免在 paintEvent 中扫描全部采样点。
#pragma once

#include <QString>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <vector>

#include "mm/pipeline/pipeline_types.h"

namespace mm::gui {

class WaveformView : public QWidget {
    Q_OBJECT

public:
    explicit WaveformView(QWidget* parent = nullptr);
    ~WaveformView() override;

    /// 载入音频文件并构建包络（失败时清空并显示占位提示）。
    void loadAudio(const QString& path);

    /// 绑定处理结果（用于绘制语音段与说话人色带）。
    void setResult(const std::shared_ptr<pipeline::PipelineResult>& result);

    void clear();

    /// 当前选中的时间点（毫秒）；-1 表示无。
    int64_t cursorMs() const { return cursorMs_; }
    void setCursorMs(int64_t ms);

    int64_t durationMs() const { return durationMs_; }

signals:
    /// 用户单击波形时发出
    void seekRequested(int64_t ms);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    static constexpr int kPointsPerSecond = 200;
    static constexpr int kTopMargin = 8;
    static constexpr int kBandsHeight = 26;
    static constexpr int kRulerHeight = 18;

    int64_t xToMs(int x) const;
    int msToX(int64_t ms) const;
    void drawPlaceholder(QPainter& painter);
    QColor speakerColor(int speakerId) const;

    std::vector<float> envMin_;
    std::vector<float> envMax_;
    int64_t durationMs_ = 0;
    QString sourcePath_;
    QString errorText_;
    int64_t cursorMs_ = -1;
    std::shared_ptr<pipeline::PipelineResult> result_;
};

}  // namespace mm::gui

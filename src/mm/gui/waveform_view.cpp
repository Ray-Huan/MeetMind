#include "waveform_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

#include "mm/audio/wav_io.h"
#include "mm/common/logger.h"
#include "mm/common/time_utils.h"

namespace mm::gui {
namespace {

const QColor kEmptyBg(0xF8, 0xFA, 0xFC);
const QColor kEmptyBorder(0xCB, 0xD5, 0xE1);
const QColor kEmptyText(0x94, 0xA3, 0xB8);
const QColor kWaveColor(0x64, 0x74, 0x8B);
const QColor kSegmentFill(0x3B, 0x82, 0xF6, 60);
const QColor kSegmentEdge(0x3B, 0x82, 0xF6, 150);
const QColor kRuler(0x94, 0xA3, 0xB8);
const QColor kCursor(0xEF, 0x44, 0x44);

}  // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

WaveformView::~WaveformView() = default;

void WaveformView::clear() {
    envMin_.clear();
    envMax_.clear();
    durationMs_ = 0;
    sourcePath_.clear();
    errorText_.clear();
    cursorMs_ = -1;
    result_.reset();
    update();
}

void WaveformView::loadAudio(const QString& path) {
    envMin_.clear();
    envMax_.clear();
    durationMs_ = 0;
    sourcePath_ = path;
    errorText_.clear();

    AudioQualityReport report;
    Result<AudioBuffer> audio = wav::read(path.toStdString(), &report);
    if (!audio.ok()) {
        errorText_ = QString::fromUtf8("无法载入音频：") +
                     QString::fromStdString(audio.message());
        MM_LOG_WARN("gui.waveform") << "载入波形失败: " << audio.message();
        update();
        return;
    }

    const AudioBuffer& buf = audio.value();
    durationMs_ = buf.durationMs();
    const size_t total = buf.samples.size();
    if (total == 0) {
        errorText_ = QString::fromUtf8("音频为空");
        update();
        return;
    }

    const size_t bucket =
        std::max<size_t>(1, static_cast<size_t>(buf.sampleRate) / kPointsPerSecond);
    const size_t count = (total + bucket - 1) / bucket;
    envMin_.resize(count);
    envMax_.resize(count);

    for (size_t i = 0; i < count; ++i) {
        const size_t from = i * bucket;
        const size_t to = std::min(total, from + bucket);
        float lo = 0.0f;
        float hi = 0.0f;
        for (size_t k = from; k < to; ++k) {
            const float v = buf.samples[k];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        envMin_[i] = lo;
        envMax_[i] = hi;
    }
    MM_LOG_INFO("gui.waveform") << "波形包络已构建: " << count << " 点, 时长 "
                                << report.durationMs << " ms";
    update();
}

void WaveformView::setResult(const std::shared_ptr<pipeline::PipelineResult>& result) {
    result_ = result;
    update();
}

int64_t WaveformView::xToMs(int x) const {
    if (durationMs_ <= 0) return 0;
    const int w = std::max(1, width());
    const double frac = std::max(0.0, std::min(1.0, static_cast<double>(x) / w));
    return static_cast<int64_t>(frac * static_cast<double>(durationMs_));
}

int WaveformView::msToX(int64_t ms) const {
    if (durationMs_ <= 0) return 0;
    const int w = std::max(1, width());
    const double frac = static_cast<double>(ms) / static_cast<double>(durationMs_);
    return static_cast<int>(frac * w);
}

QColor WaveformView::speakerColor(int speakerId) const {
    if (speakerId < 0) return QColor(0x94, 0xA3, 0xB8);
    static const QColor kColors[] = {
        QColor(0x25, 0x63, 0xEB), QColor(0x16, 0xA3, 0x4A), QColor(0xD9, 0x77, 0x06),
        QColor(0xDC, 0x26, 0x26), QColor(0x7C, 0x3A, 0xED), QColor(0x08, 0x91, 0xB2),
        QColor(0xBE, 0x18, 0x5D), QColor(0x65, 0xA3, 0x0D),
    };
    return kColors[speakerId % 8];
}

void WaveformView::setCursorMs(int64_t ms) {
    cursorMs_ = ms;
    update();
}

void WaveformView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    if (durationMs_ <= 0) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int x = static_cast<int>(event->position().x());
#else
    const int x = event->x();
#endif
    const int64_t ms = xToMs(x);
    setCursorMs(ms);
    emit seekRequested(ms);
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (durationMs_ <= 0) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int x = static_cast<int>(event->position().x());
#else
    const int x = event->x();
#endif
    setToolTip(QString::fromStdString(timeutil::formatDuration(xToMs(x))));
}

void WaveformView::drawPlaceholder(QPainter& painter) {
    painter.setPen(QPen(kEmptyBorder, 1, Qt::DashLine));
    painter.setBrush(kEmptyBg);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
    painter.setPen(kEmptyText);
    painter.drawText(rect(), Qt::AlignCenter,
                     errorText_.isEmpty() ? QString::fromUtf8("拖入 WAV 文件或使用「打开音频」载入")
                                          : errorText_);
}

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (durationMs_ <= 0 || envMin_.empty()) {
        drawPlaceholder(painter);
        return;
    }

    const QPalette pal = palette();
    const int w = width();
    const int h = height();
    const int waveTop = kTopMargin;
    const int waveBottom = h - kBandsHeight - kRulerHeight - kTopMargin;
    const int mid = (waveTop + waveBottom) / 2;
    const int halfHeight = std::max(1, (waveBottom - waveTop) / 2);

    painter.fillRect(rect(), pal.base());

    // ---- 语音段底色 ----
    if (result_) {
        for (const SpeechSegment& seg : result_->segments) {
            const int x1 = msToX(seg.startMs);
            const int x2 = std::max(x1 + 1, msToX(seg.endMs));
            painter.fillRect(QRect(x1, waveTop, x2 - x1, waveBottom - waveTop), kSegmentFill);
            painter.setPen(kSegmentEdge);
            painter.drawLine(x1, waveTop, x1, waveBottom);
            painter.drawLine(x2, waveTop, x2, waveBottom);
        }
    }

    // ---- 波形包络 ----
    painter.setPen(QPen(kWaveColor, 1));
    const size_t points = envMin_.size();
    for (int x = 0; x < w; ++x) {
        const size_t i = static_cast<size_t>(
            static_cast<double>(x) / std::max(1, w) * static_cast<double>(points));
        if (i >= points) continue;
        const int y1 = mid - static_cast<int>(envMax_[i] * halfHeight);
        const int y2 = mid - static_cast<int>(envMin_[i] * halfHeight);
        painter.drawLine(x, y1, x, std::max(y1 + 1, y2));
    }

    // ---- 零点参考线 ----
    painter.setPen(QPen(QColor(0xE2, 0xE8, 0xF0), 1));
    painter.drawLine(0, mid, w, mid);

    // ---- 说话人色带 ----
    const int bandTop = waveBottom + 4;
    if (result_ && !result_->diarization.turns.empty()) {
        for (const diar::SpeakerTurn& turn : result_->diarization.turns) {
            const int x1 = msToX(turn.startMs);
            const int x2 = std::max(x1 + 1, msToX(turn.endMs));
            QColor c = speakerColor(turn.speakerId);
            c.setAlpha(190);
            painter.fillRect(QRect(x1, bandTop, x2 - x1, kBandsHeight - 10), c);
            if (x2 - x1 > 60) {
                painter.setPen(Qt::white);
                painter.drawText(QRect(x1 + 4, bandTop, x2 - x1 - 8, kBandsHeight - 10),
                                 Qt::AlignVCenter | Qt::AlignLeft,
                                 QString::fromStdString(result_->speakerLabel(turn.speakerId)));
            }
        }
    } else {
        painter.setPen(kEmptyText);
        painter.drawText(QRect(0, bandTop, w, kBandsHeight - 10),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QString::fromUtf8(result_ ? "未识别说话人（单说话人或已关闭分离）"
                                                   : "尚未处理"));
    }

    // ---- 时间刻度 ----
    const int rulerTop = h - kRulerHeight;
    painter.setPen(kRuler);
    const int64_t totalSec = durationMs_ / 1000;
    int stepSec = 1;
    if (totalSec > 600) stepSec = 120;
    else if (totalSec > 300) stepSec = 60;
    else if (totalSec > 120) stepSec = 30;
    else if (totalSec > 60) stepSec = 10;
    else if (totalSec > 20) stepSec = 5;

    for (int64_t s = 0; s <= totalSec; s += stepSec) {
        const int x = msToX(s * 1000);
        if (x > w - 1) break;
        painter.drawLine(x, rulerTop, x, rulerTop + 4);
        painter.drawText(QRect(x + 3, rulerTop + 2, 60, kRulerHeight - 2),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QString::fromStdString(timeutil::formatShort(s * 1000)));
    }
    painter.setPen(QColor(0xE2, 0xE8, 0xF0));
    painter.drawLine(0, rulerTop, w, rulerTop);

    // ---- 游标 ----
    if (cursorMs_ >= 0 && cursorMs_ <= durationMs_) {
        const int x = msToX(cursorMs_);
        painter.setPen(QPen(kCursor, 1));
        painter.drawLine(x, waveTop, x, rulerTop);
    }

    // ---- 外框 ----
    painter.setPen(QPen(QColor(0xE2, 0xE8, 0xF0), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

}  // namespace mm::gui

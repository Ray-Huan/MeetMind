#include "transcript_model.h"

#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm::gui {

TranscriptModel::TranscriptModel(QObject* parent) : QAbstractTableModel(parent) {}

void TranscriptModel::setResult(const std::shared_ptr<pipeline::PipelineResult>& result) {
    beginResetModel();
    result_ = result;
    endResetModel();
}

void TranscriptModel::clear() {
    beginResetModel();
    result_.reset();
    endResetModel();
}

int TranscriptModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !result_) return 0;
    return static_cast<int>(result_->asr.segments.size());
}

int TranscriptModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColumnCount;
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const {
    if (!result_ || !index.isValid()) return {};
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(result_->asr.segments.size())) return {};
    const asr::AsrSegment& seg = result_->asr.segments[static_cast<size_t>(row)];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColumnTime:
                return QString::fromStdString(timeutil::formatDuration(seg.startMs));
            case ColumnSpeaker:
                return QString::fromStdString(result_->speakerLabel(seg.speakerId));
            case ColumnText:
                return QString::fromStdString(str::trim(seg.text));
            case ColumnConfidence:
                return QString::number(seg.confidence * 100.0, 'f', 0) + "%";
            default:
                break;
        }
    } else if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColumnTime || index.column() == ColumnConfidence) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    } else if (role == Qt::ForegroundRole) {
        // 低置信片段用橙色提示，便于人工复核
        if (seg.confidence < 0.45f && !str::trim(seg.text).empty()) {
            return QColor(0xB4, 0x53, 0x09);
        }
        if (index.column() == ColumnTime) {
            return QColor(0x64, 0x74, 0x8B);
        }
    } else if (role == Qt::ToolTipRole) {
        return QString::fromUtf8("时间 %1 - %2\n置信度 %3%\n后端 %4")
            .arg(QString::fromStdString(timeutil::formatClock(seg.startMs)))
            .arg(QString::fromStdString(timeutil::formatClock(seg.endMs)))
            .arg(QString::number(seg.confidence * 100.0, 'f', 1))
            .arg(QString::fromStdString(result_->asr.backend));
    }
    return {};
}

QVariant TranscriptModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case ColumnTime: return QString::fromUtf8("时间");
        case ColumnSpeaker: return QString::fromUtf8("说话人");
        case ColumnText: return QString::fromUtf8("转写内容");
        case ColumnConfidence: return QString::fromUtf8("置信度");
        default: return {};
    }
}

int64_t TranscriptModel::startMsAt(int row) const {
    if (!result_ || row < 0 || row >= static_cast<int>(result_->asr.segments.size())) return -1;
    return result_->asr.segments[static_cast<size_t>(row)].startMs;
}

}  // namespace mm::gui

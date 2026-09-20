// MeetMind — 转写结果表格模型
#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QString>
#include <vector>

#include "mm/pipeline/pipeline_types.h"

namespace mm::gui {

class TranscriptModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColumnTime = 0, ColumnSpeaker, ColumnText, ColumnConfidence, ColumnCount };

    explicit TranscriptModel(QObject* parent = nullptr);

    /// 载入结果；模型只保存共享指针，不复制数据。
    void setResult(const std::shared_ptr<pipeline::PipelineResult>& result);
    void clear();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /// 某行对应的时间点（毫秒），用于点击定位波形。
    int64_t startMsAt(int row) const;

private:
    std::shared_ptr<pipeline::PipelineResult> result_;
};

}  // namespace mm::gui

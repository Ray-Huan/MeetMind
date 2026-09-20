// MeetMind — 后台处理线程
// worker-object 模式：PipelineWorker 在独立 QThread 中执行，通过信号把进度/结果投递回主线程。
#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "mm/common/config.h"
#include "mm/pipeline/pipeline.h"

namespace mm::gui {

/// 处理结果（跨线程传递，使用共享指针避免深拷贝）
using SharedResult = std::shared_ptr<pipeline::PipelineResult>;

class PipelineWorker : public QObject {
    Q_OBJECT

public:
    explicit PipelineWorker(Config config, QObject* parent = nullptr);
    ~PipelineWorker() override;

    /// 请求取消（线程安全，可从任意线程调用）
    void requestCancel();

public slots:
    /// 在工作线程中执行；由 startRequested 信号触发
    void process();

signals:
    void startRequested();
    void progressChanged(int stageIndex, int stageCount, int percent, const QString& message);
    void finished(SharedResult result);
    void failed(const QString& message, int errorCode);

private:
    Config config_;
    CancelToken cancel_;
};

}  // namespace mm::gui

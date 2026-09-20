#include "session_worker.h"

#include <QThread>

#include "mm/common/logger.h"

namespace mm::gui {

PipelineWorker::PipelineWorker(Config config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {}

PipelineWorker::~PipelineWorker() = default;

void PipelineWorker::requestCancel() { cancel_.cancel(); }

void PipelineWorker::process() {
    cancel_.reset();
    pipeline::Pipeline pipe(config_);

    pipeline::ProgressCallback cb = [this](const pipeline::ProgressInfo& info) {
        emit progressChanged(info.stageIndex, info.stageCount,
                             static_cast<int>(info.fraction * 100.0 + 0.5),
                             QString::fromUtf8(info.message));
    };

    Result<pipeline::PipelineResult> result = pipe.run(&cancel_, cb);
    if (!result.ok()) {
        MM_LOG_ERROR("gui.worker") << "处理失败: " << result.message();
        emit failed(QString::fromUtf8(result.message().c_str()), static_cast<int>(result.code()));
        return;
    }
    emit finished(std::make_shared<pipeline::PipelineResult>(std::move(result.value())));
}

}  // namespace mm::gui

// MeetMind — 识别后端工厂
// 依据配置与运行环境选择可用后端，并给出选择理由日志，保证「无模型也能跑通链路」。
#pragma once

#include <memory>
#include <string>

#include "mm/asr/asr_types.h"
#include "mm/asr/iasr_engine.h"
#include "mm/common/config.h"
#include "mm/common/result.h"

namespace mm::asr {

struct EngineSelection {
    std::unique_ptr<IAsrEngine> engine;
    std::string reason;   ///< 选择该后端的理由（写入报告，便于排查）
    bool degraded = false;///< 是否因环境限制而降级
};

class AsrFactory {
public:
    /// 按配置选择并「初始化好」的后端。
    static Result<EngineSelection> create(const Config& config);

    /// 仅选择类型，不初始化（用于能力探测与界面展示）。
    static AsrBackend resolveBackend(const Config& config, std::string* reason = nullptr);

    /// 探测默认模型路径：依次尝试 <projectRoot>/models/ggml-base.bin、ggml-small.bin 等。
    static std::string detectDefaultModel(const std::string& modelsDir = {});
};

}  // namespace mm::asr

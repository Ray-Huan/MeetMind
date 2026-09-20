// MeetMind — 语音识别后端抽象接口
// 上层（Pipeline）只依赖此接口；新增后端无需修改任何调用方代码（开闭原则）。
#pragma once

#include <string>

#include "mm/asr/asr_types.h"
#include "mm/audio/audio_types.h"
#include "mm/common/result.h"

namespace mm::asr {

class IAsrEngine {
public:
    virtual ~IAsrEngine() = default;

    /// 后端唯一标识："whisper" / "replay" / "null"
    virtual std::string id() const = 0;
    /// 人类可读名称
    virtual std::string displayName() const = 0;
    virtual AsrCapabilities capabilities() const = 0;

    /// 加载模型/准备资源。可重复调用（幂等）。
    virtual Result<void> initialize(const AsrModelConfig& config) = 0;
    /// 是否已就绪（可调用 transcribe）。
    virtual bool ready() const = 0;

    /// 转写单个音频块。
    /// @param chunk       单声道 16 kHz 浮点音频
    /// @param chunkStartMs 该块在原始会议音频中的起始时间（用于回填时间戳）
    /// @param cancel      可空；非空时后端应在长循环中检查取消
    virtual Result<AsrSegment> transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                          const CancelToken* cancel) = 0;
};

}  // namespace mm::asr

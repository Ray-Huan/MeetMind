// MeetMind — 空识别引擎
// 用途：只验证音频前端与纪要链路、跳过识别；也用于性能基准中隔离 ASR 开销。
#pragma once

#include "mm/asr/iasr_engine.h"

namespace mm::asr {

class NullAsrEngine final : public IAsrEngine {
public:
    std::string id() const override { return "null"; }
    std::string displayName() const override { return "空引擎（跳过识别）"; }
    AsrCapabilities capabilities() const override {
        AsrCapabilities c;
        c.onDevice = true;
        c.requiresModelFile = false;
        c.multilingual = false;
        c.description = "不做任何识别，仅返回空文本段";
        return c;
    }
    Result<void> initialize(const AsrModelConfig&) override {
        ready_ = true;
        return okStatus();
    }
    bool ready() const override { return ready_; }
    Result<AsrSegment> transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                  const CancelToken* cancel) override {
        if (cancel && cancel->isCancelled()) {
            return fail(ErrorCode::Cancelled, "转写已取消");
        }
        AsrSegment seg;
        seg.startMs = chunkStartMs;
        seg.endMs = chunkStartMs + chunk.durationMs();
        seg.confidence = 0.0f;
        seg.noSpeechProb = 1.0f;
        return seg;
    }

private:
    bool ready_ = false;
};

}  // namespace mm::asr

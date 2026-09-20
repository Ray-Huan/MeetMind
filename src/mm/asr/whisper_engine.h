// MeetMind — whisper.cpp 端侧推理后端
// 仅在编译期定义 MEETMIND_WITH_WHISPER 时启用；未启用时工厂自动退化为回放引擎。
#pragma once

#include "mm/asr/iasr_engine.h"

namespace mm::asr {

class WhisperCppEngine final : public IAsrEngine {
public:
    WhisperCppEngine();
    ~WhisperCppEngine() override;

    WhisperCppEngine(const WhisperCppEngine&) = delete;
    WhisperCppEngine& operator=(const WhisperCppEngine&) = delete;

    std::string id() const override { return "whisper"; }
    std::string displayName() const override { return "whisper.cpp（端侧）"; }
    AsrCapabilities capabilities() const override;

    Result<void> initialize(const AsrModelConfig& config) override;
    bool ready() const override;
    Result<AsrSegment> transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                  const CancelToken* cancel) override;

    /// 本构建是否包含 whisper.cpp 支持。
    static bool compiledIn();

    /// whisper.cpp 编译期启用的后端清单（来自 whisper_print_system_info）。
    /// 用于回答「我到底能不能用 GPU」——例如字符串里含 "CUDA" 或 "Vulkan"
    /// 才代表对应 GPU 后端被编译进来。
    static std::string systemInfo();

    /// 系统信息中是否包含可用的 GPU 后端。
    static bool gpuBackendAvailable();

    /// 是否已实际在 GPU 上初始化（initialize 之后才有效）。
    bool usingGpu() const { return usingGpu_; }

    /// 「请求了 GPU 但不可用」的告警文本；非空表示实际退回了 CPU。
    const std::string& gpuWarning() const { return gpuWarning_; }

    /// 最近一次初始化/推理的错误描述（便于界面提示）。
    const std::string& lastError() const { return lastError_; }

private:
    void release();

    struct Impl;   ///< 隐藏 whisper.h 依赖，避免污染公共头文件
    Impl* impl_ = nullptr;
    AsrModelConfig config_;
    std::string lastError_;
    bool ready_ = false;
    bool usingGpu_ = false;
    std::string gpuWarning_;
};

}  // namespace mm::asr

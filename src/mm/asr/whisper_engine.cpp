#include "mm/asr/whisper_engine.h"

#include <algorithm>
#include <filesystem>
#include <thread>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

#if defined(MEETMIND_WITH_WHISPER)
#include "whisper.h"
#endif

namespace mm::asr {

#if defined(MEETMIND_WITH_WHISPER)

struct WhisperCppEngine::Impl {
    whisper_context* ctx = nullptr;
};

WhisperCppEngine::WhisperCppEngine() : impl_(new Impl()) {}
WhisperCppEngine::~WhisperCppEngine() { release(); }

bool WhisperCppEngine::compiledIn() { return true; }

std::string WhisperCppEngine::systemInfo() {
    const char* info = whisper_print_system_info();
    return info != nullptr ? std::string(info) : std::string();
}

bool WhisperCppEngine::gpuBackendAvailable() {
    const std::string info = systemInfo();
    return info.find("CUDA") != std::string::npos ||
           info.find("Vulkan") != std::string::npos ||
           info.find("Metal") != std::string::npos ||
           info.find("SYCL") != std::string::npos ||
           info.find("OpenCL") != std::string::npos;
}

AsrCapabilities WhisperCppEngine::capabilities() const {
    AsrCapabilities c;
    c.wordTimestamps = true;
    c.streaming = false;
    c.multilingual = true;
    c.onDevice = true;
    c.requiresModelFile = true;
    c.description = "whisper.cpp（GGML/CPU 端侧推理，完全离线）";
    return c;
}

void WhisperCppEngine::release() {
    if (impl_ && impl_->ctx) {
        whisper_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    ready_ = false;
}

Result<void> WhisperCppEngine::initialize(const AsrModelConfig& config) {
    config_ = config;
    if (config.modelPath.empty()) {
        lastError_ = "未指定 whisper 模型路径（--model 或在设置中配置）";
        MM_LOG_WARN("asr.whisper") << lastError_;
        return fail(ErrorCode::ModelNotLoaded, lastError_);
    }
    std::error_code ec;
    if (!std::filesystem::exists(config.modelPath, ec)) {
        lastError_ = "模型文件不存在: " + config.modelPath;
        MM_LOG_ERROR("asr.whisper") << lastError_;
        return fail(ErrorCode::FileNotFound, lastError_);
    }

    release();

    if (config.useGpu && !gpuBackendAvailable()) {
        // 本构建没有编入任何 GPU 后端。注意：useGpu 默认为 true，所以这条在
        // 「CPU 版构建」上是常态而不是故障 —— 它属于**构建选择**，
        // 因此只记 INFO（WARN 会让 CPU 版构建每次运行都刷一条，反而淹没真问题）。
        //
        // 但仍然把文字留在 gpuWarning_ 里：界面可以据此显示一行温和提示，
        // 保持「以为在用显卡、实际在跑 CPU」这类误解可被发现。
        gpuWarning_ = "本构建未包含 GPU 后端（编译期后端: " + systemInfo() +
                      "），已按 CPU 运行；GPU 版构建见 tools/build-whisper-cuda.sh";
        MM_LOG_INFO("asr.whisper") << gpuWarning_;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config.useGpu;
    cparams.flash_attn = false;

    const int64_t t0 = 0;
    (void)t0;
    MM_LOG_INFO("asr.whisper") << "加载模型: " << config.modelPath
                               << " (use_gpu=" << (config.useGpu ? "true" : "false") << ")";

    impl_->ctx = whisper_init_from_file_with_params(config.modelPath.c_str(), cparams);
    if (impl_->ctx == nullptr) {
        lastError_ = "whisper_init_from_file_with_params 返回空指针，模型可能损坏: " + config.modelPath;
        MM_LOG_ERROR("asr.whisper") << lastError_;
        return fail(ErrorCode::ModelNotLoaded, lastError_);
    }

    ready_ = true;
    usingGpu_ = config.useGpu && gpuBackendAvailable();
    lastError_.clear();
    MM_LOG_INFO("asr.whisper") << "模型加载完成（推理设备: "
                               << (usingGpu_ ? "GPU" : "CPU") << "）";
    return okStatus();
}

bool WhisperCppEngine::ready() const { return ready_ && impl_ && impl_->ctx != nullptr; }

Result<AsrSegment> WhisperCppEngine::transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                                const CancelToken* cancel) {
    if (!ready()) {
        return fail(ErrorCode::ModelNotLoaded, "whisper 后端未就绪，请先调用 initialize()");
    }
    if (chunk.samples.empty()) {
        return fail(ErrorCode::InvalidArgument, "音频块为空");
    }
    if (cancel && cancel->isCancelled()) {
        return fail(ErrorCode::Cancelled, "转写已取消");
    }
    // whisper.cpp 要求 16 kHz 单声道浮点
    if (chunk.sampleRate != 16000) {
        return fail(ErrorCode::InvalidArgument,
                    "whisper 后端要求 16 kHz 输入，实际 " + std::to_string(chunk.sampleRate));
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    const int threads = config_.threads > 0 ? config_.threads : 4;
    params.n_threads = threads;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.translate = config_.translate;
    params.no_context = true;
    params.no_timestamps = false;
    params.single_segment = false;
    params.suppress_blank = true;
    params.temperature_inc = config_.temperatureInc;
    params.token_timestamps = config_.wordTimestamps;

    std::string lang;
    if (!config_.language.empty() && !str::equalsIgnoreCaseAscii(config_.language, "auto")) {
        lang = config_.language;
        params.language = lang.c_str();
        params.detect_language = false;
    } else {
        // 自动识别语言。
        //
        // 重要（曾导致"默认配置输出全空"的缺陷）：whisper.cpp 里 detect_language
        // 的语义是「只做语言检测，然后直接返回」，源码为：
        //     if (params.detect_language) { return 0; }
        // 因此 detect_language=true 时 whisper_full 不会产生任何转写结果。
        // 「自动识别语言并继续转写」的正确姿势是：language 传 "auto"（或 nullptr），
        // 而 detect_language 保持 false —— 这样内部完成检测后会接着解码。
        params.language = "auto";
        params.detect_language = false;
    }

    const int rc = whisper_full(impl_->ctx, params, chunk.samples.data(),
                                static_cast<int>(chunk.samples.size()));
    if (rc != 0) {
        lastError_ = "whisper_full 执行失败，返回码 " + std::to_string(rc);
        MM_LOG_ERROR("asr.whisper") << lastError_;
        return fail(ErrorCode::InferenceFailed, lastError_);
    }
    if (cancel && cancel->isCancelled()) {
        return fail(ErrorCode::Cancelled, "转写已取消");
    }

    AsrSegment out;
    out.startMs = chunkStartMs;
    out.endMs = chunkStartMs + chunk.durationMs();

    const int nSeg = whisper_full_n_segments(impl_->ctx);
    double probSum = 0.0;
    int probCount = 0;
    double noSpeechSum = 0.0;
    std::string merged;

    for (int i = 0; i < nSeg; ++i) {
        const char* text = whisper_full_get_segment_text(impl_->ctx, i);
        if (text) merged += text;

        // whisper 的时间戳单位为 10 ms
        const int64_t t0 = whisper_full_get_segment_t0(impl_->ctx, i) * 10;
        const int64_t t1 = whisper_full_get_segment_t1(impl_->ctx, i) * 10;
        if (nSeg == 1 || i == 0) out.startMs = chunkStartMs + t0;
        if (i == nSeg - 1) out.endMs = std::min<int64_t>(chunkStartMs + chunk.durationMs(),
                                                         chunkStartMs + t1);

        const int nTok = whisper_full_n_tokens(impl_->ctx, i);
        for (int t = 0; t < nTok; ++t) {
            probSum += whisper_full_get_token_p(impl_->ctx, i, t);
            ++probCount;
        }
        noSpeechSum += whisper_full_get_segment_no_speech_prob(impl_->ctx, i);

        if (config_.wordTimestamps) {
            for (int t = 0; t < nTok; ++t) {
                const char* tt = whisper_full_get_token_text(impl_->ctx, i, t);
                if (!tt) continue;
                AsrWord w;
                w.text = tt;
                w.probability = whisper_full_get_token_p(impl_->ctx, i, t);
                out.words.push_back(std::move(w));
            }
        }
    }

    out.text = str::trim(merged);
    out.confidence = probCount > 0 ? static_cast<float>(probSum / probCount) : 0.0f;
    out.noSpeechProb = nSeg > 0 ? static_cast<float>(noSpeechSum / nSeg) : 1.0f;
    return out;
}

#else  // !MEETMIND_WITH_WHISPER —— 未编译 whisper 支持时的桩实现

struct WhisperCppEngine::Impl {};

WhisperCppEngine::WhisperCppEngine() : impl_(nullptr) {}
WhisperCppEngine::~WhisperCppEngine() = default;

bool WhisperCppEngine::compiledIn() { return false; }

AsrCapabilities WhisperCppEngine::capabilities() const {
    AsrCapabilities c;
    c.onDevice = true;
    c.requiresModelFile = true;
    c.description = "whisper.cpp 未编译进本构建（使用 -DMEETMIND_WITH_WHISPER=ON 启用）";
    return c;
}

void WhisperCppEngine::release() { ready_ = false; }

Result<void> WhisperCppEngine::initialize(const AsrModelConfig&) {
    lastError_ = "本构建未包含 whisper.cpp 支持，请重新以 -DMEETMIND_WITH_WHISPER=ON 构建";
    MM_LOG_ERROR("asr.whisper") << lastError_;
    return fail(ErrorCode::ModelNotLoaded, lastError_);
}

bool WhisperCppEngine::ready() const { return false; }

Result<AsrSegment> WhisperCppEngine::transcribe(const AudioBuffer&, int64_t, const CancelToken*) {
    return fail(ErrorCode::ModelNotLoaded, lastError_);
}

#endif

}  // namespace mm::asr

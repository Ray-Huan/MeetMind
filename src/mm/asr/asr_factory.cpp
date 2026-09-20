#include "mm/asr/asr_factory.h"

#include <filesystem>
#include <vector>

#include "mm/asr/null_engine.h"
#include "mm/asr/replay_engine.h"
#include "mm/asr/whisper_engine.h"
#include "mm/common/path_utils.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::asr {
namespace fs = std::filesystem;

std::string AsrFactory::detectDefaultModel(const std::string& modelsDir) {
    std::vector<std::string> dirs;
    if (!modelsDir.empty()) dirs.push_back(modelsDir);
    const std::string cwd = pathutil::toUtf8(std::filesystem::current_path());
    if (!cwd.empty()) {
        dirs.push_back(pathutil::join(cwd, "models"));
        dirs.push_back(pathutil::joinPath(cwd, "..", "models"));
    }
    dirs.push_back(pathutil::join(MEETMIND_SOURCE_DIR, "models"));

    // 质量优先：small > base > tiny
    static const char* kCandidates[] = {"ggml-small.bin", "ggml-base.bin", "ggml-tiny.bin",
                                        "ggml-medium.bin"};

    for (const std::string& dir : dirs) {
        if (!pathutil::isDirectory(dir)) continue;
        for (const char* name : kCandidates) {
            const std::string p = pathutil::join(dir, name);
            if (pathutil::isRegularFile(p)) {
                const std::string s = pathutil::canonical(p);
                MM_LOG_DEBUG("asr.factory") << "探测到默认模型: " << s;
                return s;
            }
        }
    }
    return {};
}

AsrBackend AsrFactory::resolveBackend(const Config& config, std::string* reason) {
    auto setReason = [&](const std::string& r) {
        if (reason) *reason = r;
    };

    switch (config.backend) {
        case AsrBackend::Null:
            setReason("配置显式指定 null 后端");
            return AsrBackend::Null;

        case AsrBackend::Replay:
            setReason("配置显式指定 replay 后端");
            return AsrBackend::Replay;

        case AsrBackend::Whisper:
            if (!WhisperCppEngine::compiledIn()) {
                setReason("请求 whisper 后端，但本构建未编译 whisper.cpp");
                // 配置显式指定时仍返回 Whisper，由 initialize 报错，避免静默降级
            } else {
                setReason("配置显式指定 whisper 后端");
            }
            return AsrBackend::Whisper;

        case AsrBackend::Auto:
        default:
            break;
    }

    if (!WhisperCppEngine::compiledIn()) {
        setReason("本构建未包含 whisper.cpp，自动退化为回放引擎");
        return AsrBackend::Replay;
    }
    if (config.modelPath.empty()) {
        setReason("未配置模型路径，自动退化为回放引擎");
        return AsrBackend::Replay;
    }
    if (!pathutil::exists(config.modelPath)) {
        setReason("模型文件不存在（" + config.modelPath + "），自动退化为回放引擎");
        return AsrBackend::Replay;
    }
    setReason("whisper.cpp 可用且模型存在，使用端侧推理");
    return AsrBackend::Whisper;
}

Result<EngineSelection> AsrFactory::create(const Config& config) {
    std::string reason;
    const AsrBackend resolved = resolveBackend(config, &reason);

    EngineSelection selection;
    selection.reason = reason;

    AsrModelConfig modelCfg;
    modelCfg.modelPath = config.modelPath;
    modelCfg.language = config.language;
    modelCfg.threads = config.effectiveThreads();
    modelCfg.useGpu = config.useGpu;
    modelCfg.temperatureInc = static_cast<float>(config.asrTemperatureInc);
    modelCfg.replayScriptPath = config.replayScriptPath;

    switch (resolved) {
        case AsrBackend::Null:
            selection.engine = std::make_unique<NullAsrEngine>();
            break;
        case AsrBackend::Replay: {
            auto replay = std::make_unique<ReplayAsrEngine>();
            selection.engine = std::move(replay);
            selection.degraded = (config.backend == AsrBackend::Auto);
            break;
        }
        case AsrBackend::Whisper:
        case AsrBackend::Auto:
        default:
            selection.engine = std::make_unique<WhisperCppEngine>();
            break;
    }

    Result<void> init = selection.engine->initialize(modelCfg);
    if (!init.ok()) {
        if (config.backend == AsrBackend::Whisper) {
            return fail(init.code(), "whisper 后端初始化失败: " + init.message());
        }
        // 自动模式下初始化失败 → 降级为回放
        MM_LOG_WARN("asr.factory") << "后端初始化失败（" << init.message() << "），降级为回放引擎";
        auto replay = std::make_unique<ReplayAsrEngine>();
        AsrModelConfig fallbackCfg = modelCfg;
        Result<void> fallbackInit = replay->initialize(fallbackCfg);
        if (!fallbackInit.ok()) {
            return fail(fallbackInit.code(), fallbackInit.message());
        }
        selection.engine = std::move(replay);
        selection.reason = "首选项初始化失败，已降级为回放引擎：" + init.message();
        selection.degraded = true;
    }

    MM_LOG_INFO("asr.factory") << "选用后端: " << selection.engine->displayName()
                               << "（" << selection.reason << "）";
    return selection;
}

}  // namespace mm::asr

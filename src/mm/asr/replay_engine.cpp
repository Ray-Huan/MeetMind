#include "mm/asr/replay_engine.h"

#include <filesystem>
#include <fstream>

#include "mm/common/path_utils.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::asr {

void ReplayAsrEngine::setScript(std::vector<std::string> lines) {
    script_ = std::move(lines);
    cursor_ = 0;
}

Result<void> ReplayAsrEngine::loadScript(const std::string& path) {
    if (!pathutil::exists(path)) {
        return fail(ErrorCode::FileNotFound, "脚本文件不存在: " + path);
    }
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::IoError, "无法打开脚本文件: " + path);

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = str::trim(line);
        if (trimmed.empty()) {
            // 空行 = 显式的「该段无语音」占位
            lines.emplace_back();
            continue;
        }
        if (trimmed[0] == '#') continue;  // 注释
        lines.push_back(trimmed);
    }
    if (lines.empty()) {
        return fail(ErrorCode::InvalidArgument, "脚本文件不包含任何有效行: " + path);
    }
    script_ = std::move(lines);
    cursor_ = 0;
    MM_LOG_INFO("asr.replay") << "已加载脚本 " << path << " 共 " << script_.size() << " 行";
    return okStatus();
}

AsrCapabilities ReplayAsrEngine::capabilities() const {
    AsrCapabilities c;
    c.wordTimestamps = false;
    c.streaming = false;
    c.multilingual = true;
    c.onDevice = true;
    c.requiresModelFile = false;
    c.description = "脚本回放引擎：按顺序输出预置文本，时间戳跟随输入音频块";
    return c;
}

Result<void> ReplayAsrEngine::initialize(const AsrModelConfig& config) {
    if (!config.replayScriptPath.empty()) {
        Result<void> loaded = loadScript(config.replayScriptPath);
        if (!loaded.ok()) return loaded;
    }
    initialized_ = true;
    return okStatus();
}

Result<AsrSegment> ReplayAsrEngine::transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                               const CancelToken* cancel) {
    if (cancel && cancel->isCancelled()) {
        return fail(ErrorCode::Cancelled, "转写已取消");
    }
    AsrSegment seg;
    seg.startMs = chunkStartMs;
    seg.endMs = chunkStartMs + chunk.durationMs();
    seg.confidence = 1.0f;
    seg.noSpeechProb = 0.0f;

    if (script_.empty()) {
        return seg;  // 无脚本：返回空文本段，保证链路可跑通
    }

    if (cursor_ >= script_.size()) {
        if (!loop_) {
            // 脚本耗尽：继续返回空段
            return seg;
        }
        cursor_ = 0;
    }
    seg.text = script_[cursor_++];
    // 脚本行完全确定时给出确定置信度；空文本视为非语音
    seg.confidence = seg.text.empty() ? 0.0f : 0.95f;
    seg.noSpeechProb = seg.text.empty() ? 1.0f : 0.0f;
    return seg;
}

}  // namespace mm::asr

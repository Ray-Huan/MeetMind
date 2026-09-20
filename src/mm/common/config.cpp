#include "mm/common/path_utils.h"
#include "mm/common/config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#include "mm/common/string_utils.h"

namespace mm {
namespace fs = std::filesystem;

const char* toString(AsrBackend b) noexcept {
    switch (b) {
        case AsrBackend::Auto: return "auto";
        case AsrBackend::Whisper: return "whisper";
        case AsrBackend::Replay: return "replay";
        case AsrBackend::Null: return "null";
    }
    return "auto";
}

AsrBackend asrBackendFromString(const std::string& s, AsrBackend fallback) {
    const std::string v = str::toLowerAscii(str::trim(s));
    if (v == "auto") return AsrBackend::Auto;
    if (v == "whisper" || v == "whispercpp" || v == "whisper.cpp") return AsrBackend::Whisper;
    if (v == "replay" || v == "script") return AsrBackend::Replay;
    if (v == "null" || v == "none") return AsrBackend::Null;
    return fallback;
}

timeutil::Date Config::effectiveMeetingDate() const {
    const timeutil::Date parsed = timeutil::parseYmd(meetingDate);
    return parsed.valid ? parsed : timeutil::today();
}

int Config::effectiveThreads() const {
    if (threads > 0) return threads;
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned candidate = hw == 0 ? 4u : hw / 2u;
    return static_cast<int>(std::max(2u, candidate));
}

int Config::effectiveSpeakerCount() const {
    if (speakerMode != SpeakerMode::Fixed) return 0;
    return std::max(1, std::min(8, speakerCount));
}

Json Config::toJson() const {
    Json j = Json::object();
    j.set("inputPath", inputPath);
    j.set("outputDir", outputDir);
    j.set("title", title);
    j.set("meetingDate", meetingDate);

    j.set("backend", std::string(toString(backend)));
    j.set("modelPath", modelPath);
    j.set("replayScriptPath", replayScriptPath);
    j.set("language", language);
    j.set("threads", threads);
    j.set("useGpu", useGpu);
    j.set("asrTemperatureInc", asrTemperatureInc);
    j.set("enableItn", enableItn);
    j.set("enablePunctuation", enablePunctuation);
    j.set("enableT2s", enableT2s);

    j.set("vadThresholdDeltaDb", vadThresholdDeltaDb);
    j.set("vadNoiseFloorPercentile", vadNoiseFloorPercentile);
    j.set("minSpeechMs", minSpeechMs);
    j.set("minSilenceMs", minSilenceMs);
    j.set("maxSegmentMs", maxSegmentMs);
    j.set("vadPadMs", vadPadMs);
    j.set("transcriptionUnitMs", transcriptionUnitMs);
    j.set("unitMergeGapMs", unitMergeGapMs);

    j.set("speakerMode", std::string(speakerMode == SpeakerMode::Fixed ? "fixed" : "auto"));
    j.set("speakerCount", speakerCount);
    j.set("diarMergeThreshold", diarMergeThreshold);
    j.set("enableDiarization", enableDiarization);

    j.set("enableSummarization", enableSummarization);
    j.set("summaryRatio", summaryRatio);
    j.set("summaryMaxSentences", summaryMaxSentences);
    j.set("maxKeywords", maxKeywords);
    j.set("maxActionItems", maxActionItems);

    j.set("exportMarkdown", exportMarkdown);
    j.set("exportJson", exportJson);
    j.set("exportSrt", exportSrt);
    j.set("exportText", exportText);
    j.set("exportHtml", exportHtml);
    j.set("baseFileName", baseFileName);

    j.set("logLevel", std::string(toString(logLevel)));
    j.set("quiet", quiet);
    j.set("lexiconPath", lexiconPath);
    j.set("stopwordsPath", stopwordsPath);
    return j;
}

Config Config::fromJson(const Json& j) {
    Config c;
    c.inputPath = j.getString("inputPath", c.inputPath);
    c.outputDir = j.getString("outputDir", c.outputDir);
    c.title = j.getString("title", c.title);
    c.meetingDate = j.getString("meetingDate", c.meetingDate);

    c.backend = asrBackendFromString(j.getString("backend", "auto"));
    c.modelPath = j.getString("modelPath", c.modelPath);
    c.replayScriptPath = j.getString("replayScriptPath", c.replayScriptPath);
    c.language = j.getString("language", c.language);
    c.threads = j.getInt("threads", c.threads);
    c.useGpu = j.getBool("useGpu", c.useGpu);
    c.asrTemperatureInc = j.getDouble("asrTemperatureInc", c.asrTemperatureInc);
    c.enableItn = j.getBool("enableItn", c.enableItn);
    c.enablePunctuation = j.getBool("enablePunctuation", c.enablePunctuation);
    c.enableT2s = j.getBool("enableT2s", c.enableT2s);

    c.vadThresholdDeltaDb = j.getDouble("vadThresholdDeltaDb", c.vadThresholdDeltaDb);
    c.vadNoiseFloorPercentile = j.getDouble("vadNoiseFloorPercentile", c.vadNoiseFloorPercentile);
    c.minSpeechMs = j.getInt("minSpeechMs", c.minSpeechMs);
    c.minSilenceMs = j.getInt("minSilenceMs", c.minSilenceMs);
    c.maxSegmentMs = j.getInt("maxSegmentMs", c.maxSegmentMs);
    c.vadPadMs = j.getInt("vadPadMs", c.vadPadMs);
    c.transcriptionUnitMs = j.getInt("transcriptionUnitMs", c.transcriptionUnitMs);
    c.unitMergeGapMs = j.getInt("unitMergeGapMs", c.unitMergeGapMs);

    const std::string sm = str::toLowerAscii(j.getString("speakerMode", "auto"));
    c.speakerMode = (sm == "fixed") ? SpeakerMode::Fixed : SpeakerMode::Auto;
    c.speakerCount = j.getInt("speakerCount", c.speakerCount);
    c.diarMergeThreshold = j.getDouble("diarMergeThreshold", c.diarMergeThreshold);
    c.enableDiarization = j.getBool("enableDiarization", c.enableDiarization);

    c.enableSummarization = j.getBool("enableSummarization", c.enableSummarization);
    c.summaryRatio = j.getDouble("summaryRatio", c.summaryRatio);
    c.summaryMaxSentences = j.getInt("summaryMaxSentences", c.summaryMaxSentences);
    c.maxKeywords = j.getInt("maxKeywords", c.maxKeywords);
    c.maxActionItems = j.getInt("maxActionItems", c.maxActionItems);

    c.exportMarkdown = j.getBool("exportMarkdown", c.exportMarkdown);
    c.exportJson = j.getBool("exportJson", c.exportJson);
    c.exportSrt = j.getBool("exportSrt", c.exportSrt);
    c.exportText = j.getBool("exportText", c.exportText);
    c.exportHtml = j.getBool("exportHtml", c.exportHtml);
    c.baseFileName = j.getString("baseFileName", c.baseFileName);

    c.logLevel = logLevelFromString(j.getString("logLevel", "info"), c.logLevel);
    c.quiet = j.getBool("quiet", c.quiet);
    c.lexiconPath = j.getString("lexiconPath", c.lexiconPath);
    c.stopwordsPath = j.getString("stopwordsPath", c.stopwordsPath);
    return c;
}

std::string Config::defaultConfigPath() {
#if defined(_WIN32)
    const char* appData = std::getenv("APPDATA");
    if (appData && *appData) {
        return pathutil::joinPath(appData, "MeetMind", "config.json");
    }
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return pathutil::joinPath(userProfile, ".meetmind", "config.json");
    }
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return pathutil::joinPath(home, ".config", "MeetMind", "config.json");
    }
#endif
    return "meetmind_config.json";
}

Config Config::loadOrDefault(const std::string& path) {
    const std::string p = path.empty() ? defaultConfigPath() : path;
    if (!pathutil::exists(p)) {
        MM_LOG_INFO("config") << "配置文件不存在，使用默认配置: " << p;
        return Config{};
    }
    Result<Json> parsed = Json::parseFile(p);
    if (!parsed.ok()) {
        MM_LOG_WARN("config") << "配置文件解析失败，回退默认配置: " << parsed.message();
        return Config{};
    }
    return fromJson(parsed.value());
}

Result<Config> Config::load(const std::string& path) {
    const std::string p = path.empty() ? defaultConfigPath() : path;
    Result<Json> parsed = Json::parseFile(p);
    if (!parsed.ok()) return fail(parsed.code(), parsed.message());
    return fromJson(parsed.value());
}

Result<void> Config::save(const std::string& path) const {
    const std::string p = path.empty() ? defaultConfigPath() : path;
    return toJson().writeFile(p, 2);
}

std::vector<std::string> Config::validate() const {
    std::vector<std::string> notes;
    auto clampInt = [&](const char* name, int value, int lo, int hi) {
        if (value < lo || value > hi) {
            notes.push_back(std::string(name) + " 超出范围 [" + std::to_string(lo) + "," +
                            std::to_string(hi) + "]，已按边界处理");
            return true;
        }
        return false;
    };
    clampInt("minSpeechMs", minSpeechMs, 20, 5000);
    clampInt("minSilenceMs", minSilenceMs, 50, 10000);
    clampInt("maxSegmentMs", maxSegmentMs, 1000, 120000);
    clampInt("transcriptionUnitMs", transcriptionUnitMs, 0, 30000);
    clampInt("unitMergeGapMs", unitMergeGapMs, 0, 10000);
    if (asrTemperatureInc < 0.0 || asrTemperatureInc > 1.0) {
        notes.push_back("asrTemperatureInc 超出 [0,1]，将使用 0.2");
    }
    clampInt("summaryMaxSentences", summaryMaxSentences, 1, 60);
    clampInt("maxKeywords", maxKeywords, 1, 60);
    clampInt("maxActionItems", maxActionItems, 1, 200);
    if (summaryRatio <= 0.0 || summaryRatio > 1.0) {
        notes.push_back("summaryRatio 超出 (0,1]，将使用 0.25");
    }
    if (diarMergeThreshold < 0.01 || diarMergeThreshold > 1.0) {
        notes.push_back("diarMergeThreshold 超出 [0.01,1]，将使用 0.35");
    }
    if (speakerMode == SpeakerMode::Fixed && (speakerCount < 1 || speakerCount > 8)) {
        notes.push_back("speakerCount 超出 [1,8]，将使用 2");
    }
    // 注：这里刻意不对 useGpu 发表意见。useGpu 默认为 true，是否真的能用上显卡
    // 取决于「构建时有没有编入 GPU 后端」，而 common 层看不到 asr 层，无法静态判断；
    // 若在此提示，CPU 版构建会每次运行都刷一条，变成噪音。
    // 实际设备由 initialize() 判定后写进日志（推理设备: GPU/CPU），
    // 并在 CLI 侧结合后端可用性给出更准确的提示。
    return notes;
}

std::string Config::locateDataDir(const std::string& exeDir) {
    std::vector<std::string> candidates;
    if (!exeDir.empty()) {
        candidates.push_back(pathutil::joinPath(exeDir, "data"));
        candidates.push_back(pathutil::joinPath(exeDir, "..", "data"));
        candidates.push_back(pathutil::joinPath(exeDir, "..", "..", "data"));
    }
    const std::string cwd = pathutil::toUtf8(std::filesystem::current_path());
    if (!cwd.empty()) {
        candidates.push_back(pathutil::joinPath(cwd, "data"));
        candidates.push_back(pathutil::joinPath(cwd, "..", "data"));
    }
    for (const std::string& c : candidates) {
        if (pathutil::exists(pathutil::join(c, "lexicon_zh.txt"))) {
            return pathutil::canonical(c);
        }
    }
    return {};
}

bool Config::offlineForced() {
    const char* v = std::getenv("MEETMIND_OFFLINE");
    if (!v || !*v) return false;
    const std::string s = str::toLowerAscii(str::trim(v));
    return !(s == "0" || s == "false" || s == "no" || s == "off");
}

}  // namespace mm

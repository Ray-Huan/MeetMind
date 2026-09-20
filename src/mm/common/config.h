// MeetMind — 运行时配置
// 所有可调参数集中于此，支持 JSON 持久化与默认值兜底。
#pragma once

#include <string>
#include <vector>

#include "mm/common/json.h"
#include "mm/common/logger.h"
#include "mm/common/result.h"
#include "mm/common/time_utils.h"

namespace mm {

/// ASR 后端类型
enum class AsrBackend {
    Auto,    ///< 自动：whisper 可用则用，否则退化为回放引擎
    Whisper, ///< 强制 whisper.cpp（不可用时直接报错）
    Replay,  ///< 回放脚本模式（无模型环境/回归测试）
    Null,    ///< 空实现（仅跑前端链路）
};

const char* toString(AsrBackend b) noexcept;
AsrBackend asrBackendFromString(const std::string& s, AsrBackend fallback = AsrBackend::Auto);

/// 说话人数量设定
enum class SpeakerMode { Auto, Fixed };

class Config {
public:
    // ---- 输入 / 输出 ----
    std::string inputPath;              ///< 由命令行或 GUI 填入
    std::string outputDir;              ///< 导出目录（空 = 与输入同目录）
    std::string title;                  ///< 会议标题（空 = 自动推导）
    std::string meetingDate;            ///< 会议日期 "YYYY-MM-DD"（空 = 今天），用于待办截止日推算

    // ---- ASR ----
    AsrBackend backend = AsrBackend::Auto;
    std::string modelPath;              ///< whisper ggml 模型文件路径
    std::string replayScriptPath;       ///< 回放后端脚本路径（backend=replay 时生效）
    std::string language = "auto";      ///< "auto" / "zh" / "en"
    int threads = 0;                    ///< 0 = 自动
    /// 是否使用 GPU 推理。
    ///
    /// 默认 **true**：本构建若含 CUDA 后端就直接走显卡（实测约 40× 实时），
    /// 未含 GPU 后端时**静默回退 CPU**（属构建选择，不算故障，因此只记 INFO、不告警）。
    /// 想强制走 CPU 时用 `--no-gpu`，或在配置文件里设 `"useGpu": false`。
    bool useGpu = true;
    /// whisper 的温度回退步长。0 表示关闭回退（不重试）。
    /// 实测：真实嘈杂多人对话上，回退会反复重解码同一窗口，
    /// 是「识别阶段耗时远超音频时长」的主因；关闭可换取数倍吞吐。
    double asrTemperatureInc = 0.2;
    bool enableItn = true;              ///< 逆文本正则化
    bool enablePunctuation = true;      ///< 标点恢复
    /// 繁转简：whisper 等模型对中文常输出繁体，统一转简体可显著提升纪要质量
    bool enableT2s = true;

    // ---- VAD ----
    double vadThresholdDeltaDb = 0.0;   ///< 0 = 自适应
    double vadNoiseFloorPercentile = 0.10;
    int minSpeechMs = 200;
    int minSilenceMs = 300;
    int maxSegmentMs = 30000;
    int vadPadMs = 200;
    /// 转写单元时长上限（毫秒）。whisper 按固定 30 秒窗口编码，把相邻语音段
    /// 聚合成接近窗口长度的单元可显著降低固定开销；设为 0 表示逐段转写。
    /// 实测（60 秒真实对话片段，base 模型）：25000 → 14.3s，29000 → 12.1s。
    /// 取 29000 是因为它刚好贴着 whisper 的 30 秒窗口，再往上会被窗口边界截断。
    int transcriptionUnitMs = 29000;
    /// 允许合并为同一转写单元的最大段间隔（毫秒）。
    /// 实测：会议语音的自然句间停顿约 1.5–1.8 秒，若阈值低于该值则几乎不会合并，
    /// 优化失效（实测 RTF 0.47× → 1.97×，见 docs/04-测试计划与报告.md）。
    int unitMergeGapMs = 2500;

    // ---- 说话人分离 ----
    SpeakerMode speakerMode = SpeakerMode::Auto;
    int speakerCount = 0;               ///< speakerMode == Fixed 时生效（1..8）
    double diarMergeThreshold = 0.35;
    bool enableDiarization = true;

    // ---- 纪要 ----
    bool enableSummarization = true;
    double summaryRatio = 0.25;         ///< 摘要压缩比
    int summaryMaxSentences = 12;
    int maxKeywords = 12;
    int maxActionItems = 30;

    // ---- 导出 ----
    bool exportMarkdown = true;
    bool exportJson = true;
    bool exportSrt = true;
    bool exportText = true;
    bool exportHtml = false;
    std::string baseFileName;           ///< 导出文件名主干（空 = 由标题推导）

    // ---- 运行时 ----
    LogLevel logLevel = LogLevel::Info;
    bool quiet = false;

    // ---- 数据文件 ----
    std::string lexiconPath;            ///< 中文词典（空 = 自动探测 data/lexicon_zh.txt）
    std::string stopwordsPath;          ///< 停用词表

    /// 生效的会议日期（未设置则取今天）。
    timeutil::Date effectiveMeetingDate() const;

    /// 生效的线程数。
    int effectiveThreads() const;

    /// 固定人数模式下的目标人数（0 表示自适应）。
    int effectiveSpeakerCount() const;

    Json toJson() const;
    static Config fromJson(const Json& j);

    /// 默认配置路径：%APPDATA%/MeetMind/config.json 或 ~/.config/MeetMind/config.json
    static std::string defaultConfigPath();

    /// 加载配置；文件不存在时返回默认配置并附带警告日志。
    static Config loadOrDefault(const std::string& path);

    /// 加载配置；文件不存在视为错误。
    static Result<Config> load(const std::string& path);

    Result<void> save(const std::string& path) const;

    /// 完整性校验与自动修正；返回修正说明列表（空表示无问题）。
    std::vector<std::string> validate() const;

    /// 数据目录探测：依次尝试 <exe>/data、<cwd>/data、<cwd>/../data。
    static std::string locateDataDir(const std::string& exeDir = {});

    /// 强制离线模式（环境变量 MEETMIND_OFFLINE=1）。
    static bool offlineForced();
};

}  // namespace mm

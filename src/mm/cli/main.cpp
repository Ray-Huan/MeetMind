// MeetMind 命令行入口
// 退出码：0 成功 / 1 运行期错误 / 2 参数错误 / 3 用户取消 / 70 内部错误（含崩溃）
//   注意：70 是刻意与 3 区分开的 —— 曾经因为进程 abort 恰好返回 3，
//         导致「崩溃」被误读成「用户取消」。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mm/asr/asr_factory.h"
#include "mm/asr/whisper_engine.h"
#include "mm/common/config.h"
#include "mm/common/crash_guard.h"
#include "mm/common/logger.h"
#include "mm/common/path_utils.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "mm/live/audio_source.h"
#include "mm/live/realtime_transcriber.h"
#include "mm/pipeline/exporter.h"
#include "mm/pipeline/pipeline.h"
#include "mm/storage/session_store.h"

#if defined(_WIN32)
#include <windows.h>   // shellapi.h 依赖 windows.h 的基础类型，顺序不可颠倒
#include <shellapi.h>
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRuntime = 1;
constexpr int kExitUsage = 2;
constexpr int kExitCancelled = 3;

namespace fs = std::filesystem;
using mm::Config;
using mm::ErrorCode;
using mm::Result;

struct CliOptions {
    std::vector<std::string> inputs;
    std::vector<std::string> formats;
    std::string batchDir;
    std::string configPath;
    bool saveConfig = false;
    bool showConfig = false;
    bool listEngines = false;
    bool noProgress = false;
    bool printJson = false;
    bool dryRun = false;
    bool showHelp = false;
    bool showVersion = false;
    /// 命令行是否显式给出了 --gpu / --no-gpu。
    /// 未显式给出时应沿用配置文件里的 useGpu，而不是用默认值把它覆盖掉。
    bool gpuSet = false;
    // 实时（流式）转写
    bool realtime = false;
    double speed = 1.0;        ///< 文件模拟实时流时的倍速
    int rtSegmentMs = 12000;   ///< 实时单段上限（控制时延）
    int rtSilenceMs = 600;     ///< 判句停顿
    Config config;
};

void printUsage() {
    std::cout <<
        "MeetMind — 端侧语音转写与智能会议纪要工具\n"
        "\n"
        "用法:\n"
        "  meetmind <音频.wav> [更多音频...] [选项]\n"
        "  meetmind --batch <目录> [选项]\n"
        "\n"
        "输入输出:\n"
        "  -o, --out <目录>        导出目录（默认与输入文件同目录）\n"
        "  -n, --name <主干名>     导出文件名主干（默认由音频文件名 + 日期推导）\n"
        "  -t, --title <标题>      会议标题（默认自动推导）\n"
        "  -d, --date <YYYY-MM-DD> 会议日期（用于待办截止时间推算，默认今天）\n"
        "      --formats <列表>    导出格式，逗号分隔：md,json,srt,txt,html\n"
        "\n"
        "识别引擎:\n"
        "      --backend <类型>    auto | whisper | replay | null（默认 auto）\n"
        "  -m, --model <路径>      whisper ggml 模型文件\n"
        "  -l, --lang <语言>       auto | zh | en（默认 auto）\n"
        "  -j, --threads <N>       推理线程数（默认自动）\n"
        "      --script <路径>     回放模式脚本文件（backend=replay 时使用）\n"
        "      --list-engines      列出可用识别后端及其状态（含 GPU 后端探测）\n"
        "      --gpu               请求 GPU 推理（若未编入 GPU 后端会明确告警并回退 CPU）\n"
        "      --no-gpu            强制使用 CPU\n"
        "                          （两者都不给时按配置文件里的 useGpu 决定，默认 CPU）\n"
        "\n"
        "实时转写:\n"
        "      --realtime          实时（流式）模式：边进音频边出稿，逐段打印\n"
        "      --speed <n>         文件模拟实时流的倍速（默认 1.0；10 = 十倍速压测）\n"
        "      --rt-segment <ms>   实时单段时长上限（默认 12000；越小出稿越勤、时延越低）\n"
        "      --rt-silence <ms>   判句停顿阈值（默认 600；说话人停顿超过它才断句）\n"
        "\n"
        "处理开关:\n"
        "      --no-diar           关闭说话人分离\n"
        "      --speakers <N>      指定说话人数（1-8，默认自适应）\n"
        "      --no-summary        跳过摘要与关键词生成\n"
        "      --no-itn            关闭数字/单位规范化\n"
        "      --no-punct          关闭标点恢复\n"
        "      --min-speech <ms>   最小语音段长度（默认 200）\n"
        "      --max-segment <ms>  单段转写上限（默认 30000）\n"
        "      --unit-ms <ms>      转写单元时长上限（默认 25000；0 = 逐段转写）\n"
        "      --unit-gap <ms>     允许合并为同一单元的最大段间隔（默认 2500）\n"
        "      --temp-inc <f>      whisper 温度回退步长，0 = 关闭回退（默认 0.2）\n"
        "\n"
        "运行控制:\n"
        "      --config <路径>     指定配置文件\n"
        "      --show-config       打印当前生效配置\n"
        "      --save-config       把当前参数写回配置文件\n"
        "      --json              以 JSON 形式输出处理报告\n"
        "      --no-progress       不显示进度条\n"
        "      --dry-run           仅解析参数与校验输入，不执行处理\n"
        "  -v, --verbose           输出调试日志\n"
        "  -q, --quiet             静默模式（仅错误）\n"
        "  -h, --help              显示帮助\n"
        "      --version           显示版本\n"
        "\n"
        "示例:\n"
        "  meetmind meeting.wav -o out --formats md,json,srt\n"
        "  meetmind meeting.wav -m models/ggml-small.bin -l zh --speakers 3\n"
        "  meetmind --batch ./recordings --out ./minutes\n";
}

void printVersion() {
    std::cout << "MeetMind " << MEETMIND_VERSION << " (C++17, 端侧离线)\n";
    const bool whisper = mm::asr::WhisperCppEngine::compiledIn();
    std::cout << "  识别后端支持: " << (whisper ? "whisper.cpp 已启用" : "未编译 whisper.cpp（回放模式）")
              << "\n";
}

/// @return 是否解析成功
bool parseArgs(int argc, char** argv, CliOptions& opt, std::string& error) {
    auto needValue = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) {
            error = std::string("参数 ") + flag + " 缺少取值";
            return {};
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.empty()) continue;

        if (arg == "-h" || arg == "--help") {
            opt.showHelp = true;
        } else if (arg == "--version") {
            opt.showVersion = true;
        } else if (arg == "--list-engines") {
            opt.listEngines = true;
        } else if (arg == "--show-config") {
            opt.showConfig = true;
        } else if (arg == "--save-config") {
            opt.saveConfig = true;
        } else if (arg == "--json") {
            opt.printJson = true;
        } else if (arg == "--no-progress") {
            opt.noProgress = true;
        } else if (arg == "--dry-run") {
            opt.dryRun = true;
        } else if (arg == "-o" || arg == "--out") {
            opt.config.outputDir = needValue(i, "--out");
        } else if (arg == "-n" || arg == "--name") {
            opt.config.baseFileName = needValue(i, "--name");
        } else if (arg == "-t" || arg == "--title") {
            opt.config.title = needValue(i, "--title");
        } else if (arg == "-d" || arg == "--date") {
            opt.config.meetingDate = needValue(i, "--date");
        } else if (arg == "-m" || arg == "--model") {
            opt.config.modelPath = needValue(i, "--model");
        } else if (arg == "-l" || arg == "--lang") {
            opt.config.language = needValue(i, "--lang");
        } else if (arg == "-j" || arg == "--threads") {
            const std::string v = needValue(i, "--threads");
            try {
                opt.config.threads = std::stoi(v);
            } catch (...) {
                error = "线程数不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--backend") {
            opt.config.backend = mm::asrBackendFromString(needValue(i, "--backend"));
        } else if (arg == "--script") {
            opt.config.replayScriptPath = needValue(i, "--script");
        } else if (arg == "--formats") {
            const std::string v = needValue(i, "--formats");
            opt.formats = mm::str::splitAny(mm::str::toLowerAscii(v), ",; ");
        } else if (arg == "--batch") {
            opt.batchDir = needValue(i, "--batch");
        } else if (arg == "--config") {
            opt.configPath = needValue(i, "--config");
        } else if (arg == "--no-diar") {
            opt.config.enableDiarization = false;
        } else if (arg == "--speakers") {
            const std::string v = needValue(i, "--speakers");
            try {
                opt.config.speakerCount = std::stoi(v);
            } catch (...) {
                error = "说话人数不是合法整数: " + v;
                return false;
            }
            opt.config.speakerMode = mm::SpeakerMode::Fixed;
        } else if (arg == "--no-summary") {
            opt.config.enableSummarization = false;
        } else if (arg == "--no-itn") {
            opt.config.enableItn = false;
        } else if (arg == "--no-punct") {
            opt.config.enablePunctuation = false;
        } else if (arg == "--min-speech") {
            const std::string v = needValue(i, "--min-speech");
            try {
                opt.config.minSpeechMs = std::stoi(v);
            } catch (...) {
                error = "min-speech 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--max-segment") {
            const std::string v = needValue(i, "--max-segment");
            try {
                opt.config.maxSegmentMs = std::stoi(v);
            } catch (...) {
                error = "max-segment 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--unit-ms") {
            const std::string v = needValue(i, "--unit-ms");
            try {
                opt.config.transcriptionUnitMs = std::stoi(v);
            } catch (...) {
                error = "unit-ms 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--unit-gap") {
            const std::string v = needValue(i, "--unit-gap");
            try {
                opt.config.unitMergeGapMs = std::stoi(v);
            } catch (...) {
                error = "unit-gap 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--temp-inc") {
            const std::string v = needValue(i, "--temp-inc");
            try {
                opt.config.asrTemperatureInc = std::stod(v);
            } catch (...) {
                error = "temp-inc 不是合法数值: " + v;
                return false;
            }
        } else if (arg == "--realtime") {
            opt.realtime = true;
        } else if (arg == "--speed") {
            const std::string v = needValue(i, "--speed");
            try {
                opt.speed = std::stod(v);
            } catch (...) {
                error = "speed 不是合法数值: " + v;
                return false;
            }
            if (opt.speed <= 0.0) {
                error = "speed 必须为正数";
                return false;
            }
        } else if (arg == "--rt-segment") {
            const std::string v = needValue(i, "--rt-segment");
            try {
                opt.rtSegmentMs = std::stoi(v);
            } catch (...) {
                error = "rt-segment 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--rt-silence") {
            const std::string v = needValue(i, "--rt-silence");
            try {
                opt.rtSilenceMs = std::stoi(v);
            } catch (...) {
                error = "rt-silence 不是合法整数: " + v;
                return false;
            }
        } else if (arg == "--gpu") {
            opt.config.useGpu = true;
            opt.gpuSet = true;
        } else if (arg == "--no-gpu") {
            opt.config.useGpu = false;
            opt.gpuSet = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opt.config.logLevel = mm::LogLevel::Debug;
        } else if (arg == "-q" || arg == "--quiet") {
            opt.config.quiet = true;
            opt.config.logLevel = mm::LogLevel::Error;
        } else if (!arg.empty() && arg[0] == '-') {
            error = "未知参数: " + arg;
            return false;
        } else {
            opt.inputs.push_back(arg);
        }
    }
    return true;
}

std::vector<mm::pipeline::ExportFormat> parseFormats(const std::vector<std::string>& names,
                                                     bool* ok) {
    std::vector<mm::pipeline::ExportFormat> out;
    if (ok) *ok = true;
    for (const std::string& n : names) {
        if (n == "md" || n == "markdown") {
            out.push_back(mm::pipeline::ExportFormat::Markdown);
        } else if (n == "json") {
            out.push_back(mm::pipeline::ExportFormat::Json);
        } else if (n == "srt") {
            out.push_back(mm::pipeline::ExportFormat::Srt);
        } else if (n == "txt" || n == "text") {
            out.push_back(mm::pipeline::ExportFormat::Text);
        } else if (n == "html") {
            out.push_back(mm::pipeline::ExportFormat::Html);
        } else {
            if (ok) *ok = false;
        }
    }
    return out;
}

void printProgressBar(const mm::pipeline::ProgressInfo& p) {
    if (p.stage == mm::pipeline::Stage::Done) {
        std::printf("\r%-70s\n", "[完成] 全部处理结束");
        std::fflush(stdout);
        return;
    }
    constexpr int kWidth = 28;
    const int filled = static_cast<int>(p.fraction * kWidth);
    std::string bar(static_cast<size_t>(kWidth), '-');
    for (int i = 0; i < filled && i < kWidth; ++i) bar[static_cast<size_t>(i)] = '#';
    std::printf("\r[%s] %3d%%  %-44s", bar.c_str(), static_cast<int>(p.fraction * 100.0),
                p.message.c_str());
    std::fflush(stdout);
}

void printStageReport(const mm::pipeline::PipelineReport& r) {
    std::cout << "\n各阶段耗时:\n";
    for (const auto& t : r.timings) {
        std::printf("  %-14s %8lld ms\n", mm::pipeline::stageLabelCn(t.stage),
                    static_cast<long long>(t.elapsedMs));
    }
    std::printf("  %-14s %8lld ms\n", "总计", static_cast<long long>(r.totalMs));
    if (r.audioMs > 0 && r.totalMs > 0) {
        std::printf("  整体实时倍率: %.2fx（音频 %.1f 秒 / 处理 %.1f 秒）\n",
                    r.overallRealTimeFactor, r.audioMs / 1000.0, r.totalMs / 1000.0);
    }
    if (r.peakRssKb > 0) {
        std::printf("  峰值内存: %.1f MB\n", r.peakRssKb / 1024.0);
    }
    if (r.asrRealTimeFactor > 0) {
        std::printf("  识别阶段实时倍率: %.2fx\n", r.asrRealTimeFactor);
    }
}

int processOne(const std::string& input, const CliOptions& opt, bool showProgress) {
    Config cfg = opt.config;
    cfg.inputPath = input;

    mm::pipeline::Pipeline pipeline(cfg);
    mm::CancelToken cancel;

    mm::pipeline::ProgressCallback cb;
    if (showProgress) {
        cb = [](const mm::pipeline::ProgressInfo& p) { printProgressBar(p); };
    }

    Result<mm::pipeline::PipelineResult> result = pipeline.run(&cancel, cb);
    if (!result.ok()) {
        if (showProgress) std::printf("\n");
        std::cerr << "处理失败(" << mm::toString(result.code()) << "): " << result.message()
                  << "\n";
        return result.code() == ErrorCode::Cancelled ? kExitCancelled : kExitRuntime;
    }

    const mm::pipeline::PipelineResult& r = result.value();

    if (opt.printJson) {
        std::cout << r.report.toJson().dump(2) << "\n";
    } else {
        std::cout << "\n=========== 处理结果 ===========\n";
        std::cout << "标题      : " << r.title << "\n";
        std::cout << "音频      : " << r.quality.toSummary() << "\n";
        std::cout << "识别后端  : " << r.report.asrBackend << "（" << r.report.asrBackendReason
                  << "）\n";
        std::cout << "说话人    : " << r.minutes.stats.speakerCount << " 人，"
                  << r.diarization.turns.size() << " 个发言轮次\n";
        std::cout << "转写字数  : " << r.minutes.stats.cjkCharacters << " 字（"
                  << r.minutes.stats.sentenceCount << " 句）\n";
        std::cout << "关键词    : ";
        for (size_t i = 0; i < r.minutes.keywords.size() && i < 8; ++i) {
            if (i) std::cout << "、";
            std::cout << r.minutes.keywords[i].word;
        }
        std::cout << "\n";
        std::cout << "决议      : " << r.minutes.decisions.size() << " 条\n";
        std::cout << "待办      : " << r.minutes.actionItems.size() << " 条\n";
        if (!r.minutes.overview.empty()) {
            std::cout << "概览      : " << r.minutes.overview << "\n";
        }
        if (!r.minutes.risks.empty()) {
            std::cout << "风险提示  :\n";
            for (const std::string& s : r.minutes.risks) std::cout << "  - " << s << "\n";
        }
        printStageReport(r.report);
        std::cout << "导出文件  :\n";
        for (const std::string& f : r.exportedFiles) std::cout << "  - " << f << "\n";
        std::cout << "================================\n";
    }

    // 落盘会话历史（失败不视为致命，仅告警）
    mm::storage::SessionStore store(cfg.outputDir.empty()
                                        ? std::string{}
                                        : mm::pathutil::join(cfg.outputDir, "sessions"));
    Result<std::string> saved = store.save(r);
    if (!saved.ok()) {
        MM_LOG_WARN("cli") << "会话保存失败: " << saved.message();
    }
    return kExitOk;
}

/// 毫秒 → mm:ss（超过一小时显示 hh:mm:ss）。
std::string formatClock(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t total = ms / 1000;
    const int64_t h = total / 3600;
    const int64_t m = (total % 3600) / 60;
    const int64_t s = total % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
                      static_cast<long long>(h), static_cast<long long>(m),
                      static_cast<long long>(s));
    } else {
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                      static_cast<long long>(m), static_cast<long long>(s));
    }
    return std::string(buf);
}

/// 实时（流式）转写：音频源按实时速率推送 → 在线 VAD 断句 → 推理线程出稿。
/// 当前音频源为「文件模拟实时流」；麦克风来源由 GUI 侧提供（同一 IAudioSource 接口）。
int processRealtime(const std::string& input, const Config& cfg, const CliOptions& opt) {
    Config local = cfg;
    local.inputPath = input;

    Result<mm::asr::EngineSelection> selection = mm::asr::AsrFactory::create(local);
    if (!selection.ok()) {
        std::cerr << "实时转写失败：后端初始化错误(" << mm::toString(selection.code())
                  << "): " << selection.message() << "\n";
        return kExitRuntime;
    }

    // 设备状态必须如实透出：「以为在用显卡、实际在跑 CPU」是最难排查的问题
    if (auto* whisper = dynamic_cast<mm::asr::WhisperCppEngine*>(selection->engine.get())) {
        if (!whisper->gpuWarning().empty()) {
            std::cerr << "警告: " << whisper->gpuWarning() << "\n";
        }
        std::cout << "推理设备: " << (whisper->usingGpu() ? "GPU（CUDA）" : "CPU")
                  << "    后端: " << selection->engine->displayName() << "\n";
    }

    mm::live::FileAudioSource source(input, opt.speed, 100);
    if (!source.loaded()) {
        std::cerr << "音频载入失败: " << source.loadError() << "\n";
        return kExitRuntime;
    }

    mm::live::RealtimeConfig rtCfg;
    rtCfg.vad.sampleRate = source.sampleRate();
    rtCfg.vad.exitFrames = std::max(1, opt.rtSilenceMs / rtCfg.vad.frameShiftMs);
    rtCfg.maxSegmentMs = opt.rtSegmentMs;
    rtCfg.padMs = 200;

    std::cout << "实时参数: 判句停顿 " << opt.rtSilenceMs << " ms，单段上限 " << opt.rtSegmentMs
              << " ms，模拟倍速 " << opt.speed << "×\n";
    std::cout << "（音频 " << formatClock(source.durationMs()) << "，边进边出稿）\n\n";

    mm::live::RealtimeTranscriber transcriber(selection->engine.get(), rtCfg);

    std::mutex printMu;
    transcriber.setCallback([&printMu](const mm::live::RealtimeEvent& ev) {
        std::lock_guard<std::mutex> lock(printMu);
        std::printf("[%s] %s  [延迟 %lld ms]\n", formatClock(ev.segment.startMs).c_str(),
                    ev.segment.text.c_str(), static_cast<long long>(ev.latencyMs));
        std::fflush(stdout);
    });

    Result<void> started = transcriber.start();
    if (!started.ok()) {
        std::cerr << "实时转写器启动失败: " << started.message() << "\n";
        return kExitRuntime;
    }

    const auto wallStart = std::chrono::steady_clock::now();
    Result<void> begun = source.start(
        [&transcriber](const float* samples, size_t count) { transcriber.push(samples, count); });
    if (!begun.ok()) {
        std::cerr << "音频源启动失败: " << begun.message() << "\n";
        transcriber.stop();
        return kExitRuntime;
    }

    // 音频源按实时节奏推送；这里等它推完，再冲刷尾部语音段
    while (source.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    source.stop();
    transcriber.flush();

    const int64_t wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - wallStart)
                               .count();
    const mm::live::RealtimeStats st = transcriber.stats();
    transcriber.stop();

    std::cout << "\n=========== 实时转写统计 ===========\n";
    std::printf("音频时长        : %.1f 秒\n", source.durationMs() / 1000.0);
    std::printf("墙钟耗时        : %.1f 秒（含实时等待）\n", wallMs / 1000.0);
    std::printf("累计推理耗时    : %.1f 秒\n", st.transcribeMs / 1000.0);
    std::printf("推理实时倍率    : %.2f×（>1 表示推理跑得比录音快，可支撑实时）\n",
                st.realTimeFactor);
    std::printf("产出段 / 文本量 : %lld 段 / %lld 字节\n", static_cast<long long>(st.segments),
                static_cast<long long>(st.chars));
    std::printf("平均延迟 / 峰值 : %.0f ms / %.0f ms\n", st.avgLatencyMs, st.maxLatencyMs);
    std::cout << "====================================\n";
    return kExitOk;
}

/// Windows 下 main 收到的 argv 是本地 ANSI 编码，中文路径会变成乱码；
/// 这里改用 GetCommandLineW + CommandLineToArgvW 取宽字符参数再转 UTF-8。
std::vector<std::string> collectUtf8Args(int argc, char** argv) {
#if defined(_WIN32)
    int wideArgc = 0;
    LPWSTR* wideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(wideArgc));
        for (int i = 0; i < wideArgc; ++i) {
            out.push_back(mm::pathutil::wideToUtf8(wideArgv[i]));
        }
        ::LocalFree(wideArgv);
        if (!out.empty()) return out;
    }
#endif
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    return out;
}

}  // namespace

int runMain(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    CliOptions opt;
    std::string error;
    if (!parseArgs(argc, argv, opt, error)) {
        std::cerr << "参数错误: " << error << "\n\n";
        printUsage();
        return kExitUsage;
    }

    if (opt.showHelp) {
        printUsage();
        return kExitOk;
    }
    if (opt.showVersion) {
        printVersion();
        return kExitOk;
    }

    // 配置文件（命令行参数优先）
    Config fileConfig = Config::loadOrDefault(opt.configPath);
    Config merged = fileConfig;
    // 仅当命令行显式给出时覆盖
    if (!opt.config.outputDir.empty()) merged.outputDir = opt.config.outputDir;
    if (!opt.config.baseFileName.empty()) merged.baseFileName = opt.config.baseFileName;
    if (!opt.config.title.empty()) merged.title = opt.config.title;
    if (!opt.config.meetingDate.empty()) merged.meetingDate = opt.config.meetingDate;
    if (!opt.config.modelPath.empty()) merged.modelPath = opt.config.modelPath;
    if (!opt.config.replayScriptPath.empty()) merged.replayScriptPath = opt.config.replayScriptPath;
    if (!opt.config.language.empty() && opt.config.language != "auto") merged.language = opt.config.language;
    if (opt.config.backend != mm::AsrBackend::Auto) merged.backend = opt.config.backend;
    if (opt.config.threads > 0) merged.threads = opt.config.threads;
    // 仅当命令行显式给出 --gpu / --no-gpu 时才覆盖配置文件里的 useGpu，
    // 否则配置文件中的开关会被默认值静默抹掉（曾是「配置文件里开了 GPU 却不生效」的原因）。
    if (opt.gpuSet) merged.useGpu = opt.config.useGpu;
    if (!opt.config.enableDiarization) merged.enableDiarization = false;
    if (!opt.config.enableSummarization) merged.enableSummarization = false;
    if (!opt.config.enableItn) merged.enableItn = false;
    if (!opt.config.enablePunctuation) merged.enablePunctuation = false;
    if (opt.config.speakerMode == mm::SpeakerMode::Fixed) {
        merged.speakerMode = mm::SpeakerMode::Fixed;
        merged.speakerCount = opt.config.speakerCount;
    }
    if (opt.config.minSpeechMs != Config{}.minSpeechMs) merged.minSpeechMs = opt.config.minSpeechMs;
    if (opt.config.maxSegmentMs != Config{}.maxSegmentMs) merged.maxSegmentMs = opt.config.maxSegmentMs;
    // 0 是「逐段转写」的合法取值，因此不能与默认值比较后忽略，需显式判定是否出现在命令行
    for (int k = 1; k < argc; ++k) {
        const std::string a = argv[k];
        if (a == "--unit-ms" || a.rfind("--unit-ms=", 0) == 0) {
            merged.transcriptionUnitMs = opt.config.transcriptionUnitMs;
        } else if (a == "--unit-gap" || a.rfind("--unit-gap=", 0) == 0) {
            merged.unitMergeGapMs = opt.config.unitMergeGapMs;
        } else if (a == "--temp-inc" || a.rfind("--temp-inc=", 0) == 0) {
            merged.asrTemperatureInc = opt.config.asrTemperatureInc;
        }
    }
    merged.logLevel = opt.config.logLevel;
    merged.quiet = opt.config.quiet;

    mm::Logger::instance().setLevel(merged.logLevel);
    mm::Logger::instance().setTimestampEnabled(!merged.quiet);

    // 模型自动探测
    if (merged.modelPath.empty()) {
        const std::string detected = mm::asr::AsrFactory::detectDefaultModel();
        if (!detected.empty()) {
            merged.modelPath = detected;
            MM_LOG_INFO("cli") << "自动探测到模型: " << detected;
        }
    }

    // 输入集合
    std::vector<std::string> inputs = opt.inputs;
    if (!opt.batchDir.empty()) {
        if (!mm::pathutil::isDirectory(opt.batchDir)) {
            std::cerr << "批处理目录不存在或不是目录: " << opt.batchDir << "\n";
            return kExitUsage;
        }
        std::vector<std::string> found = mm::pathutil::listFiles(opt.batchDir, true, ".wav");
        std::sort(found.begin(), found.end());
        inputs.insert(inputs.end(), found.begin(), found.end());
        if (inputs.empty()) {
            std::cerr << "批处理目录中未找到 .wav 文件: " << opt.batchDir << "\n";
            return kExitUsage;
        }
        std::cout << "批处理模式: 发现 " << inputs.size() << " 个音频文件\n";
    }

    if (opt.listEngines) {
        printVersion();
        std::cout << "\n识别后端状态:\n";
        const bool whisper = mm::asr::WhisperCppEngine::compiledIn();
        std::cout << "  whisper : " << (whisper ? "可用（编译已启用）" : "不可用（未编译）") << "\n";
        if (whisper) {
            if (!merged.modelPath.empty() && mm::pathutil::exists(merged.modelPath)) {
                std::cout << "            模型: " << merged.modelPath << "\n";
            } else {
                std::cout << "            模型: 未找到（将自动降级为回放引擎）\n";
            }

            // 后端能力：这是判断「能否用 GPU」的唯一可靠依据
            const std::string sysinfo = mm::asr::WhisperCppEngine::systemInfo();
            const bool gpu = mm::asr::WhisperCppEngine::gpuBackendAvailable();
            std::cout << "            编译期后端: " << (sysinfo.empty() ? "(无信息)" : sysinfo)
                      << "\n";
            std::cout << "            GPU 推理  : "
                      << (gpu ? "可用（默认启用；--no-gpu 可关闭）"
                              : "不可用（本构建未编入 CUDA 等 GPU 后端，将按 CPU 运行）")
                      << "\n";
            std::cout << "            当前设置  : "
                      << (merged.useGpu ? (gpu ? "GPU（CUDA，默认启用）"
                                               : "已启用 GPU，但本构建无 GPU 后端 → 实际按 CPU 运行")
                                        : "CPU（已显式关闭）")
                      << "\n";
            if (!gpu) {
                std::cout << "            启用显卡  : bash tools/build-whisper-cuda.sh\n"
                             "                        bash tools/build.sh gpu\n";
            }
        }
        std::cout << "  replay  : 可用（脚本回放，用于无模型环境）\n";
        std::cout << "  null    : 可用（跳过识别）\n";
        std::cout << "\n当前配置下将使用: ";
        std::string reason;
        const mm::AsrBackend resolved = mm::asr::AsrFactory::resolveBackend(merged, &reason);
        std::cout << mm::toString(resolved) << "\n  原因: " << reason << "\n";
        return kExitOk;
    }

    if (opt.showConfig) {
        std::cout << merged.toJson().dump(2) << "\n";
        const auto notes = merged.validate();
        if (!notes.empty()) {
            std::cout << "\n校验提示:\n";
            for (const std::string& n : notes) std::cout << "  - " << n << "\n";
        }
        if (inputs.empty() && !opt.saveConfig) return kExitOk;
    }

    if (opt.saveConfig) {
        Result<void> saved = merged.save(opt.configPath);
        if (!saved.ok()) {
            std::cerr << "配置保存失败: " << saved.message() << "\n";
            return kExitRuntime;
        }
        std::cout << "配置已保存到 "
                  << (opt.configPath.empty() ? Config::defaultConfigPath() : opt.configPath)
                  << "\n";
        if (inputs.empty()) return kExitOk;
    }

    for (const std::string& n : merged.validate()) {
        MM_LOG_WARN("cli") << "配置提示: " << n;
    }

    // GPU 现已默认启用，因此这里只在「与预期不符」时提示，不刷常态信息。
    if (!merged.quiet) {
        const bool gpuReady = mm::asr::WhisperCppEngine::gpuBackendAvailable();
        if (merged.useGpu && !gpuReady) {
            // CPU 版构建：如实说明按 CPU 跑，并给出启用 GPU 的命令
            std::cout << "提示: 本构建未包含 CUDA 后端，本次按 CPU 推理。"
                         "需要 GPU 加速请执行： bash tools/build-whisper-cuda.sh && bash tools/build.sh gpu\n";
        } else if (!merged.useGpu && gpuReady && !opt.gpuSet) {
            // 关闭来自配置文件（而非命令行）时才提醒，避免用户刚打完 --no-gpu 又被劝回去
            std::cout << "提示: 配置文件中 useGpu=false，当前按 CPU 推理；"
                         "本构建其实含 CUDA 后端，改回 true 即可提速约 16 倍。\n";
        }
    }

    if (inputs.empty()) {
        std::cerr << "未指定输入音频文件。使用 --help 查看用法。\n";
        return kExitUsage;
    }

    // 校验输入
    for (const std::string& in : inputs) {
        if (!mm::pathutil::exists(in)) {
            std::cerr << "输入文件不存在: " << in << "\n";
            return kExitUsage;
        }
    }

    if (opt.dryRun) {
        std::cout << "dry-run: 参数校验通过，共 " << inputs.size() << " 个输入，未执行处理。\n";
        return kExitOk;
    }

    // 实时（流式）模式：与批处理互斥，逐段出稿而非整段处理后导出
    if (opt.realtime) {
        int worstRt = kExitOk;
        for (const std::string& in : inputs) {
            if (inputs.size() > 1) {
                std::cout << "\n--- 实时转写: " << in << " ---\n";
            }
            const int rc = processRealtime(in, merged, opt);
            if (rc != kExitOk) worstRt = rc;
        }
        return worstRt;
    }

    // 导出格式解析
    bool formatsOk = true;
    const std::vector<mm::pipeline::ExportFormat> formats = parseFormats(opt.formats, &formatsOk);
    if (!formatsOk) {
        std::cerr << "存在未知导出格式，可选: md,json,srt,txt,html\n";
        return kExitUsage;
    }
    if (!formats.empty()) {
        merged.exportMarkdown = false;
        merged.exportJson = false;
        merged.exportSrt = false;
        merged.exportText = false;
        merged.exportHtml = false;
        for (mm::pipeline::ExportFormat f : formats) {
            switch (f) {
                case mm::pipeline::ExportFormat::Markdown: merged.exportMarkdown = true; break;
                case mm::pipeline::ExportFormat::Json: merged.exportJson = true; break;
                case mm::pipeline::ExportFormat::Srt: merged.exportSrt = true; break;
                case mm::pipeline::ExportFormat::Text: merged.exportText = true; break;
                case mm::pipeline::ExportFormat::Html: merged.exportHtml = true; break;
            }
        }
    }

    const bool showProgress = !merged.quiet && !opt.printJson && inputs.size() == 1 &&
                              !(mm::Logger::instance().hasFile());

    int worst = kExitOk;
    int index = 0;
    for (const std::string& in : inputs) {
        ++index;
        if (inputs.size() > 1) {
            std::cout << "\n--- [" << index << "/" << inputs.size() << "] " << in << " ---\n";
        }
        CliOptions one = opt;
        one.config = merged;
        const int rc = processOne(in, one, showProgress);
        if (rc == kExitCancelled) return kExitCancelled;
        if (rc != kExitOk) worst = rc;
    }

    if (inputs.size() > 1) {
        std::cout << "\n批处理结束：共 " << inputs.size() << " 个文件，"
                  << (worst == kExitOk ? "全部成功" : "存在失败项") << "\n";
    }
    return worst;
}

int main(int argc, char** argv) {
    // 先装崩溃兜底：未捕获异常也会以 70 退出并留下日志，
    // 不再出现「崩溃被当成用户取消」这种误导性结果。
    mm::installTerminateHandler(mm::kExitInternalError);
    std::vector<std::string> args = collectUtf8Args(argc, argv);
    std::vector<char*> argvUtf8;
    argvUtf8.reserve(args.size());
    for (std::string& a : args) argvUtf8.push_back(a.data());
    return mm::runGuarded([&]() {
        return runMain(static_cast<int>(argvUtf8.size()), argvUtf8.data());
    });
}

// 流水线 / 导出 / 会话存储 集成测试
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mm/audio/wav_io.h"
#include "mm/common/config.h"
#include "mm/common/string_utils.h"
#include "mm/pipeline/exporter.h"
#include "mm/pipeline/pipeline.h"
#include "mm/storage/session_store.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;
namespace fs = std::filesystem;

namespace {

/// 会议脚本：与音频段一一对应
std::vector<std::string> meetingScript() {
    return {
        "今天我们先过一下本周的项目排期。",
        "排期的主要风险是测试资源不足。",
        "这个由李四负责跟进测试资源。",
        "我们决定把上线时间调整到十月八日。",
        "请张三在九月二十日前提交测试方案。",
        "下周我们再同步一次进展。",
    };
}

/// 构造 6 段语音的会议音频并写入 WAV
std::string makeMeetingWav(const std::string& name, int sampleRate = 16000) {
    AudioBuffer clip = mmtest::makeAlternatingClip(1200, 800, 6, 300, 300, sampleRate, 0.25);
    const std::string path = mmtest::outPath("pipeline/" + name);
    Result<void> w = wav::write16(path, clip);
    MM_EXPECT_TRUE(w.ok());
    return path;
}

Config baseConfig(const std::string& wavPath, const std::string& outDir) {
    Config cfg;
    cfg.backend = AsrBackend::Replay;
    cfg.inputPath = wavPath;
    cfg.outputDir = outDir;
    cfg.replayScriptPath.clear();
    cfg.lexiconPath = mmtest::dataPath("lexicon_zh.txt");
    cfg.stopwordsPath = mmtest::dataPath("stopwords_zh.txt");
    cfg.meetingDate = "2026-09-14";
    cfg.enableDiarization = true;
    cfg.speakerMode = SpeakerMode::Fixed;
    cfg.speakerCount = 2;
    cfg.logLevel = LogLevel::Error;
    // 回放引擎是「一次调用产出一条脚本行」，因此测试里关闭转写单元分组，
    // 保持「脚本行 ↔ 语音段」一一对应。分组行为由 pipeline.转写单元分组 单独覆盖。
    cfg.transcriptionUnitMs = 0;
    return cfg;
}

}  // namespace

// ============================ 导出 ============================

MM_TEST(export, 格式枚举与扩展名) {
    MM_EXPECT_EQ(std::string(pipeline::toString(pipeline::ExportFormat::Markdown)),
                 std::string("markdown"));
    MM_EXPECT_EQ(std::string(pipeline::fileExtension(pipeline::ExportFormat::Markdown)),
                 std::string(".md"));
    MM_EXPECT_EQ(std::string(pipeline::fileExtension(pipeline::ExportFormat::Json)),
                 std::string(".json"));
    MM_EXPECT_EQ(std::string(pipeline::fileExtension(pipeline::ExportFormat::Srt)),
                 std::string(".srt"));
    MM_EXPECT_EQ(std::string(pipeline::fileExtension(pipeline::ExportFormat::Text)),
                 std::string(".txt"));
    MM_EXPECT_EQ(std::string(pipeline::fileExtension(pipeline::ExportFormat::Html)),
                 std::string(".html"));
}

MM_TEST(export, 由配置解析格式列表) {
    Config cfg;
    cfg.exportMarkdown = true;
    cfg.exportJson = false;
    cfg.exportSrt = true;
    cfg.exportText = false;
    cfg.exportHtml = false;
    const auto formats = pipeline::Exporter::formatsFromConfig(cfg);
    MM_EXPECT_EQ(formats.size(), static_cast<size_t>(2));

    Config none;
    none.exportMarkdown = false;
    none.exportJson = false;
    none.exportSrt = false;
    none.exportText = false;
    none.exportHtml = false;
    MM_EXPECT_EQ(pipeline::Exporter::formatsFromConfig(none).size(), static_cast<size_t>(1));
}

MM_TEST(export, 主干名净化) {
    pipeline::PipelineResult r;
    r.title = "周会/纪要:2026";
    r.meetingDate = "2026-09-14";
    Config cfg;
    const std::string base = pipeline::Exporter::deriveBaseName(r, cfg);
    MM_EXPECT_FALSE(mm::str::contains(base, "/"));
    MM_EXPECT_FALSE(mm::str::contains(base, ":"));
    MM_EXPECT_TRUE(mm::str::endsWith(base, "2026-09-14"));

    Config withName;
    withName.baseFileName = "自定义名称";
    MM_EXPECT_EQ(pipeline::Exporter::deriveBaseName(r, withName), std::string("自定义名称"));
}

// ============================ 流水线 ============================

MM_TEST(pipeline, 完整流程产出纪要并导出文件) {
    const std::string wav = makeMeetingWav("meeting16k.wav");
    const std::string outDir = mmtest::outPath("pipeline/out16k");

    Config cfg = baseConfig(wav, outDir);
    pipeline::Pipeline pipe(cfg);
    // 通过脚本文件注入固定转写内容
    const std::string scriptPath = mmtest::outPath("pipeline/script.txt");
    {
        std::ofstream out(scriptPath, std::ios::binary);
        for (const std::string& line : meetingScript()) out << line << "\n";
    }
    cfg.replayScriptPath = scriptPath;
    pipe.setConfig(cfg);

    std::vector<pipeline::ProgressInfo> progress;
    Result<pipeline::PipelineResult> r = pipe.run(nullptr, [&](const pipeline::ProgressInfo& p) {
        progress.push_back(p);
    });
    MM_REQUIRE_TRUE(r.ok());

    const pipeline::PipelineResult& res = r.value();

    // --- 结构与对齐 ---
    MM_EXPECT_GT(res.segments.size(), static_cast<size_t>(4));
    MM_EXPECT_EQ(res.asr.segments.size(), res.segments.size());
    MM_EXPECT_EQ(res.diarization.speakerIds.size(), res.segments.size());
    MM_EXPECT_FALSE(res.speakerLabels.empty());

    // --- 转写内容 ---
    const std::string full = res.asr.fullText();
    MM_EXPECT_FALSE(full.empty());
    MM_EXPECT_CONTAINS(full, "排期");

    // --- 时间戳递增且落入音频范围 ---
    for (size_t i = 0; i < res.asr.segments.size(); ++i) {
        MM_EXPECT_GE(res.asr.segments[i].startMs, static_cast<int64_t>(0));
        MM_EXPECT_LE(res.asr.segments[i].endMs, res.quality.durationMs);
        MM_EXPECT_GE(res.asr.segments[i].endMs, res.asr.segments[i].startMs);
        if (i > 0) {
            MM_EXPECT_GE(res.asr.segments[i].startMs, res.asr.segments[i - 1].startMs);
        }
    }

    // --- 说话人回填 ---
    for (const auto& s : res.asr.segments) {
        MM_EXPECT_GE(s.speakerId, -1);
        MM_EXPECT_LT(s.speakerId, std::max(1, res.diarization.speakerCount));
    }

    // --- 纪要 ---
    MM_EXPECT_FALSE(res.minutes.title.empty());
    MM_EXPECT_FALSE(res.minutes.overview.empty());
    MM_EXPECT_GT(res.minutes.keywords.size(), static_cast<size_t>(0));
    MM_EXPECT_GT(res.minutes.topics.size(), static_cast<size_t>(0));
    MM_EXPECT_EQ(res.meetingDate, std::string("2026-09-14"));
    MM_EXPECT_GT(res.minutes.stats.sentenceCount, 0);
    MM_EXPECT_GT(res.minutes.stats.cjkCharacters, 0);
    MM_EXPECT_GT(res.minutes.actionItems.size(), static_cast<size_t>(0));

    // 待办责任人/截止时间（脚本第 5 段含「请张三在9月20日前」）
    bool foundOwner = false;
    for (const auto& a : res.minutes.actionItems) {
        if (a.owner == std::string("张三")) foundOwner = true;
    }
    MM_EXPECT_TRUE(foundOwner);

    // --- 报告 ---
    MM_EXPECT_GT(res.report.totalMs, 0);
    // 回放引擎几乎瞬时完成，识别阶段耗时可能不足 1 ms（计时精度为毫秒）；
    // 这里断言该阶段已记录，而非耗时大于 0。
    bool hasTranscribeTiming = false;
    bool hasDecodeTiming = false;
    for (const auto& t : res.report.timings) {
        if (t.stage == pipeline::Stage::Transcribe) hasTranscribeTiming = true;
        if (t.stage == pipeline::Stage::Decode) hasDecodeTiming = true;
    }
    MM_EXPECT_TRUE(hasTranscribeTiming);
    MM_EXPECT_TRUE(hasDecodeTiming);
    MM_EXPECT_EQ(res.report.timings.size(), static_cast<size_t>(8));
    MM_EXPECT_GE(res.report.elapsedOf(pipeline::Stage::Transcribe), static_cast<int64_t>(0));
    MM_EXPECT_EQ(res.report.asrBackend, std::string("replay"));
    MM_EXPECT_GT(res.report.audioMs, 0);

    // --- 导出文件 ---
    MM_EXPECT_GE(res.exportedFiles.size(), static_cast<size_t>(4));
    for (const std::string& f : res.exportedFiles) {
        MM_EXPECT_TRUE(fs::exists(f));
        MM_EXPECT_GT(fs::file_size(f), static_cast<std::uintmax_t>(50));
    }

    // --- 进度回调 ---
    MM_EXPECT_GT(progress.size(), static_cast<size_t>(5));
    double last = 0.0;
    for (const auto& p : progress) {
        MM_EXPECT_GE(p.fraction, last - 1e-9);
        MM_EXPECT_GE(p.fraction, 0.0);
        MM_EXPECT_LE(p.fraction, 1.0);
        MM_EXPECT_GT(p.stageCount, 0);
        last = p.fraction;
    }
    MM_EXPECT_NEAR(progress.back().fraction, 1.0, 1e-9);
    MM_EXPECT_TRUE(progress.back().stage == pipeline::Stage::Done);
}

MM_TEST(pipeline, 非16k采样率自动重采样) {
    const std::string wav = makeMeetingWav("meeting44k.wav", 44100);
    Config cfg = baseConfig(wav, mmtest::outPath("pipeline/out44k"));
    cfg.enableDiarization = false;

    const std::string scriptPath = mmtest::outPath("pipeline/script44k.txt");
    {
        std::ofstream out(scriptPath, std::ios::binary);
        for (const std::string& line : meetingScript()) out << line << "\n";
    }
    cfg.replayScriptPath = scriptPath;

    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().quality.sampleRate, 44100);
    MM_EXPECT_GT(r.value().segments.size(), static_cast<size_t>(4));
    MM_EXPECT_FALSE(r.value().minutes.overview.empty());
    // 重采样后仍应能识别出 6 段左右的语音
    MM_EXPECT_LE(r.value().segments.size(), static_cast<size_t>(10));
}

MM_TEST(pipeline, 取消令牌在各阶段边界生效) {
    const std::string wav = makeMeetingWav("cancel.wav");
    Config cfg = baseConfig(wav, mmtest::outPath("pipeline/out-cancel"));
    pipeline::Pipeline pipe(cfg);

    CancelToken cancel;
    cancel.cancel();  // 预先取消
    Result<pipeline::PipelineResult> r = pipe.run(&cancel);
    MM_EXPECT_FALSE(r.ok());
    MM_EXPECT_TRUE(r.code() == ErrorCode::Cancelled);
}

MM_TEST(pipeline, 输入文件不存在时报错且信息可读) {
    Config cfg;
    cfg.inputPath = mmtest::outPath("pipeline/不存在.wav");
    cfg.logLevel = LogLevel::Error;
    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_EXPECT_FALSE(r.ok());
    MM_EXPECT_TRUE(r.code() == ErrorCode::FileNotFound || r.code() == ErrorCode::IoError);
    MM_EXPECT_CONTAINS(r.message(), "不存在");
}

MM_TEST(pipeline, 未指定输入时参数错误) {
    Config cfg;
    cfg.logLevel = LogLevel::Error;
    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_EXPECT_FALSE(r.ok());
    MM_EXPECT_TRUE(r.code() == ErrorCode::InvalidArgument);
}

MM_TEST(pipeline, 空音频文件被拒绝) {
    const std::string path = mmtest::outPath("pipeline/empty.wav");
    {
        std::ofstream out(path, std::ios::binary);
        out << "";
    }
    Config cfg;
    cfg.inputPath = path;
    cfg.logLevel = LogLevel::Error;
    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_EXPECT_FALSE(r.ok());
    MM_EXPECT_FALSE(r.message().empty());
}

MM_TEST(pipeline, 关闭纪要生成时仍可导出转写) {
    const std::string wav = makeMeetingWav("nosum.wav");
    Config cfg = baseConfig(wav, mmtest::outPath("pipeline/out-nosum"));
    cfg.enableSummarization = false;
    cfg.exportSrt = true;

    const std::string scriptPath = mmtest::outPath("pipeline/script-nosum.txt");
    {
        std::ofstream out(scriptPath, std::ios::binary);
        for (const std::string& line : meetingScript()) out << line << "\n";
    }
    cfg.replayScriptPath = scriptPath;

    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().minutes.summaryEngine, std::string("disabled"));
    MM_EXPECT_EQ(r.value().minutes.keyPoints.size(), static_cast<size_t>(0));
    // 概览有兜底文案
    MM_EXPECT_FALSE(r.value().minutes.overview.empty());
    MM_EXPECT_GT(r.value().exportedFiles.size(), static_cast<size_t>(0));
}

MM_TEST(pipeline, 阶段数量与标签) {
    MM_EXPECT_EQ(pipeline::Pipeline::stageCount(), 8);
    for (int i = 0; i < pipeline::Pipeline::stageCount(); ++i) {
        const auto stage = static_cast<pipeline::Stage>(i + 1);
        MM_EXPECT_FALSE(std::string(pipeline::toString(stage)).empty());
        MM_EXPECT_FALSE(std::string(pipeline::stageLabelCn(stage)).empty());
    }
    MM_EXPECT_GE(pipeline::Pipeline::currentRssKb(), static_cast<int64_t>(0));
}

// ============================ 导出内容 ============================

MM_TEST(export, 各格式渲染内容正确) {
    const std::string wav = makeMeetingWav("render.wav");
    Config cfg = baseConfig(wav, mmtest::outPath("pipeline/out-render"));
    const std::string scriptPath = mmtest::outPath("pipeline/script-render.txt");
    {
        std::ofstream out(scriptPath, std::ios::binary);
        for (const std::string& line : meetingScript()) out << line << "\n";
    }
    cfg.replayScriptPath = scriptPath;

    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_REQUIRE_TRUE(r.ok());

    // Markdown
    Result<std::string> md = pipeline::Exporter::render(r.value(), pipeline::ExportFormat::Markdown);
    MM_REQUIRE_TRUE(md.ok());
    MM_EXPECT_CONTAINS(md.value(), "# ");
    MM_EXPECT_CONTAINS(md.value(), "## 八、全文转写");
    MM_EXPECT_CONTAINS(md.value(), "待办事项");
    MM_EXPECT_CONTAINS(md.value(), "关键词");

    // JSON：可被解析回对象并含关键字段
    Result<std::string> js = pipeline::Exporter::render(r.value(), pipeline::ExportFormat::Json);
    MM_REQUIRE_TRUE(js.ok());
    Result<Json> parsed = Json::parse(js.value());
    MM_REQUIRE_TRUE(parsed.ok());
    MM_EXPECT_EQ(parsed.value().getString("schemaVersion"), std::string("1.0"));
    MM_EXPECT_GT(parsed.value().get("asr").get("segments").size(), static_cast<size_t>(0));
    MM_EXPECT_GT(parsed.value().get("minutes").get("keywords").size(), static_cast<size_t>(0));

    // SRT：时间戳格式与序号
    Result<std::string> srt = pipeline::Exporter::render(r.value(), pipeline::ExportFormat::Srt);
    MM_REQUIRE_TRUE(srt.ok());
    MM_EXPECT_CONTAINS(srt.value(), "-->");
    MM_EXPECT_CONTAINS(srt.value(), "00:00:0");
    MM_EXPECT_TRUE(mm::str::startsWith(srt.value(), "1\n"));

    // 纯文本
    Result<std::string> txt = pipeline::Exporter::render(r.value(), pipeline::ExportFormat::Text);
    MM_REQUIRE_TRUE(txt.ok());
    MM_EXPECT_CONTAINS(txt.value(), "会议日期");
    MM_EXPECT_CONTAINS(txt.value(), "[");

    // HTML：自包含且转义
    Result<std::string> html = pipeline::Exporter::render(r.value(), pipeline::ExportFormat::Html);
    MM_REQUIRE_TRUE(html.ok());
    MM_EXPECT_CONTAINS(html.value(), "<!DOCTYPE html>");
    MM_EXPECT_CONTAINS(html.value(), "charset=\"utf-8\"");
    MM_EXPECT_CONTAINS(html.value(), "</html>");
    MM_EXPECT_FALSE(mm::str::contains(html.value(), "<script"));
}

MM_TEST(export, 导出目录自动创建且原子写无残留) {
    const std::string wav = makeMeetingWav("auto.wav");
    const std::string deepDir = mmtest::outPath("pipeline/deep/a/b/c");
    Config cfg = baseConfig(wav, deepDir);
    cfg.exportMarkdown = true;
    cfg.exportJson = false;
    cfg.exportSrt = false;
    cfg.exportText = false;

    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_REQUIRE_TRUE(r.ok());
    for (const std::string& f : r.value().exportedFiles) {
        MM_EXPECT_TRUE(fs::exists(f));
        MM_EXPECT_FALSE(fs::exists(f + ".tmp"));
    }
}

// ============================ 会话存储 ============================

MM_TEST(storage, 保存列出载入删除) {
    const std::string root = mmtest::outPath("sessions");
    fs::remove_all(root);
    storage::SessionStore store(root);
    MM_EXPECT_EQ(store.count(), static_cast<size_t>(0));

    pipeline::PipelineResult res;
    res.title = "周会纪要";
    res.meetingDate = "2026-09-14";
    res.inputPath = "meeting.wav";
    res.quality.durationMs = 120000;
    res.report.asrBackend = "replay";
    res.minutes.stats.speakerCount = 3;
    nlp::Keyword kw;
    kw.word = "排期";
    res.minutes.keywords.push_back(kw);
    nlp::ActionItem item;
    item.task = "确认排期";
    res.minutes.actionItems.push_back(item);

    Result<std::string> id = store.save(res, "session-a");
    MM_REQUIRE_TRUE(id.ok());
    MM_EXPECT_EQ(id.value(), std::string("session-a"));

    const auto list = store.list();
    MM_EXPECT_EQ(list.size(), static_cast<size_t>(1));
    MM_EXPECT_EQ(list.front().title, std::string("周会纪要"));
    MM_EXPECT_EQ(list.front().durationMs, static_cast<int64_t>(120000));
    MM_EXPECT_EQ(list.front().speakerCount, 3);
    MM_EXPECT_EQ(list.front().actionItemCount, 1);
    MM_EXPECT_EQ(list.front().keywords.size(), static_cast<size_t>(1));
    MM_EXPECT_FALSE(list.front().createdAt.empty());

    Result<Json> snapshot = store.load("session-a");
    MM_REQUIRE_TRUE(snapshot.ok());
    MM_EXPECT_EQ(snapshot.value().getString("sessionId"), std::string("session-a"));
    MM_EXPECT_EQ(snapshot.value().getString("title"), std::string("周会纪要"));

    // 同名保存不覆盖，追加序号
    Result<std::string> id2 = store.save(res, "session-a");
    MM_REQUIRE_TRUE(id2.ok());
    MM_EXPECT_NE(id2.value(), std::string("session-a"));
    MM_EXPECT_EQ(store.list().size(), static_cast<size_t>(2));

    MM_REQUIRE_TRUE(store.remove("session-a").ok());
    MM_EXPECT_EQ(store.list().size(), static_cast<size_t>(1));

    MM_REQUIRE_TRUE(store.clear().ok());
    MM_EXPECT_EQ(store.count(), static_cast<size_t>(0));
}

MM_TEST(storage, 非法id与不存在会话被拒绝) {
    const std::string root = mmtest::outPath("sessions2");
    fs::remove_all(root);
    storage::SessionStore store(root);
    MM_EXPECT_FALSE(store.load("../../../etc/passwd").ok());
    MM_EXPECT_FALSE(store.load("").ok());
    MM_EXPECT_FALSE(store.remove("not-exists").ok());
    MM_EXPECT_EQ(storage::SessionStore::makeId().size(), static_cast<size_t>(15));
}

MM_TEST(storage, 索引损坏时从快照重建) {
    const std::string root = mmtest::outPath("sessions3");
    fs::remove_all(root);
    storage::SessionStore store(root);

    pipeline::PipelineResult res;
    res.title = "重建测试";
    MM_REQUIRE_TRUE(store.save(res, "s1").ok());

    {
        std::ofstream out(fs::path(root) / "index.json", std::ios::binary);
        out << "{ 这不是合法 JSON";
    }
    const auto list = store.list();
    MM_EXPECT_EQ(list.size(), static_cast<size_t>(1));
    MM_EXPECT_EQ(list.front().id, std::string("s1"));
}

MM_TEST(pipeline, 转写单元分组与索引对齐) {
    const std::string wav = makeMeetingWav("grouped.wav");
    Config cfg = baseConfig(wav, mmtest::outPath("pipeline/out-grouped"));
    cfg.transcriptionUnitMs = 25000;
    cfg.unitMergeGapMs = 1500;
    cfg.enableDiarization = false;

    const std::string scriptPath = mmtest::outPath("pipeline/script-grouped.txt");
    {
        std::ofstream out(scriptPath, std::ios::binary);
        out << "今天我们先过一下排期。上线时间还要再确认一下。测试资源需要补充。\n";
    }
    cfg.replayScriptPath = scriptPath;

    pipeline::Pipeline pipe(cfg);
    Result<pipeline::PipelineResult> r = pipe.run();
    MM_REQUIRE_TRUE(r.ok());
    const pipeline::PipelineResult& res = r.value();

    // 6 段语音共约 11 秒、间隔 800 ms → 应合并为 1 个转写单元
    MM_EXPECT_EQ(res.report.transcriptionUnitCount, 1);
    MM_EXPECT_EQ(res.report.segmentCount, static_cast<int>(res.segments.size()));

    // 索引仍然一一对应
    MM_EXPECT_EQ(res.asr.segments.size(), res.segments.size());
    for (size_t i = 0; i < res.segments.size(); ++i) {
        MM_EXPECT_EQ(res.asr.segments[i].startMs, res.segments[i].startMs);
        MM_EXPECT_EQ(res.asr.segments[i].endMs, res.segments[i].endMs);
    }

    // 单条脚本行被切分后回填到各段。
    // 注意：切分点优先落在标点上，找不到标点时允许按字符切（即可能落在词中间），
    // 因此不能断言「某词组仍完整」——真正要守住的契约是：
    //   ① 内容不丢；② 每段都有内容；③ 时间戳单调。
    auto stripNoise = [](const std::string& t) {
        std::string out;
        for (char32_t c : mm::str::toUtf32(t)) {
            if (mm::str::isCjk(c) || mm::str::isAsciiAlnum(c)) {
                out += mm::str::toUtf8(std::u32string(1, c));
            }
        }
        return out;
    };
    std::string joined;
    int nonEmpty = 0;
    for (const auto& s : res.asr.segments) {
        if (!mm::str::trim(s.text).empty()) ++nonEmpty;
        joined += stripNoise(s.text);
    }
    const std::string expected =
        stripNoise("今天我们先过一下排期。上线时间还要再确认一下。测试资源需要补充。");
    MM_EXPECT_EQ(joined, expected);
    // 每个语音段都应分到内容（不允许出现「文本全堆在第一段」的情况）
    MM_EXPECT_EQ(nonEmpty, static_cast<int>(res.asr.segments.size()));
    // 时间顺序不因分组而错乱
    for (size_t i = 1; i < res.asr.segments.size(); ++i) {
        MM_EXPECT_GE(res.asr.segments[i].startMs, res.asr.segments[i - 1].startMs);
    }
}

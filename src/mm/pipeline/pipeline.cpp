#include "mm/pipeline/pipeline.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

#include "mm/audio/resampler.h"
#include "mm/audio/segmenter.h"
#include "mm/audio/vad.h"
#include "mm/audio/wav_io.h"
#include "mm/common/json.h"
#include "mm/common/path_utils.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "mm/diar/diarizer.h"
#include "mm/nlp/action_item_extractor.h"
#include "mm/nlp/minutes_builder.h"
#include "mm/nlp/punctuation_restorer.h"
#include "mm/nlp/sentence_splitter.h"
#include "mm/nlp/t2s.h"
#include "mm/nlp/text_normalizer.h"

#if defined(_WIN32)
// psapi.h 依赖 windows.h 的完整类型定义，因此这里临时关闭 WIN32_LEAN_AND_MEAN
#if defined(WIN32_LEAN_AND_MEAN)
#undef WIN32_LEAN_AND_MEAN
#define MM_RESTORE_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <psapi.h>
#if defined(MM_RESTORE_LEAN_AND_MEAN)
#undef MM_RESTORE_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

namespace mm::pipeline {
namespace fs = std::filesystem;

namespace {

/// 阶段索引（0 基）与总体进度区间映射
struct StageWindow {
    Stage stage;
    int index;
    double from;
    double to;
};

const std::vector<StageWindow>& stageWindows() {
    static const std::vector<StageWindow> kWindows = {
        {Stage::Decode, 0, 0.00, 0.08},
        {Stage::Resample, 1, 0.08, 0.12},
        {Stage::Vad, 2, 0.12, 0.18},
        {Stage::Transcribe, 3, 0.18, 0.70},
        {Stage::Diarize, 4, 0.70, 0.78},
        {Stage::Normalize, 5, 0.78, 0.84},
        {Stage::Minutes, 6, 0.84, 0.96},
        {Stage::Export, 7, 0.96, 1.00},
    };
    return kWindows;
}

double windowProgress(Stage stage, double localFraction) {
    for (const StageWindow& w : stageWindows()) {
        if (w.stage == stage) {
            const double f = std::max(0.0, std::min(1.0, localFraction));
            return w.from + (w.to - w.from) * f;
        }
    }
    return 0.0;
}

/// 时间累加器（RAII 风格）
class StageTimer {
public:
    StageTimer(PipelineReport* report, Stage stage) : report_(report), stage_(stage),
                                                      start_(timeutil::steadyNowMs()) {}
    ~StageTimer() {
        if (report_) {
            StageTiming t;
            t.stage = stage_;
            t.elapsedMs = timeutil::steadyNowMs() - start_;
            report_->timings.push_back(t);
        }
    }

private:
    PipelineReport* report_;
    Stage stage_;
    int64_t start_;
};

/// 生成说话人显示名。
std::vector<std::string> buildSpeakerLabels(int count) {
    std::vector<std::string> labels;
    if (count <= 1) {
        labels.emplace_back("发言人");
        return labels;
    }
    for (int i = 0; i < count; ++i) {
        labels.push_back("说话人" + std::to_string(i + 1));
    }
    return labels;
}

}  // namespace

int64_t Pipeline::currentRssKb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<int64_t>(pmc.WorkingSetSize / 1024);
    }
    return 0;
#else
    return 0;
#endif
}

int Pipeline::stageCount() { return static_cast<int>(stageWindows().size()); }

Pipeline::Pipeline(Config config) : config_(std::move(config)) {}

Pipeline::~Pipeline() = default;

const nlp::Tokenizer& Pipeline::ensureTokenizer() {
    if (tokenizerReady_) return tokenizer_;

    Result<void> lr = tokenizer_.loadDefaultLexicon(config_.lexiconPath);
    if (!lr.ok()) {
        MM_LOG_WARN("pipeline") << "词典加载失败，回退共享词典: " << lr.message();
        return nlp::sharedTokenizer();
    }
    tokenizerReady_ = true;

    std::string swPath = config_.stopwordsPath;
    if (swPath.empty()) {
        const std::string dir = Config::locateDataDir();
        if (!dir.empty()) swPath = pathutil::join(dir, "stopwords_zh.txt");
    }
    if (!swPath.empty() && pathutil::exists(swPath)) {
        std::ifstream in = pathutil::openInput(swPath);
        std::vector<std::string> words;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = str::trim(line);
            if (t.empty() || t[0] == '#') continue;
            words.push_back(t);
        }
        tokenizer_.setStopwords(std::move(words));
        MM_LOG_DEBUG("pipeline") << "词典就绪: " << tokenizer_.lexiconSize() << " 词 + 停用词 "
                                 << words.size() << " 条";
    }
    return tokenizer_;
}

void Pipeline::notify(const ProgressCallback& cb, Stage stage, int index, double fraction,
                      const std::string& message) const {
    if (!cb) return;
    ProgressInfo info;
    info.stage = stage;
    info.stageIndex = index;
    info.stageCount = stageCount();
    info.fraction = std::max(0.0, std::min(1.0, fraction));
    info.message = message;
    cb(info);
}

Result<PipelineResult> Pipeline::run(CancelToken* cancel, const ProgressCallback& onProgress) {
    const int64_t pipelineStart = timeutil::steadyNowMs();
    PipelineResult result;
    result.inputPath = config_.inputPath;
    result.meetingDate = timeutil::formatYmd(config_.effectiveMeetingDate());

    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };

    // ---------------- 1. 解码 ----------------
    if (config_.inputPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "未指定输入音频文件");
    }
    if (!pathutil::exists(config_.inputPath)) {
        return fail(ErrorCode::FileNotFound, "输入文件不存在: " + config_.inputPath);
    }

    notify(onProgress, Stage::Decode, 0, windowProgress(Stage::Decode, 0.0), "正在读取音频文件…");
    AudioBuffer audio;
    {
        StageTimer timer(&result.report, Stage::Decode);
        AudioQualityReport quality;
        Result<AudioBuffer> decoded = wav::read(config_.inputPath, &quality);
        if (!decoded.ok()) {
            result.report.totalMs = timeutil::steadyNowMs() - pipelineStart;
            return fail(decoded.code(), decoded.message());
        }
        audio = std::move(decoded.value());
        result.quality = std::move(quality);
    }
    if (audio.empty()) {
        return fail(ErrorCode::DecodeFailed, "音频不包含任何采样数据");
    }
    MM_LOG_INFO("pipeline") << "解码完成: " << result.quality.toSummary();
    notify(onProgress, Stage::Decode, 0, windowProgress(Stage::Decode, 1.0),
           "音频 " + timeutil::formatDuration(result.quality.durationMs));

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // ---------------- 2. 重采样 ----------------
    const int targetRate = 16000;
    {
        StageTimer timer(&result.report, Stage::Resample);
        notify(onProgress, Stage::Resample, 1, windowProgress(Stage::Resample, 0.2),
               "重采样至 16 kHz…");
        if (audio.sampleRate != targetRate) {
            audio::ResampleStats stats;
            Result<AudioBuffer> rs =
                audio::Resampler::resample(audio, targetRate, 1, &stats);
            if (!rs.ok()) return fail(rs.code(), "重采样失败: " + rs.message());
            audio = std::move(rs.value());
        }
        notify(onProgress, Stage::Resample, 1, windowProgress(Stage::Resample, 1.0),
               "采样率 " + std::to_string(audio.sampleRate) + " Hz");
    }

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // ---------------- 3. VAD 与分段 ----------------
    {
        StageTimer timer(&result.report, Stage::Vad);
        notify(onProgress, Stage::Vad, 2, windowProgress(Stage::Vad, 0.2), "正在检测语音活动…");

        audio::VadConfig vadCfg;
        vadCfg.thresholdDeltaDb = config_.vadThresholdDeltaDb;
        vadCfg.noiseFloorPercentile = config_.vadNoiseFloorPercentile;
        vadCfg.minSpeechMs = config_.minSpeechMs;
        vadCfg.minSilenceMs = config_.minSilenceMs;

        audio::VoiceActivityDetector vad(vadCfg);
        Result<audio::VadResult> vadResult = vad.process(audio);
        if (!vadResult.ok()) return fail(vadResult.code(), vadResult.message());

        audio::SegmenterConfig segCfg;
        segCfg.padMs = config_.vadPadMs;
        segCfg.maxSegmentMs = config_.maxSegmentMs;
        segCfg.minSegmentMs = std::max(50, config_.minSpeechMs / 2);

        audio::SpeechSegmenter segmenter(segCfg, audio.sampleRate);
        result.segments = segmenter.refine(vadResult.value().segments, audio);
        result.minutes.stats.speechDurationMs = 0;
        for (const SpeechSegment& s : result.segments) {
            result.minutes.stats.speechDurationMs += s.durationMs();
        }
        result.minutes.stats.speechRatio = vadResult.value().speechRatio;
        result.minutes.stats.totalDurationMs = result.quality.durationMs;

        if (result.segments.empty()) {
            return fail(ErrorCode::DecodeFailed, "未检测到可转写的语音段");
        }
        result.report.segmentCount = static_cast<int>(result.segments.size());
        notify(onProgress, Stage::Vad, 2, windowProgress(Stage::Vad, 1.0),
               "检测到 " + std::to_string(result.segments.size()) + " 个语音段");
    }

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // ---------------- 4. 转写 ----------------
    {
        StageTimer timer(&result.report, Stage::Transcribe);
        notify(onProgress, Stage::Transcribe, 3, windowProgress(Stage::Transcribe, 0.0),
               "正在加载识别模型…");

        Result<asr::EngineSelection> selection = asr::AsrFactory::create(config_);
        if (!selection.ok()) return fail(selection.code(), selection.message());

        result.report.asrBackend = selection.value().engine->id();
        result.report.asrBackendReason = selection.value().reason;
        result.asr.backend = selection.value().engine->id();
        if (selection.value().degraded) {
            MM_LOG_WARN("pipeline") << "识别后端已降级: " << selection.value().reason;
        }

        result.asr.audioMs = result.quality.durationMs;

        // 把语音段聚合为「转写单元」：whisper 按固定窗口编码，逐段调用会浪费大量固定开销
        audio::GroupConfig groupCfg;
        groupCfg.maxUnitMs = config_.transcriptionUnitMs;
        groupCfg.mergeGapMs = config_.unitMergeGapMs;
        std::vector<audio::TranscriptionUnit> units;
        if (config_.transcriptionUnitMs > 0) {
            units = audio::groupSegments(result.segments, groupCfg);
        } else {
            // 关闭分组：每个语音段各自作为一个转写单元
            units.reserve(result.segments.size());
            for (size_t i = 0; i < result.segments.size(); ++i) {
                audio::TranscriptionUnit u;
                u.startMs = result.segments[i].startMs;
                u.endMs = result.segments[i].endMs;
                u.members.push_back(i);
                units.push_back(std::move(u));
            }
        }
        result.report.transcriptionUnitCount = static_cast<int>(units.size());

        audio::SegmenterConfig sliceCfg;
        sliceCfg.padMs = config_.vadPadMs;
        sliceCfg.maxSegmentMs = config_.maxSegmentMs;
        sliceCfg.minSegmentMs = 0;
        audio::SpeechSegmenter slicer(sliceCfg, audio.sampleRate);

        // 分词器必须在转写前就绪：单元文本回填时，切点需要吸附到**词边界**，
        // 否则会把「包装」切成「包|装」（真实录音实测出现过）。
        const nlp::Tokenizer& tk = ensureTokenizer();

        // 每个语音段一份文本（单元内按成员时长比例回填，保持索引对齐）
        std::vector<std::string> segTexts(result.segments.size());
        std::vector<float> segConfidence(result.segments.size(), 0.0f);
        std::vector<float> segNoSpeech(result.segments.size(), 1.0f);

        const int totalUnits = static_cast<int>(units.size());
        const int64_t asrStart = timeutil::steadyNowMs();

        for (int u = 0; u < totalUnits; ++u) {
            if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

            const audio::TranscriptionUnit& unit = units[static_cast<size_t>(u)];
            SpeechSegment unitSpan;
            unitSpan.startMs = unit.startMs;
            unitSpan.endMs = unit.endMs;

            // 切片含 padMs 补白，必须用实际起点回填时间戳，否则整体偏移一个 pad
            int64_t chunkStartMs = unitSpan.startMs;
            const AudioBuffer chunk = slicer.slice(audio, unitSpan, 0, &chunkStartMs);

            Result<asr::AsrSegment> part =
                selection.value().engine->transcribe(chunk, chunkStartMs, cancel);
            if (!part.ok()) {
                if (part.code() == ErrorCode::Cancelled) {
                    return fail(ErrorCode::Cancelled, "用户已取消");
                }
                MM_LOG_WARN("pipeline") << "第 " << (u + 1) << "/" << totalUnits
                                        << " 个转写单元识别失败: " << part.message();
                // 保留占位（置信度 0、非语音概率 1），维持索引对齐
            } else {
                std::vector<int64_t> weights;
                weights.reserve(unit.members.size());
                for (size_t m : unit.members) {
                    weights.push_back(result.segments[m].durationMs());
                }
                const std::vector<std::string> pieces =
                    nlp::SentenceSplitter::splitByWeights(part.value().text, weights, &tk);
                for (size_t k = 0; k < unit.members.size(); ++k) {
                    const size_t m = unit.members[k];
                    if (k < pieces.size()) segTexts[m] = pieces[k];
                    segConfidence[m] = part.value().confidence;
                    segNoSpeech[m] = part.value().noSpeechProb;
                }
            }

            const double local = static_cast<double>(u + 1) / static_cast<double>(totalUnits);
            notify(onProgress, Stage::Transcribe, 3, windowProgress(Stage::Transcribe, local),
                   "正在识别第 " + std::to_string(u + 1) + "/" + std::to_string(totalUnits) +
                       " 个单元…");
        }

        result.asr.segments.reserve(result.segments.size());
        for (size_t i = 0; i < result.segments.size(); ++i) {
            asr::AsrSegment s;
            s.startMs = result.segments[i].startMs;
            s.endMs = result.segments[i].endMs;
            s.text = segTexts[i];
            s.confidence = segConfidence[i];
            s.noSpeechProb = segNoSpeech[i];
            result.asr.segments.push_back(std::move(s));
        }

        result.asr.elapsedMs = timeutil::steadyNowMs() - asrStart;
        result.report.asrRealTimeFactor = result.asr.realTimeFactor();
    }

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // ---------------- 5. 说话人分离 ----------------
    {
        StageTimer timer(&result.report, Stage::Diarize);
        notify(onProgress, Stage::Diarize, 4, windowProgress(Stage::Diarize, 0.3),
               "正在分离说话人…");

        diar::DiarizationConfig diarCfg;
        diarCfg.enabled = config_.enableDiarization;
        diarCfg.targetSpeakers = config_.effectiveSpeakerCount();
        diarCfg.mergeThreshold = config_.diarMergeThreshold;

        diar::SpeakerDiarizer diarizer(diarCfg);
        Result<diar::DiarizationResult> dr = diarizer.process(audio, result.segments);
        if (!dr.ok()) {
            MM_LOG_WARN("pipeline") << "说话人分离失败，降级为单一说话人: " << dr.message();
            result.diarization.speakerIds.assign(result.segments.size(), 0);
            result.diarization.speakerCount = 1;
            result.diarization.degraded = true;
            result.diarization.note = dr.message();
        } else {
            result.diarization = std::move(dr.value());
        }

        result.speakerLabels = buildSpeakerLabels(result.diarization.speakerCount);
        result.minutes.stats.speakerCount = result.diarization.speakerCount;
        result.minutes.stats.speakerTalkMs.assign(
            static_cast<size_t>(std::max(1, result.diarization.speakerCount)), 0);
        result.minutes.stats.speakerCharCount.assign(
            static_cast<size_t>(std::max(1, result.diarization.speakerCount)), 0);

        // 回填说话人标签
        for (size_t i = 0; i < result.asr.segments.size(); ++i) {
            if (i < result.diarization.speakerIds.size()) {
                result.asr.segments[i].speakerId = result.diarization.speakerIds[i];
            }
            if (i < result.segments.size()) {
                result.segments[i].speakerId = result.asr.segments[i].speakerId;
            }
        }
        notify(onProgress, Stage::Diarize, 4, windowProgress(Stage::Diarize, 1.0),
               "识别出 " + std::to_string(result.diarization.speakerCount) + " 位说话人");
    }

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // ---------------- 6. 文本规范化（ITN + 标点恢复）----------------
    {
        StageTimer timer(&result.report, Stage::Normalize);
        notify(onProgress, Stage::Normalize, 5, windowProgress(Stage::Normalize, 0.3),
               "正在规范化转写文本…");

        std::vector<std::string> texts;
        texts.reserve(result.asr.segments.size());
        for (const asr::AsrSegment& s : result.asr.segments) texts.push_back(s.text);

        // 繁转简必须在 ITN 之前：ITN 的数词规则基于简体字表
        if (config_.enableT2s) {
            const nlp::TraditionalConverter& t2s = nlp::sharedTraditionalConverter();
            int converted = 0;
            for (std::string& t : texts) {
                if (t2s.needsConversion(t)) {
                    t = t2s.convert(t);
                    ++converted;
                }
            }
            if (converted > 0) {
                MM_LOG_INFO("pipeline") << "繁转简: " << converted << "/" << texts.size()
                                        << " 段含繁体字，已统一为简体";
            }
        }
        if (config_.enableItn) {
            nlp::TextNormalizer itn;
            for (std::string& t : texts) t = itn.normalize(t);
        }
        if (config_.enablePunctuation) {
            std::vector<int64_t> gaps;
            gaps.reserve(result.asr.segments.size());
            for (size_t i = 0; i < result.asr.segments.size(); ++i) {
                if (i + 1 < result.asr.segments.size()) {
                    gaps.push_back(std::max<int64_t>(
                        0, result.asr.segments[i + 1].startMs - result.asr.segments[i].endMs));
                }
            }
            nlp::PunctuationRestorer restorer;
            restorer.restoreSequence(texts, gaps);
        }
        for (size_t i = 0; i < result.asr.segments.size(); ++i) {
            result.asr.segments[i].text = texts[i];
        }
        notify(onProgress, Stage::Normalize, 5, windowProgress(Stage::Normalize, 1.0),
               "文本规范化完成");
    }

    // ---------------- 7. 句子切分、统计与纪要 ----------------
    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");
    {
        StageTimer timer(&result.report, Stage::Minutes);
        notify(onProgress, Stage::Minutes, 6, windowProgress(Stage::Minutes, 0.1),
               "正在生成会议纪要…");

        std::vector<nlp::SentenceSplitter::TimedText> timed;
        timed.reserve(result.asr.segments.size());
        for (const asr::AsrSegment& s : result.asr.segments) {
            nlp::SentenceSplitter::TimedText t;
            t.text = s.text;
            t.startMs = s.startMs;
            t.endMs = s.endMs;
            t.speakerId = s.speakerId;
            timed.push_back(std::move(t));
        }
        nlp::SentenceSplitOptions splitOpts;
        splitOpts.maxSentenceChars = 60;
        result.sentences = nlp::SentenceSplitter{splitOpts}.splitTimed(timed);

        // 统计
        nlp::Statistics& st = result.minutes.stats;
        st.totalDurationMs = result.quality.durationMs;
        st.segmentCount = static_cast<int>(result.asr.segments.size());
        st.sentenceCount = static_cast<int>(result.sentences.size());
        st.lowConfidenceSegments = result.asr.lowConfidenceCount();
        int64_t charTotal = 0;
        int cjkTotal = 0;
        for (const asr::AsrSegment& s : result.asr.segments) {
            const std::u32string u = str::toUtf32(s.text);
            int cjk = 0;
            for (char32_t c : u) {
                if (str::isCjk(c)) ++cjk;
            }
            charTotal += static_cast<int64_t>(u.size());
            cjkTotal += cjk;
            const int spk = std::max(0, s.speakerId);
            if (static_cast<size_t>(spk) < st.speakerTalkMs.size()) {
                st.speakerTalkMs[static_cast<size_t>(spk)] += s.durationMs();
                st.speakerCharCount[static_cast<size_t>(spk)] += cjk;
            }
        }
        st.totalCharacters = static_cast<int>(charTotal);
        st.cjkCharacters = cjkTotal;

        // 词典与停用词已在转写阶段由 ensureTokenizer() 就绪，这里直接复用
        const nlp::Tokenizer& tk = ensureTokenizer();

        nlp::MinutesOptions mo;
        mo.title = config_.title;
        mo.meetingDate = result.meetingDate;
        mo.enableSummarization = config_.enableSummarization;
        mo.summaryRatio = config_.summaryRatio;
        mo.summaryMaxSentences = config_.summaryMaxSentences;
        mo.maxKeywords = config_.maxKeywords;
        mo.maxActionItems = config_.maxActionItems;

        nlp::MinutesBuilder builder(tk, mo);
        Result<nlp::Minutes> minutes = builder.build(result.sentences, result.speakerLabels, st,
                                                     /*summarizer=*/nullptr);
        if (!minutes.ok()) return fail(minutes.code(), "纪要生成失败: " + minutes.message());
        result.minutes = std::move(minutes.value());
        result.title = result.minutes.title;

        notify(onProgress, Stage::Minutes, 6, windowProgress(Stage::Minutes, 0.9),
               "纪要生成完成");
    }

    // ---------------- 报告定稿（必须在导出之前）----------------
    // 导出产物本身要包含这份报告，因此总耗时/内存等指标必须在渲染前算好；
    // 导出耗时单独记录在 exportMs（见 PipelineReport 注释）。
    result.report.totalMs = timeutil::steadyNowMs() - pipelineStart;
    result.report.audioMs = result.quality.durationMs;
    result.report.peakRssKb = currentRssKb();
    if (result.report.totalMs > 0) {
        result.report.overallRealTimeFactor =
            static_cast<double>(result.quality.durationMs) / static_cast<double>(result.report.totalMs);
    }
    if (result.asr.language.empty()) result.asr.language = config_.language;

    // ---------------- 8. 导出 ----------------
    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");
    {
        const int64_t exportStart = timeutil::steadyNowMs();
        {
            StageTimer timer(&result.report, Stage::Export);
            notify(onProgress, Stage::Export, 7, windowProgress(Stage::Export, 0.2), "正在导出…");
            Result<std::vector<std::string>> files = Exporter::exportAll(result, config_);
            if (!files.ok()) {
                MM_LOG_ERROR("pipeline") << "导出失败: " << files.message();
                result.minutes.risks.push_back("导出失败：" + files.message());
            } else {
                result.exportedFiles = std::move(files.value());
            }
            notify(onProgress, Stage::Export, 7, windowProgress(Stage::Export, 1.0),
                   "已导出 " + std::to_string(result.exportedFiles.size()) + " 个文件");
        }
        result.report.exportMs = timeutil::steadyNowMs() - exportStart;
    }

    notify(onProgress, Stage::Done, stageCount(), 1.0, "全部完成");
    MM_LOG_INFO("pipeline") << "流水线完成: 总耗时 "
                            << timeutil::formatDuration(result.report.totalMs) << ", 整体 RTF "
                            << result.report.overallRealTimeFactor;
    return result;
}

Result<PipelineResult> Pipeline::importReviewed(const std::string& reviewedJsonPath,
                                               CancelToken* cancel,
                                               const ProgressCallback& onProgress) {
    const int64_t start = timeutil::steadyNowMs();
    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };

    Result<Json> parsed = Json::parseFile(reviewedJsonPath);
    if (!parsed.ok()) return fail(parsed.code(), "读取审核结果失败: " + parsed.message());
    const Json& j = parsed.value();

    PipelineResult result;
    result.inputPath = j.getString("inputPath", "");
    result.title = j.getString("title", "");
    result.meetingDate = j.getString("meetingDate", "");
    result.quality.durationMs = j.get("quality").getInt64("durationMs", 0);

    // 从审核结果回填 asr.segments（text/speakerId 已人工修改，可能已合并）
    const Json& asr = j.get("asr");
    const Json& segs = asr.get("segments");
    result.asr.language = asr.getString("language", "zh");
    result.asr.backend = "reviewed";
    result.asr.segments.reserve(segs.size());
    for (size_t i = 0; i < segs.size(); ++i) {
        const Json& s = segs.at(i);
        asr::AsrSegment seg;
        seg.startMs = s.getInt64("startMs", 0);
        seg.endMs = s.getInt64("endMs", 0);
        seg.text = s.getString("text", "");
        seg.speakerId = s.getInt("speakerId", 0);
        result.asr.segments.push_back(std::move(seg));
    }

    // speakerLabels
    const Json& labels = j.get("speakerLabels");
    for (size_t i = 0; i < labels.size(); ++i) {
        result.speakerLabels.push_back(labels.at(i).asString());
    }

    if (result.asr.segments.empty()) {
        return fail(ErrorCode::InvalidArgument, "审核结果不含任何转写段落");
    }
    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // 句子切分
    std::vector<nlp::SentenceSplitter::TimedText> timed;
    timed.reserve(result.asr.segments.size());
    for (const asr::AsrSegment& s : result.asr.segments) {
        nlp::SentenceSplitter::TimedText t;
        t.text = s.text;
        t.startMs = s.startMs;
        t.endMs = s.endMs;
        t.speakerId = s.speakerId;
        timed.push_back(std::move(t));
    }
    nlp::SentenceSplitOptions splitOpts;
    splitOpts.maxSentenceChars = 60;
    result.sentences = nlp::SentenceSplitter{splitOpts}.splitTimed(timed);

    // 统计
    nlp::Statistics& st = result.minutes.stats;
    st.totalDurationMs = result.quality.durationMs;
    st.segmentCount = static_cast<int>(result.asr.segments.size());
    st.sentenceCount = static_cast<int>(result.sentences.size());
    st.lowConfidenceSegments = 0;
    st.speakerCount = static_cast<int>(result.speakerLabels.size());
    st.speakerTalkMs.assign(std::max<size_t>(1, result.speakerLabels.size()), 0);
    st.speakerCharCount.assign(std::max<size_t>(1, result.speakerLabels.size()), 0);
    int64_t charTotal = 0;
    int cjkTotal = 0;
    for (const asr::AsrSegment& s : result.asr.segments) {
        const std::u32string u = str::toUtf32(s.text);
        int cjk = 0;
        for (char32_t c : u) {
            if (str::isCjk(c)) ++cjk;
        }
        charTotal += static_cast<int64_t>(u.size());
        cjkTotal += cjk;
        const int spk = std::max(0, s.speakerId);
        if (static_cast<size_t>(spk) < st.speakerTalkMs.size()) {
            st.speakerTalkMs[static_cast<size_t>(spk)] += s.durationMs();
            st.speakerCharCount[static_cast<size_t>(spk)] += cjk;
        }
    }
    st.totalCharacters = static_cast<int>(charTotal);
    st.cjkCharacters = cjkTotal;

    // 纪要（复用与完整流水线相同的组件与配置）
    const nlp::Tokenizer& tk = ensureTokenizer();
    nlp::MinutesOptions mo;
    mo.title = config_.title;
    mo.meetingDate = result.meetingDate;
    mo.enableSummarization = config_.enableSummarization;
    mo.summaryRatio = config_.summaryRatio;
    mo.summaryMaxSentences = config_.summaryMaxSentences;
    mo.maxKeywords = config_.maxKeywords;
    mo.maxActionItems = config_.maxActionItems;

    nlp::MinutesBuilder builder(tk, mo);
    Result<nlp::Minutes> minutes =
        builder.build(result.sentences, result.speakerLabels, st, /*summarizer=*/nullptr);
    if (!minutes.ok()) return fail(minutes.code(), "纪要生成失败: " + minutes.message());
    result.minutes = std::move(minutes.value());
    result.title = result.minutes.title;

    // 报告定稿
    result.report.totalMs = timeutil::steadyNowMs() - start;
    result.report.audioMs = result.quality.durationMs;

    if (cancelled()) return fail(ErrorCode::Cancelled, "用户已取消");

    // 导出
    {
        const int64_t exportStart = timeutil::steadyNowMs();
        Result<std::vector<std::string>> files = Exporter::exportAll(result, config_);
        if (!files.ok()) {
            result.minutes.risks.push_back("导出失败：" + files.message());
        } else {
            result.exportedFiles = std::move(files.value());
        }
        result.report.exportMs = timeutil::steadyNowMs() - exportStart;
    }

    notify(onProgress, Stage::Done, stageCount(), 1.0, "审核结果已导入并重新生成");
    return result;
}

}  // namespace mm::pipeline

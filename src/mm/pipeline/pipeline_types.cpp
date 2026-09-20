#include "mm/pipeline/pipeline_types.h"

#include <cmath>

#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm::pipeline {

const char* toString(Stage stage) noexcept {
    switch (stage) {
        case Stage::Idle: return "idle";
        case Stage::Decode: return "decode";
        case Stage::Resample: return "resample";
        case Stage::Vad: return "vad";
        case Stage::Transcribe: return "transcribe";
        case Stage::Diarize: return "diarize";
        case Stage::Normalize: return "normalize";
        case Stage::Minutes: return "minutes";
        case Stage::Export: return "export";
        case Stage::Done: return "done";
        case Stage::Failed: return "failed";
    }
    return "unknown";
}

const char* stageLabelCn(Stage stage) noexcept {
    switch (stage) {
        case Stage::Idle: return "空闲";
        case Stage::Decode: return "解码音频";
        case Stage::Resample: return "重采样";
        case Stage::Vad: return "语音检测";
        case Stage::Transcribe: return "语音转写";
        case Stage::Diarize: return "说话人分离";
        case Stage::Normalize: return "文本规范化";
        case Stage::Minutes: return "纪要生成";
        case Stage::Export: return "导出结果";
        case Stage::Done: return "已完成";
        case Stage::Failed: return "失败";
    }
    return "未知阶段";
}

int64_t PipelineReport::elapsedOf(Stage stage) const {
    for (const StageTiming& t : timings) {
        if (t.stage == stage) return t.elapsedMs;
    }
    return 0;
}

Json PipelineReport::toJson() const {
    Json j = Json::object();
    j.set("totalMs", totalMs);
    j.set("totalText", timeutil::formatDuration(totalMs));
    j.set("exportMs", exportMs);
    j.set("modelLoadMs", modelLoadMs);
    j.set("segmentCount", segmentCount);
    j.set("transcriptionUnitCount", transcriptionUnitCount);
    j.set("audioMs", audioMs);
    j.set("asrBackend", asrBackend);
    j.set("asrBackendReason", asrBackendReason);
    j.set("asrRealTimeFactor", asrRealTimeFactor);
    j.set("overallRealTimeFactor", overallRealTimeFactor);
    j.set("peakRssKb", peakRssKb);

    Json arr = Json::array();
    for (const StageTiming& t : timings) {
        Json e = Json::object();
        e.set("stage", std::string(toString(t.stage)));
        e.set("label", std::string(stageLabelCn(t.stage)));
        e.set("elapsedMs", t.elapsedMs);
        arr.push(e);
    }
    j.set("stages", arr);
    return j;
}

std::string PipelineResult::speakerLabel(int id) const {
    if (id < 0) return "未识别";
    if (static_cast<size_t>(id) < speakerLabels.size()) return speakerLabels[static_cast<size_t>(id)];
    return "说话人" + std::to_string(id + 1);
}

std::string PipelineResult::plainTranscript() const {
    std::string out;
    for (const asr::AsrSegment& s : asr.segments) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        out += "[" + timeutil::formatDuration(s.startMs) + "] " + speakerLabel(s.speakerId) +
               ": " + text + "\n";
    }
    return out;
}

Json PipelineResult::toJson() const {
    Json j = Json::object();
    j.set("schemaVersion", "1.0");
    j.set("inputPath", inputPath);
    j.set("title", title);
    j.set("meetingDate", meetingDate);
    j.set("quality", quality.toJson());
    j.set("asr", asr.toJson());
    j.set("diarization", diarization.toJson());

    Json segArr = Json::array();
    for (const SpeechSegment& s : segments) {
        Json sj = Json::object();
        sj.set("startMs", s.startMs);
        sj.set("endMs", s.endMs);
        sj.set("durationMs", s.durationMs());
        sj.set("energyDb", static_cast<double>(s.energyDb));
        sj.set("speakerId", s.speakerId);
        segArr.push(sj);
    }
    j.set("segments", segArr);

    j.set("speakerLabels", Json::of(speakerLabels));
    j.set("minutes", minutes.toJson());
    j.set("report", report.toJson());
    j.set("exportedFiles", Json::of(exportedFiles));
    return j;
}

}  // namespace mm::pipeline

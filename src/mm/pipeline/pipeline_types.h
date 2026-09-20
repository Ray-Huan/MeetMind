// MeetMind — 流水线公共类型
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mm/asr/asr_types.h"
#include "mm/audio/audio_types.h"
#include "mm/common/json.h"
#include "mm/diar/diarizer.h"
#include "mm/nlp/minutes_builder.h"
#include "mm/nlp/sentence_splitter.h"

namespace mm::pipeline {

/// 流水线阶段。
enum class Stage {
    Idle = 0,
    Decode,      ///< 读取并解析 WAV
    Resample,    ///< 重采样至 16 kHz
    Vad,         ///< 语音活动检测与分段
    Transcribe,  ///< 逐段识别
    Diarize,     ///< 说话人分离
    Normalize,   ///< ITN 与标点恢复
    Minutes,     ///< 纪要生成
    Export,      ///< 导出
    Done,
    Failed,
};

const char* toString(Stage stage) noexcept;
/// 阶段的中文可读名（用于界面与报告）。
const char* stageLabelCn(Stage stage) noexcept;

/// 阶段计时。
struct StageTiming {
    Stage stage = Stage::Idle;
    int64_t elapsedMs = 0;
};

/// 流水线执行报告（性能与诊断）。
struct PipelineReport {
    std::vector<StageTiming> timings;
    /// 处理总耗时（解码 → 纪要；**不含导出**）。
    /// 之所以排除导出：导出产物本身要写入该报告，若把导出耗时也计入，
    /// 则任何已写出的文件都无法反映真实总耗时（鸡生蛋问题）。
    /// 导出耗时单独记录在 exportMs 中，两者相加即端到端总耗时。
    int64_t totalMs = 0;
    int64_t exportMs = 0;
    int64_t modelLoadMs = 0;
    int segmentCount = 0;
    int transcriptionUnitCount = 0;   ///< 实际调用识别引擎的次数
    int64_t audioMs = 0;
    std::string asrBackend;
    std::string asrBackendReason;
    double asrRealTimeFactor = 0.0;
    double overallRealTimeFactor = 0.0;
    int64_t peakRssKb = 0;

    int64_t elapsedOf(Stage stage) const;
    Json toJson() const;
};

/// 进度信息。
struct ProgressInfo {
    Stage stage = Stage::Idle;
    int stageIndex = 0;
    int stageCount = 0;
    double fraction = 0.0;  ///< 总体进度 [0,1]
    std::string message;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

/// 流水线完整产物。
struct PipelineResult {
    std::string inputPath;
    std::string title;
    std::string meetingDate;

    AudioQualityReport quality;
    std::vector<SpeechSegment> segments;   ///< 实际送入识别引擎的转写单元
    asr::AsrResult asr;
    diar::DiarizationResult diarization;
    std::vector<nlp::Sentence> sentences;
    std::vector<std::string> speakerLabels;
    nlp::Minutes minutes;
    PipelineReport report;
    std::vector<std::string> exportedFiles;

    /// 说话人标签（越界返回空串）。
    std::string speakerLabel(int id) const;
    /// 带说话人与时间戳的纯文本转写。
    std::string plainTranscript() const;
    /// 序列化为 JSON。
    Json toJson() const;
};

}  // namespace mm::pipeline

// MeetMind — 转写单元切分
// 职责：在 VAD 原始段基础上做「补白 / 过长切分 / 切片」，产出可直接送入 ASR 的音频块。
#pragma once

#include <vector>

#include "mm/audio/audio_types.h"

namespace mm::audio {

struct SegmenterConfig {
    int padMs = 200;         ///< 段首尾各补白（防止切掉字头字尾）
    int maxSegmentMs = 30000;///< 单段上限，超出则在最低能量处切分
    int minSegmentMs = 200;  ///< 单段下限，低于此值丢弃
};

/// 转写单元分组配置。
///
/// 动机：whisper 一类基于固定 30 秒窗口的模型，无论输入多短都会完整编码一个窗口，
/// 因此「每个 VAD 段单独调用一次」会造成大量固定开销（实测 1.2 秒的段与 30 秒的段
/// 耗时几乎相同）。把相邻且间隔不大的语音段合并成接近窗口长度的转写单元，
/// 可以在不损失时间戳精度的前提下将吞吐提升数倍。
struct GroupConfig {
    int maxUnitMs = 25000;   ///< 单个转写单元时长上限（略小于 30 秒窗口）
    int mergeGapMs = 1500;   ///< 允许合并的最大段间隔；间隔更大的段保持独立
};

/// 一个转写单元：覆盖若干连续语音段。
struct TranscriptionUnit {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::vector<size_t> members;   ///< 对应的语音段索引（升序）

    int64_t durationMs() const { return endMs > startMs ? endMs - startMs : 0; }
    size_t size() const { return members.size(); }
};

/// 把语音段按「相邻 + 间隔小 + 不超过长度上限」聚合为转写单元。
std::vector<TranscriptionUnit> groupSegments(const std::vector<SpeechSegment>& segments,
                                             const GroupConfig& config);

class SpeechSegmenter {
public:
    SpeechSegmenter(SegmenterConfig config, int sampleRate);

    /// 后处理：边界裁剪 → 过长切分 → 重新计算能量。
    std::vector<SpeechSegment> refine(const std::vector<SpeechSegment>& raw,
                                      const AudioBuffer& audio) const;

    /// 截取段落音频（含 padMs 补白，边界自动裁剪）。
    /// @param actualStartMsOut 非空时写入切片在原始音频中的实际起始毫秒。
    ///        由于补白与边界裁剪，实际起点通常早于 segment.startMs，
    ///        调用方回填时间戳时必须使用该值，否则会产生 padMs 级偏移。
    AudioBuffer slice(const AudioBuffer& audio, const SpeechSegment& segment,
                      int extraPadMs = 0, int64_t* actualStartMsOut = nullptr) const;

    /// 单段切分的最大采样点数。
    size_t maxSegmentSamples() const;

    const SegmenterConfig& config() const { return config_; }
    int sampleRate() const { return sampleRate_; }

private:
    /// 在 [startSample, endSample) 内寻找能量最低的一帧中心点。
    size_t findLowestEnergySplit(const AudioBuffer& audio, size_t startSample,
                                 size_t endSample) const;

    SegmenterConfig config_;
    int sampleRate_ = 16000;
};

}  // namespace mm::audio

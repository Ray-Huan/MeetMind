// MeetMind — 说话人分离
// 段级声纹嵌入（MFCC 统计量）+ 凝聚式层次聚类 + 时间轴平滑。
// 优点：无监督、无需声纹注册、纯 CPU；局限：MFCC 统计量属弱嵌入，短段与强噪声下会退化。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mm/audio/audio_types.h"
#include "mm/audio/mel.h"
#include "mm/common/json.h"
#include "mm/common/result.h"

namespace mm::diar {

/// 发言轮次（时间轴上连续属于同一说话人的区间）。
struct SpeakerTurn {
    int speakerId = 0;
    int64_t startMs = 0;
    int64_t endMs = 0;
    int segmentCount = 0;

    int64_t durationMs() const { return endMs > startMs ? endMs - startMs : 0; }
};

struct DiarizationConfig {
    int targetSpeakers = 0;      ///< 0 = 自动估计
    int maxSpeakers = 8;
    double mergeThreshold = 0.35;///< 余弦距离合并阈值
    bool enabled = true;

    dsp::MfccConfig mfcc;
    bool useDelta = true;        ///< 是否加入一阶差分统计量
    int minEmbeddingSegments = 2;///< 有效段少于该值时退化为单说话人
    int minEmbeddingMs = 300;    ///< 短于此的段不参与建模（标签由邻段继承）
    /// 标签平滑强度：
    ///   0–2 → 不平滑；3–4 → 仅消除「孤立单段标签」（推荐，默认）；
    ///   >=5 → 中值滤波（会抹掉占比小的说话人，仅在前者不足时使用）
    int smoothingWindow = 3;
    double jumpRatio = 1.5;      ///< 自动估计人数时的距离跳变比
};

struct DiarizationResult {
    std::vector<int> speakerIds;  ///< 与输入段一一对应
    int speakerCount = 0;
    std::vector<SpeakerTurn> turns;
    double stoppingDistance = 0.0;
    bool degraded = false;        ///< 是否因数据不足退化为单说话人
    std::string note;             ///< 退化原因

    Json toJson() const;
};

class SpeakerDiarizer {
public:
    explicit SpeakerDiarizer(DiarizationConfig config = DiarizationConfig{});
    ~SpeakerDiarizer() = default;

    /// 对已切分的语音段做说话人分离。
    /// @param audio     16 kHz 单声道整段音频
    /// @param segments  VAD 输出的语音段
    Result<DiarizationResult> process(const AudioBuffer& audio,
                                      const std::vector<SpeechSegment>& segments) const;

    /// 计算单个音频段的声纹嵌入向量（L2 归一化）。
    static std::vector<float> computeEmbedding(const AudioBuffer& segment,
                                               const DiarizationConfig& config);

    const DiarizationConfig& config() const { return config_; }

private:
    DiarizationConfig config_;
};

}  // namespace mm::diar

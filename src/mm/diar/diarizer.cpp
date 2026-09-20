#include "mm/diar/diarizer.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/diar/cluster.h"

namespace mm::diar {
namespace {

void l2Normalize(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    if (sum <= 1e-18) return;
    const double inv = 1.0 / std::sqrt(sum);
    for (float& x : v) x = static_cast<float>(x * inv);
}

/// 消除「孤立单段标签」：A A B A A → A A A A A。
///
/// 为什么不用中值滤波：中值滤波会把「只出现在少数段里的合法说话人」整体抹掉。
/// 真实 9 分钟多人座谈实测：跳变分析正确估计出 2 人，但窗口 3 的中值滤波
/// 把占比小的那一类全部翻转，最终结果退化成 1 人 —— 少数说话人被当成噪声删除。
/// 本函数只处理「长度为 1 的孤立抖动」，不触碰连续多段出现的类别。
std::vector<int> removeIsolatedLabels(const std::vector<int>& labels) {
    if (labels.size() < 3) return labels;
    std::vector<int> out = labels;
    const int n = static_cast<int>(labels.size());
    for (int i = 1; i + 1 < n; ++i) {
        // 仅当左右邻居同类、且自己与它们不同（即长度为 1 的插入段）才消除
        if (labels[static_cast<size_t>(i - 1)] == labels[static_cast<size_t>(i + 1)] &&
            labels[static_cast<size_t>(i)] != labels[static_cast<size_t>(i - 1)]) {
            out[static_cast<size_t>(i)] = labels[static_cast<size_t>(i - 1)];
        }
    }
    return out;
}

/// 中值滤波（窗口为奇数）。保留作为可选的强平滑手段，默认不启用。
std::vector<int> medianFilter(const std::vector<int>& labels, int window) {
    if (window < 3 || labels.size() < 3) return labels;
    if (window % 2 == 0) ++window;
    const int half = window / 2;
    const int n = static_cast<int>(labels.size());
    std::vector<int> out(labels.size());
    std::vector<int> buf;
    for (int i = 0; i < n; ++i) {
        buf.clear();
        for (int k = -half; k <= half; ++k) {
            const int idx = i + k;
            if (idx < 0 || idx >= n) continue;
            buf.push_back(labels[static_cast<size_t>(idx)]);
        }
        std::sort(buf.begin(), buf.end());
        // 取众数（出现最多者，平票取中间值）
        int best = buf[buf.size() / 2];
        int bestCount = 0;
        for (size_t a = 0; a < buf.size();) {
            size_t b = a;
            while (b < buf.size() && buf[b] == buf[a]) ++b;
            const int cnt = static_cast<int>(b - a);
            if (cnt > bestCount) {
                bestCount = cnt;
                best = buf[a];
            }
            a = b;
        }
        out[static_cast<size_t>(i)] = best;
    }
    return out;
}

}  // namespace

Json DiarizationResult::toJson() const {
    Json j = Json::object();
    j.set("speakerCount", speakerCount);
    j.set("degraded", degraded);
    j.set("note", note);
    j.set("stoppingDistance", stoppingDistance);

    Json idArr = Json::array();
    for (int id : speakerIds) idArr.push(Json(static_cast<int64_t>(id)));
    j.set("speakerIds", idArr);

    Json turnArr = Json::array();
    for (const SpeakerTurn& t : turns) {
        Json tj = Json::object();
        tj.set("speakerId", t.speakerId);
        tj.set("startMs", t.startMs);
        tj.set("endMs", t.endMs);
        tj.set("durationMs", t.durationMs());
        tj.set("segmentCount", t.segmentCount);
        turnArr.push(tj);
    }
    j.set("turns", turnArr);
    return j;
}

SpeakerDiarizer::SpeakerDiarizer(DiarizationConfig config) : config_(config) {}

std::vector<float> SpeakerDiarizer::computeEmbedding(const AudioBuffer& segment,
                                                     const DiarizationConfig& config) {
    dsp::MfccExtractor extractor(config.mfcc);
    const std::vector<std::vector<float>> frames = extractor.compute(segment);
    std::vector<float> out;
    if (frames.empty()) return out;

    const size_t dim = frames.front().size();
    // 均值
    std::vector<float> mean(dim, 0.0f);
    for (const auto& f : frames) {
        for (size_t d = 0; d < dim; ++d) mean[d] += f[d];
    }
    for (float& v : mean) v = static_cast<float>(v / static_cast<double>(frames.size()));
    out.insert(out.end(), mean.begin(), mean.end());

    // 标准差
    std::vector<float> var(dim, 0.0f);
    for (const auto& f : frames) {
        for (size_t d = 0; d < dim; ++d) {
            const float diff = f[d] - mean[d];
            var[d] += diff * diff;
        }
    }
    for (float& v : var) v = std::sqrt(v / static_cast<double>(frames.size()));
    out.insert(out.end(), var.begin(), var.end());

    // 一阶差分统计
    if (config.useDelta && frames.size() >= 3) {
        const std::vector<std::vector<float>> d1 = dsp::MfccExtractor::delta(frames, 2);
        std::vector<float> dmean(dim, 0.0f);
        for (const auto& f : d1) {
            for (size_t d = 0; d < dim; ++d) dmean[d] += f[d];
        }
        for (float& v : dmean) v = static_cast<float>(v / static_cast<double>(d1.size()));
        out.insert(out.end(), dmean.begin(), dmean.end());
    }

    l2Normalize(out);
    return out;
}

Result<DiarizationResult> SpeakerDiarizer::process(const AudioBuffer& audio,
                                                   const std::vector<SpeechSegment>& segments) const {
    DiarizationResult result;
    result.speakerIds.assign(segments.size(), 0);

    if (segments.empty()) {
        result.speakerCount = 0;
        result.degraded = true;
        result.note = "无语音段，未执行说话人分离";
        return result;
    }
    if (!config_.enabled) {
        result.speakerCount = 1;
        result.degraded = true;
        result.note = "说话人分离已在配置中关闭";
        return result;
    }

    // ---- 计算段级嵌入 ----
    std::vector<std::vector<float>> features;
    std::vector<size_t> featureToSegment;
    features.reserve(segments.size());

    for (size_t i = 0; i < segments.size(); ++i) {
        const SpeechSegment& seg = segments[i];
        if (seg.durationMs() < config_.minEmbeddingMs) continue;
        const size_t startSample = msToSamples(seg.startMs, audio.sampleRate);
        size_t endSample = msToSamples(seg.endMs, audio.sampleRate);
        endSample = std::min(endSample, audio.samples.size());
        if (endSample <= startSample) continue;

        AudioBuffer slice;
        slice.sampleRate = audio.sampleRate;
        slice.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(startSample),
                             audio.samples.begin() + static_cast<std::ptrdiff_t>(endSample));

        std::vector<float> emb = computeEmbedding(slice, config_);
        if (emb.empty()) continue;
        features.push_back(std::move(emb));
        featureToSegment.push_back(i);
    }

    if (static_cast<int>(features.size()) < std::max(2, config_.minEmbeddingSegments)) {
        result.speakerCount = 1;
        result.degraded = true;
        result.note = "有效语音段不足（" + std::to_string(features.size()) +
                      " 段），退化为单一说话人";
        MM_LOG_WARN("diar") << result.note;
        SpeakerTurn turn;
        turn.speakerId = 0;
        turn.startMs = segments.front().startMs;
        turn.endMs = segments.back().endMs;
        turn.segmentCount = static_cast<int>(segments.size());
        result.turns.push_back(turn);
        return result;
    }

    // ---- 聚类 ----
    ClusterResult clustered;
    const int requested = std::max(0, std::min(config_.maxSpeakers, config_.targetSpeakers));

    if (requested > 0) {
        // 人工指定人数：直接聚到该簇数
        ClusterConfig cc;
        cc.targetClusters = requested;
        cc.mergeThreshold = 1e9;  // 以目标簇数为准，不受绝对阈值干扰
        clustered = AgglomerativeClusterer::cluster(features, cc);
    } else {
        // 自动估计人数：必须先把「完全合并」跑一遍，才能拿到完整的合并距离序列。
        //
        // 曾经的做法是「按绝对阈值聚类，若簇数为 1 再看合并距离跳变」——
        // 这个逻辑永远不会触发：当最小簇间距离就已经大于阈值时，一次合并都不会发生，
        // mergeDistances 是空的，跳变分析无从谈起（真实多人对话实测被判成单一说话人）。
        ClusterConfig full;
        full.targetClusters = 1;    // 合并到只剩 1 簇 → 得到 n-1 个合并距离
        full.mergeThreshold = 1e9;  // 阈值不参与
        const ClusterResult dendrogram = AgglomerativeClusterer::cluster(features, full);

        const int suggested = AgglomerativeClusterer::estimateClusters(
            dendrogram.mergeDistances, config_.jumpRatio, config_.maxSpeakers);

        if (suggested > 1) {
            ClusterConfig cc;
            cc.targetClusters = suggested;
            cc.mergeThreshold = 1e9;
            clustered = AgglomerativeClusterer::cluster(features, cc);
            MM_LOG_INFO("diar") << "树状图跳变分析估计人数 = " << suggested
                                << "（合并距离 " << dendrogram.mergeDistances.size() << " 步）";
        } else {
            // 跳变分析未发现结构性差异，再退回绝对阈值判断（可能只合并很近的段）
            ClusterConfig cc;
            cc.targetClusters = 0;
            cc.mergeThreshold = config_.mergeThreshold;
            clustered = AgglomerativeClusterer::cluster(features, cc);
            MM_LOG_INFO("diar") << "未发现明显说话人结构，按阈值 "
                                << config_.mergeThreshold << " 判定为 "
                                << clustered.clusterCount << " 人";
        }
    }

    result.speakerCount = clustered.clusterCount;
    result.stoppingDistance = clustered.stoppingDistance;

    // ---- 标签回填：短段继承前一段 ----
    std::vector<int> labels(segments.size(), -1);
    for (size_t k = 0; k < featureToSegment.size(); ++k) {
        labels[featureToSegment[k]] = clustered.labels[k];
    }
    int last = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] < 0) {
            labels[i] = last;
        } else {
            last = labels[i];
        }
    }

    // ---- 平滑 ----
    // 默认只做「孤立段消除」；只有显式把 smoothingWindow 设得较大时才用中值滤波。
    if (config_.smoothingWindow >= 5) {
        labels = medianFilter(labels, config_.smoothingWindow);
    } else if (config_.smoothingWindow >= 3) {
        labels = removeIsolatedLabels(labels);
    }

    // 平滑后可能消失的说话人：重编号保持连续
    {
        std::unordered_map<int, int> remap;
        for (int& l : labels) {
            auto it = remap.find(l);
            if (it == remap.end()) {
                const int id = static_cast<int>(remap.size());
                remap.emplace(l, id);
                l = id;
            } else {
                l = it->second;
            }
        }
        result.speakerCount = static_cast<int>(remap.size());
    }

    result.speakerIds = labels;

    // ---- 轮次合并 ----
    for (size_t i = 0; i < segments.size(); ++i) {
        const int id = labels[i];
        if (!result.turns.empty() && result.turns.back().speakerId == id) {
            SpeakerTurn& back = result.turns.back();
            back.endMs = segments[i].endMs;
            ++back.segmentCount;
        } else {
            SpeakerTurn t;
            t.speakerId = id;
            t.startMs = segments[i].startMs;
            t.endMs = segments[i].endMs;
            t.segmentCount = 1;
            result.turns.push_back(t);
        }
    }

    result.degraded = (result.speakerCount <= 1);
    if (result.degraded) {
        result.note = "声纹区分度不足，判为单一说话人";
    }

    MM_LOG_INFO("diar") << "说话人分离完成: " << result.speakerCount << " 人, 轮次 "
                        << result.turns.size() << ", 停止距离 "
                        << static_cast<int>(result.stoppingDistance * 100) / 100.0;
    return result;
}

}  // namespace mm::diar

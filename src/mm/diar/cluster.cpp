#include "mm/diar/cluster.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mm/common/logger.h"

namespace mm::diar {
namespace {

constexpr double kInf = std::numeric_limits<double>::max();

void l2Normalize(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    if (sum <= 1e-18) return;
    const double inv = 1.0 / std::sqrt(sum);
    for (float& x : v) x = static_cast<float>(x * inv);
}

}  // namespace

double AgglomerativeClusterer::cosineDistance(const std::vector<float>& a,
                                              const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i];
        const double y = b[i];
        dot += x * y;
        na += x * x;
        nb += y * y;
    }
    if (na <= 1e-18 || nb <= 1e-18) return 1.0;
    double cos = dot / (std::sqrt(na) * std::sqrt(nb));
    cos = std::max(-1.0, std::min(1.0, cos));
    return 1.0 - cos;
}

int AgglomerativeClusterer::estimateClusters(const std::vector<double>& mergeDistances,
                                             double jumpRatio, int maxClusters) {
    // 语义：mergeDistances 是「完全合并」过程中的距离序列（长度为 n-1，n 为样本数，
    // 按合并顺序非递减）。第 i 个元素（0 基）是第 i+1 次合并发生的距离；
    // 完成第 i+1 次合并后剩余簇数 = n - (i+1)。
    //
    // 判定：在序列中寻找**相对增幅最大**的一处 —— 它之前都是「同一个人内部的合并」
    // （距离小且平缓），它之后就进入「不同说话人之间的合并」（距离陡增）。
    // 选在跳变发生前停止，即保留 n - i 个簇（宁可多分，也不要把两个人并成一个人）。
    if (mergeDistances.empty()) return 1;
    if (maxClusters <= 1) return 1;

    const int n = static_cast<int>(mergeDistances.size()) + 1;
    const double ratio = std::max(1.05, jumpRatio);

    // 只在「结果不超过 maxClusters」的区间里搜索，避免把上限钳制当成答案
    const int minIdx = std::max(1, n - maxClusters);
    int bestIdx = -1;
    double bestRatio = 0.0;
    for (int i = minIdx; i < static_cast<int>(mergeDistances.size()); ++i) {
        const double prev = std::max(1e-6, mergeDistances[static_cast<size_t>(i - 1)]);
        const double jump = mergeDistances[static_cast<size_t>(i)] / prev;
        if (jump > bestRatio) {
            bestRatio = jump;
            bestIdx = i;
        }
    }
    if (bestIdx < 0 || bestRatio < ratio) return 1;
    return std::max(1, std::min(maxClusters, n - bestIdx));
}

ClusterResult AgglomerativeClusterer::cluster(const std::vector<std::vector<float>>& features,
                                              const ClusterConfig& config) {
    ClusterResult result;
    const size_t n = features.size();
    if (n == 0) return result;

    // 统一维度
    size_t dim = 0;
    for (const auto& f : features) dim = std::max(dim, f.size());
    if (dim == 0) {
        result.labels.assign(n, 0);
        result.clusterCount = 1;
        return result;
    }

    std::vector<std::vector<float>> data(n, std::vector<float>(dim, 0.0f));
    for (size_t i = 0; i < n; ++i) {
        std::copy(features[i].begin(), features[i].end(), data[i].begin());
        l2Normalize(data[i]);
    }

    if (n == 1) {
        result.labels.assign(1, 0);
        result.clusterCount = 1;
        return result;
    }

    // 距离矩阵（对称，只维护上三角）
    std::vector<std::vector<double>> dist(n, std::vector<double>(n, kInf));
    for (size_t i = 0; i < n; ++i) {
        dist[i][i] = kInf;
        for (size_t j = i + 1; j < n; ++j) {
            const double d = cosineDistance(data[i], data[j]);
            dist[i][j] = d;
            dist[j][i] = d;
        }
    }

    std::vector<int> label(n);
    for (size_t i = 0; i < n; ++i) label[i] = static_cast<int>(i);
    std::vector<bool> active(n, true);
    int activeCount = static_cast<int>(n);

    const int target = std::max(0, config.targetClusters);
    const double threshold = config.mergeThreshold;

    std::vector<double> merges;
    int steps = 0;

    while (activeCount > 1 && steps < config.maxSteps) {
        if (target > 0 && activeCount <= target) break;

        // 找最小距离对
        double best = kInf;
        size_t bi = 0;
        size_t bj = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!active[i]) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (!active[j]) continue;
                if (dist[i][j] < best) {
                    best = dist[i][j];
                    bi = i;
                    bj = j;
                }
            }
        }
        if (best >= kInf) break;
        if (target <= 0 && best > threshold) {
            result.stoppingDistance = best;
            break;
        }

        merges.push_back(best);

        // 合并 bj -> bi（质心 = 归一化均值，近似 average linkage）
        const size_t si = static_cast<size_t>(std::count_if(
            label.begin(), label.end(), [&](int l) { return l == label[bi]; }));
        const size_t sj = static_cast<size_t>(std::count_if(
            label.begin(), label.end(), [&](int l) { return l == label[bj]; }));
        const double w1 = static_cast<double>(si) / static_cast<double>(si + sj);
        const double w2 = static_cast<double>(sj) / static_cast<double>(si + sj);
        for (size_t d = 0; d < dim; ++d) {
            data[bi][d] = static_cast<float>(w1 * data[bi][d] + w2 * data[bj][d]);
        }
        l2Normalize(data[bi]);

        const int keep = label[bi];
        const int drop = label[bj];
        for (size_t k = 0; k < n; ++k) {
            if (label[k] == drop) label[k] = keep;
        }
        active[bj] = false;
        --activeCount;

        // 重算 bi 与其他活跃簇的距离
        for (size_t k = 0; k < n; ++k) {
            if (!active[k] || k == bi) continue;
            const double d = cosineDistance(data[bi], data[k]);
            dist[bi][k] = d;
            dist[k][bi] = d;
        }
        ++steps;
        (void)config.maxSteps;
    }

    // 若因 target 退出，记录剩余最小距离
    if (target > 0) {
        double best = kInf;
        for (size_t i = 0; i < n; ++i) {
            if (!active[i]) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (!active[j]) continue;
                best = std::min(best, dist[i][j]);
            }
        }
        result.stoppingDistance = (best < kInf) ? best : 0.0;
    }

    // 重编号为 0..k-1（按首次出现顺序，保证确定性）
    std::unordered_map<int, int> remap;
    result.labels.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const int raw = label[i];
        auto it = remap.find(raw);
        if (it == remap.end()) {
            const int id = static_cast<int>(remap.size());
            remap.emplace(raw, id);
            result.labels[i] = id;
        } else {
            result.labels[i] = it->second;
        }
    }
    result.clusterCount = static_cast<int>(remap.size());
    result.mergeDistances = std::move(merges);

    MM_LOG_DEBUG("diar.cluster") << "聚类完成: 样本 " << n << " -> 簇 " << result.clusterCount
                                 << " 停止距离 " << result.stoppingDistance;
    return result;
}

std::unordered_map<int, int> ClusterResult::clusterSizes() const {
    std::unordered_map<int, int> sizes;
    for (int l : labels) ++sizes[l];
    return sizes;
}

}  // namespace mm::diar

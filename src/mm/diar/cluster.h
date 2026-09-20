// MeetMind — 凝聚式层次聚类（余弦距离，平均/质心连接）
// 用途：说话人分离。段数通常 ≤ 数百，采用 O(n^3) 朴素实现以保证可读性与确定性。
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm::diar {

struct ClusterConfig {
    /// 目标簇数；> 0 时优先按目标数停止（除非距离已超过阈值）
    int targetClusters = 0;
    /// 合并阈值：最小簇间距离超过该值即停止
    double mergeThreshold = 0.35;
    /// 最大迭代保护
    int maxSteps = 10000;
};

struct ClusterResult {
    std::vector<int> labels;        ///< 与输入一一对应，取值 0..k-1
    int clusterCount = 0;
    /// 停止时的最小簇间距离（越大表示簇越可分）
    double stoppingDistance = 0.0;
    /// 合并过程中记录的「距离跳变」比（用于自适应估计人数）
    std::vector<double> mergeDistances;

    /// 每个簇的样本数。
    std::unordered_map<int, int> clusterSizes() const;
};

class AgglomerativeClusterer {
public:
    /// 输入特征向量（要求同维度）。若向量未归一化，内部会做 L2 归一化。
    static ClusterResult cluster(const std::vector<std::vector<float>>& features,
                                 const ClusterConfig& config = ClusterConfig{});

    /// 余弦距离 1 - cos(θ)，输入需非零向量。
    static double cosineDistance(const std::vector<float>& a, const std::vector<float>& b);

    /// 由「合并距离序列」自适应估计簇数：当相邻合并距离出现显著跳变（> jumpRatio 倍）时停止。
    static int estimateClusters(const std::vector<double>& mergeDistances, double jumpRatio = 1.5,
                                int maxClusters = 8);
};

}  // namespace mm::diar

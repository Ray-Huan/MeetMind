// MeetMind — TextRank（图排序）
// 参考文献：Mihalcea & Tarau, EMNLP 2004。用于关键词与句子重要性排序。
// 迭代式：WS(Vi) = (1-d) + d · Σ_j (w_ji / Σ_k w_jk) · WS(Vj)
#pragma once

#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mm::nlp {

struct TextRankOptions {
    double damping = 0.85;
    double epsilon = 1e-4;
    int maxIterations = 100;
};

class TextRank {
public:
    /// 通用加权无向图排序。edges 元素为 (from, to, weight)，权重需 >= 0。
    static std::vector<double> rank(size_t nodeCount,
                                    const std::vector<std::tuple<int, int, double>>& edges,
                                    const TextRankOptions& options = TextRankOptions{});

    /// 词级排序：输入为每个句子（或窗口）的词元列表。
    /// @param cooccurWindow 共现窗口大小（词距 ≤ window 视为共现）
    static std::unordered_map<std::string, double> rankWords(
        const std::vector<std::vector<std::string>>& sentences, int cooccurWindow = 5,
        const TextRankOptions& options = TextRankOptions{});

    /// 句级排序：以句子相似度作为边权。
    static std::vector<double> rankSentences(
        const std::vector<std::vector<std::string>>& sentences,
        const TextRankOptions& options = TextRankOptions{});

    /// TextRank 原文的句子相似度：|S∩T| / (log|S| + log|T|)
    static double sentenceSimilarity(const std::vector<std::string>& a,
                                     const std::vector<std::string>& b);

    /// 把得分线性归一化到 [0,1]（全相等时返回全 0.5）。
    static void normalize(std::vector<double>& scores);
};

}  // namespace mm::nlp

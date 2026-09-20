#include "mm/nlp/textrank.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace mm::nlp {

std::vector<double> TextRank::rank(size_t nodeCount,
                                   const std::vector<std::tuple<int, int, double>>& edges,
                                   const TextRankOptions& options) {
    std::vector<double> score(nodeCount, 1.0);
    if (nodeCount == 0) return score;

    // 邻接表（无向图：双向加入）与出度和
    struct Link {
        int to;
        double w;
    };
    std::vector<std::vector<Link>> adj(nodeCount);
    for (const auto& [from, to, w] : edges) {
        if (w <= 0.0) continue;
        if (from < 0 || to < 0 || static_cast<size_t>(from) >= nodeCount ||
            static_cast<size_t>(to) >= nodeCount) {
            continue;
        }
        if (from == to) continue;
        adj[static_cast<size_t>(from)].push_back({to, w});
        adj[static_cast<size_t>(to)].push_back({from, w});
    }

    std::vector<double> outSum(nodeCount, 0.0);
    for (size_t i = 0; i < nodeCount; ++i) {
        double s = 0.0;
        for (const Link& l : adj[i]) s += l.w;
        outSum[i] = s;
    }

    const double d = std::max(0.0, std::min(1.0, options.damping));
    std::vector<double> next(nodeCount, 0.0);
    for (int iter = 0; iter < options.maxIterations; ++iter) {
        double delta = 0.0;
        for (size_t i = 0; i < nodeCount; ++i) {
            double acc = 0.0;
            for (const Link& l : adj[i]) {
                const size_t j = static_cast<size_t>(l.to);
                if (outSum[j] > 0.0) acc += (l.w / outSum[j]) * score[j];
            }
            next[i] = (1.0 - d) + d * acc;
            delta += std::fabs(next[i] - score[i]);
        }
        score.swap(next);
        if (delta < options.epsilon) break;
    }
    return score;
}

double TextRank::sentenceSimilarity(const std::vector<std::string>& a,
                                    const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return 0.0;
    std::unordered_set<std::string> setB(b.begin(), b.end());
    int overlap = 0;
    std::unordered_set<std::string> counted;
    for (const std::string& w : a) {
        if (setB.count(w) > 0 && counted.insert(w).second) ++overlap;
    }
    if (overlap == 0) return 0.0;
    const double denom = std::log(static_cast<double>(a.size()) + 1.0) +
                         std::log(static_cast<double>(b.size()) + 1.0);
    if (denom <= 0.0) return 0.0;
    return static_cast<double>(overlap) / denom;
}

std::unordered_map<std::string, double> TextRank::rankWords(
    const std::vector<std::vector<std::string>>& sentences, int cooccurWindow,
    const TextRankOptions& options) {
    std::unordered_map<std::string, int> index;
    std::vector<std::string> words;
    std::vector<std::pair<int, int>> pairs;

    const int window = std::max(1, cooccurWindow);
    for (const auto& sent : sentences) {
        std::vector<int> ids;
        ids.reserve(sent.size());
        for (const std::string& w : sent) {
            if (w.empty()) continue;
            auto it = index.find(w);
            if (it == index.end()) {
                const int id = static_cast<int>(words.size());
                index.emplace(w, id);
                words.push_back(w);
                ids.push_back(id);
            } else {
                ids.push_back(it->second);
            }
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            for (size_t j = i + 1; j < ids.size() && j <= i + static_cast<size_t>(window); ++j) {
                if (ids[i] == ids[j]) continue;
                pairs.emplace_back(ids[i], ids[j]);
            }
        }
    }

    // 共现次数 → 边权
    std::unordered_map<long long, double> weight;
    weight.reserve(pairs.size() * 2);
    for (const auto& [a, b] : pairs) {
        const long long key = (static_cast<long long>(std::min(a, b)) << 32) |
                              static_cast<unsigned>(std::max(a, b));
        weight[key] += 1.0;
    }
    std::vector<std::tuple<int, int, double>> edges;
    edges.reserve(weight.size());
    for (const auto& [key, w] : weight) {
        const int a = static_cast<int>(key >> 32);
        const int b = static_cast<int>(key & 0xFFFFFFFFLL);
        edges.emplace_back(a, b, w);
    }

    const std::vector<double> scores = rank(words.size(), edges, options);
    std::unordered_map<std::string, double> out;
    out.reserve(words.size());
    for (size_t i = 0; i < words.size(); ++i) out.emplace(words[i], scores[i]);
    return out;
}

std::vector<double> TextRank::rankSentences(const std::vector<std::vector<std::string>>& sentences,
                                            const TextRankOptions& options) {
    const size_t n = sentences.size();
    std::vector<std::tuple<int, int, double>> edges;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const double sim = sentenceSimilarity(sentences[i], sentences[j]);
            if (sim > 0.0) {
                edges.emplace_back(static_cast<int>(i), static_cast<int>(j), sim);
            }
        }
    }
    return rank(n, edges, options);
}

void TextRank::normalize(std::vector<double>& scores) {
    if (scores.empty()) return;
    const auto [loIt, hiIt] = std::minmax_element(scores.begin(), scores.end());
    // 必须先取值：下面会原地修改 scores，迭代器指向的元素会被覆盖
    const double lo = *loIt;
    const double hi = *hiIt;
    const double range = hi - lo;
    if (range <= 1e-12) {
        std::fill(scores.begin(), scores.end(), 0.5);
        return;
    }
    for (double& s : scores) s = (s - lo) / range;
}

}  // namespace mm::nlp

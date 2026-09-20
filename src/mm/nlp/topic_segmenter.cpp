#include "mm/nlp/topic_segmenter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/nlp/textrank.h"

namespace mm::nlp {
namespace {

using Vec = std::unordered_map<std::string, double>;

Vec buildVector(const std::vector<std::vector<std::string>>& tokens, size_t from, size_t to) {
    Vec v;
    for (size_t i = from; i < to && i < tokens.size(); ++i) {
        for (const std::string& w : tokens[i]) {
            v[w] += 1.0;
        }
    }
    return v;
}

double cosine(const Vec& a, const Vec& b) {
    if (a.empty() || b.empty()) return 0.0;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (const auto& [w, x] : a) {
        na += x * x;
        const auto it = b.find(w);
        if (it != b.end()) dot += x * it->second;
    }
    for (const auto& [w, x] : b) {
        (void)w;
        nb += x * x;
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace

TopicSegmenter::TopicSegmenter(const Tokenizer& tokenizer, TopicSegmentOptions options)
    : tokenizer_(tokenizer), options_(options) {}

std::vector<double> TopicSegmenter::blockSimilarities(
    const std::vector<std::vector<std::string>>& sentenceTokens) const {
    std::vector<double> sims;
    const int block = std::max(1, options_.blockSentences);
    const size_t n = sentenceTokens.size();
    if (n < static_cast<size_t>(block) * 2) return sims;

    const size_t blockCount = (n + static_cast<size_t>(block) - 1) / static_cast<size_t>(block);
    std::vector<Vec> vectors;
    vectors.reserve(blockCount);
    for (size_t b = 0; b < blockCount; ++b) {
        const size_t from = b * static_cast<size_t>(block);
        const size_t to = std::min(n, from + static_cast<size_t>(block));
        vectors.push_back(buildVector(sentenceTokens, from, to));
    }
    for (size_t i = 0; i + 1 < vectors.size(); ++i) {
        sims.push_back(cosine(vectors[i], vectors[i + 1]));
    }
    return sims;
}

std::vector<Topic> TopicSegmenter::segment(const std::vector<Sentence>& sentences) const {
    std::vector<Topic> topics;
    if (sentences.empty()) return topics;

    // 分词
    std::vector<std::vector<std::string>> tokens;
    tokens.reserve(sentences.size());
    for (const Sentence& s : sentences) {
        tokens.push_back(tokenizer_.cut(s.text, true));
    }

    auto makeTitle = [&](int from, int to) {
        // 取段内 TextRank 权重最高的若干词
        std::vector<std::vector<std::string>> local;
        for (int i = from; i < to && i < static_cast<int>(tokens.size()); ++i) {
            local.push_back(tokens[static_cast<size_t>(i)]);
        }
        const auto scores = TextRank::rankWords(local, 5);
        std::vector<std::pair<std::string, double>> ranked(scores.begin(), scores.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            if (std::fabs(a.second - b.second) > 1e-9) return a.second > b.second;
            return a.first < b.first;
        });
        std::vector<std::string> kws;
        for (const auto& [w, sc] : ranked) {
            (void)sc;
            if (static_cast<int>(kws.size()) >= options_.titleKeywordCount) break;
            if (str::utf8Length(w) < 2) continue;
            kws.push_back(w);
        }
        return kws;
    };

    const std::vector<double> sims = blockSimilarities(tokens);
    const int block = std::max(1, options_.blockSentences);

    // 无足够块 → 单议题
    if (sims.size() < 2) {
        Topic t;
        t.startSentence = 0;
        t.endSentence = static_cast<int>(sentences.size());
        t.keywords = makeTitle(0, t.endSentence);
        t.title = t.keywords.empty() ? "会议内容" : str::join(t.keywords, " / ");
        if (!sentences.empty()) {
            t.startMs = sentences.front().startMs;
            t.endMs = sentences.back().endMs;
        }
        topics.push_back(std::move(t));
        return topics;
    }

    // 谷底判定
    double mean = 0.0;
    for (double v : sims) mean += v;
    mean /= static_cast<double>(sims.size());
    double var = 0.0;
    for (double v : sims) var += (v - mean) * (v - mean);
    var /= static_cast<double>(sims.size());
    const double sd = std::sqrt(var);
    const double threshold = mean - options_.valleySigma * sd;

    std::vector<int> boundaries;  // 句索引
    boundaries.push_back(0);
    for (size_t i = 0; i < sims.size(); ++i) {
        const bool isValley = (sims[i] < threshold);
        // 局部极小
        const bool localMin = (i == 0 || sims[i] <= sims[i - 1]) &&
                              (i + 1 >= sims.size() || sims[i] <= sims[i + 1]);
        if (isValley && localMin) {
            const int sentenceIdx = static_cast<int>((i + 1) * static_cast<size_t>(block));
            if (sentenceIdx > boundaries.back() &&
                sentenceIdx < static_cast<int>(sentences.size())) {
                boundaries.push_back(sentenceIdx);
            }
        }
    }
    boundaries.push_back(static_cast<int>(sentences.size()));

    // 合并过短议题
    std::vector<int> merged;
    merged.push_back(boundaries.front());
    for (size_t i = 1; i < boundaries.size(); ++i) {
        const int span = boundaries[i] - merged.back();
        if (span < std::max(1, options_.minTopicSentences) && i + 1 < boundaries.size()) {
            continue;
        }
        merged.push_back(boundaries[i]);
    }
    if (merged.size() < 2) {
        merged.clear();
        merged.push_back(0);
        merged.push_back(static_cast<int>(sentences.size()));
    }

    for (size_t i = 0; i + 1 < merged.size(); ++i) {
        Topic t;
        t.startSentence = merged[i];
        t.endSentence = merged[i + 1];
        if (t.sentenceCount() <= 0) continue;
        t.keywords = makeTitle(t.startSentence, t.endSentence);
        t.title = t.keywords.empty() ? ("议题 " + std::to_string(topics.size() + 1))
                                     : str::join(t.keywords, " / ");
        if (t.startSentence < static_cast<int>(sentences.size())) {
            t.startMs = sentences[static_cast<size_t>(t.startSentence)].startMs;
        }
        if (t.endSentence - 1 < static_cast<int>(sentences.size())) {
            t.endMs = sentences[static_cast<size_t>(t.endSentence - 1)].endMs;
        }
        topics.push_back(std::move(t));
    }

    if (options_.maxTopics > 0 && static_cast<int>(topics.size()) > options_.maxTopics) {
        topics.resize(static_cast<size_t>(options_.maxTopics));
    }

    MM_LOG_INFO("topic") << "话题分段完成: " << topics.size() << " 个议题";
    return topics;
}

}  // namespace mm::nlp

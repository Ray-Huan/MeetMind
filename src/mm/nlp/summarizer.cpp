#include "mm/nlp/summarizer.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::nlp {

ExtractiveSummarizer::ExtractiveSummarizer(const Tokenizer& tokenizer, ExtractiveOptions options)
    : tokenizer_(tokenizer), options_(options) {}

size_t ExtractiveSummarizer::targetSentenceCount(size_t total) const {
    if (total == 0) return 0;
    double ratio = options_.ratio;
    if (ratio <= 0.0) ratio = 0.25;
    if (ratio > 1.0) ratio = 1.0;
    size_t k = static_cast<size_t>(std::ceil(static_cast<double>(total) * ratio));
    k = std::max(k, static_cast<size_t>(std::max(1, options_.minSentences)));
    k = std::min(k, static_cast<size_t>(std::max(1, options_.maxSentences)));
    k = std::min(k, total);
    return k;
}

std::vector<size_t> ExtractiveSummarizer::selectSentences(
    const std::vector<std::vector<std::string>>& sentenceTokens,
    const std::vector<double>& importance) const {
    std::vector<size_t> chosen;
    const size_t n = sentenceTokens.size();
    if (n == 0) return chosen;
    if (importance.size() != n) return chosen;
    if (n <= static_cast<size_t>(std::max(1, options_.minSentences))) {
        for (size_t i = 0; i < n; ++i) chosen.push_back(i);
        return chosen;
    }

    const size_t k = targetSentenceCount(n);
    const double lambda = std::max(0.0, std::min(1.0, options_.lambda));

    // 预计算句子两两相似度（n 为句数，通常 < 500，O(n²) 可接受）
    std::vector<std::vector<double>> sim(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const double s = TextRank::sentenceSimilarity(sentenceTokens[i], sentenceTokens[j]);
            sim[i][j] = s;
            sim[j][i] = s;
        }
    }

    std::vector<bool> picked(n, false);
    while (chosen.size() < k) {
        double bestScore = -1e30;
        size_t bestIdx = static_cast<size_t>(-1);
        for (size_t i = 0; i < n; ++i) {
            if (picked[i]) continue;
            double maxSim = 0.0;
            for (size_t j : chosen) {
                maxSim = std::max(maxSim, sim[i][j]);
            }
            const double score = lambda * importance[i] - (1.0 - lambda) * maxSim;
            // 同分时优先靠前的句子，保证确定性
            if (score > bestScore + 1e-12) {
                bestScore = score;
                bestIdx = i;
            }
        }
        if (bestIdx == static_cast<size_t>(-1)) break;
        picked[bestIdx] = true;
        chosen.push_back(bestIdx);
    }

    std::sort(chosen.begin(), chosen.end());
    return chosen;
}

Result<SummaryDraft> ExtractiveSummarizer::summarize(const SummaryRequest& request) {
    SummaryDraft draft;
    draft.engine = id();

    const std::vector<Sentence>& sentences = request.sentences;
    if (sentences.empty()) {
        MM_LOG_WARN("summarizer") << "输入句子为空，摘要为空";
        return draft;
    }

    // 先去重：完全相同的句子只保留首次出现，避免 MMR 反复选中同一句话
    std::vector<size_t> uniqueIdx;
    {
        std::unordered_set<std::string> seen;
        uniqueIdx.reserve(sentences.size());
        for (size_t i = 0; i < sentences.size(); ++i) {
            const std::string key = str::trim(sentences[i].text);
            if (key.empty()) continue;
            if (seen.insert(key).second) uniqueIdx.push_back(i);
        }
    }
    if (uniqueIdx.empty()) {
        MM_LOG_WARN("summarizer") << "去重后无有效句子，摘要为空";
        return draft;
    }

    // 分词（过滤停用词）用于相似度计算
    std::vector<std::vector<std::string>> tokens;
    tokens.reserve(uniqueIdx.size());
    for (size_t i : uniqueIdx) {
        tokens.push_back(tokenizer_.cut(sentences[i].text, true));
    }

    // 句级 TextRank 重要性
    std::vector<double> importance = TextRank::rankSentences(tokens, options_.textRank);
    TextRank::normalize(importance);

    const std::vector<size_t> chosenLocal = selectSentences(tokens, importance);
    if (chosenLocal.empty()) {
        MM_LOG_WARN("summarizer") << "未能选出任何句子";
        return draft;
    }

    draft.keyPoints.reserve(chosenLocal.size());
    for (size_t local : chosenLocal) {
        const Sentence& s = sentences[uniqueIdx[local]];
        SummarySentence ss;
        ss.text = s.text;
        ss.score = importance[local];
        ss.startMs = s.startMs;
        ss.endMs = s.endMs;
        ss.speakerId = s.speakerId;
        ss.order = s.index;
        draft.keyPoints.push_back(std::move(ss));
    }

    // 概览：取得分最高的若干要点（保持原序）
    std::vector<size_t> byScore(draft.keyPoints.size());
    for (size_t i = 0; i < byScore.size(); ++i) byScore[i] = i;
    std::sort(byScore.begin(), byScore.end(), [&](size_t a, size_t b) {
        return draft.keyPoints[a].score > draft.keyPoints[b].score;
    });
    const size_t overviewCount = std::min<size_t>(
        byScore.size(), static_cast<size_t>(std::max(1, options_.overviewSentences)));
    std::vector<size_t> overviewIdx(byScore.begin(), byScore.begin() + overviewCount);
    std::sort(overviewIdx.begin(), overviewIdx.end());

    std::string overview;
    for (size_t i : overviewIdx) {
        const std::string t = str::trim(draft.keyPoints[i].text);
        if (t.empty()) continue;
        if (!overview.empty()) overview += " ";
        overview += t;
    }
    draft.overview = overview;

    MM_LOG_INFO("summarizer") << "抽取式摘要完成：句数 " << sentences.size() << "（去重后 "
                              << uniqueIdx.size() << "） -> 要点 " << draft.keyPoints.size();
    return draft;
}

}  // namespace mm::nlp

#include "mm/nlp/keyword_extractor.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "mm/common/string_utils.h"
#include "mm/nlp/sentence_splitter.h"

namespace mm::nlp {
namespace {

bool isCandidate(const std::string& w, const Tokenizer& tk, const KeywordOptions& opt) {
    if (w.empty()) return false;
    if (static_cast<int>(str::utf8Length(w)) < opt.minLength) return false;
    if (tk.isStopword(w)) return false;
    if (!str::isContentToken(w)) return false;
    // 纯 ASCII 单字母或全数字已由 isContentToken 过滤；此处再排除纯符号
    bool hasAlnumOrCjk = false;
    for (char32_t c : str::toUtf32(w)) {
        if (str::isCjk(c) || str::isAsciiAlnum(c)) {
            hasAlnumOrCjk = true;
            break;
        }
    }
    return hasAlnumOrCjk;
}

}  // namespace

KeywordExtractor::KeywordExtractor(KeywordOptions options) : options_(options) {}

std::vector<Keyword> KeywordExtractor::extractFromTokens(
    const std::vector<std::vector<std::string>>& sentenceTokens, const Tokenizer& tokenizer) const {
    std::vector<Keyword> out;
    if (sentenceTokens.empty()) return out;

    // ---- 候选词与词频 ----
    std::unordered_map<std::string, int> tf;
    int totalTokens = 0;
    for (const auto& sent : sentenceTokens) {
        for (const std::string& w : sent) {
            if (!isCandidate(w, tokenizer, options_)) continue;
            ++tf[w];
            ++totalTokens;
        }
    }
    if (tf.empty() || totalTokens == 0) return out;

    // ---- 过滤后的句子序列（供 TextRank 使用）----
    std::vector<std::vector<std::string>> filtered;
    filtered.reserve(sentenceTokens.size());
    for (const auto& sent : sentenceTokens) {
        std::vector<std::string> f;
        for (const std::string& w : sent) {
            if (isCandidate(w, tokenizer, options_)) f.push_back(w);
        }
        if (!f.empty()) filtered.push_back(std::move(f));
    }
    if (filtered.empty()) return out;

    const std::unordered_map<std::string, double> trScores =
        TextRank::rankWords(filtered, options_.cooccurWindow, options_.textRank);

    // ---- TF-IDF ----
    const double corpusTotal = std::max(1.0, tokenizer.totalFrequency());
    std::unordered_map<std::string, double> tfidf;
    tfidf.reserve(tf.size());
    for (const auto& [w, count] : tf) {
        const double termFreq =
            static_cast<double>(count) / static_cast<double>(totalTokens);
        const double freq = tokenizer.wordFrequency(w);
        const double idf = std::log(1.0 + corpusTotal / (1.0 + freq));
        tfidf[w] = termFreq * idf;
    }

    // ---- 归一化 ----
    std::vector<double> trVals;
    trVals.reserve(tf.size());
    std::vector<double> tfVals;
    tfVals.reserve(tf.size());
    std::vector<std::string> words;
    words.reserve(tf.size());
    for (const auto& [w, count] : tf) {
        (void)count;
        words.push_back(w);
        const auto it = trScores.find(w);
        trVals.push_back(it == trScores.end() ? 0.0 : it->second);
        tfVals.push_back(tfidf[w]);
    }
    TextRank::normalize(trVals);
    TextRank::normalize(tfVals);

    const double alpha = std::max(0.0, std::min(1.0, options_.alpha));
    for (size_t i = 0; i < words.size(); ++i) {
        Keyword k;
        k.word = words[i];
        k.textRank = trVals[i];
        k.tfidf = tfVals[i];
        k.frequency = tf[words[i]];
        k.score = alpha * trVals[i] + (1.0 - alpha) * tfVals[i];

        // 长词与高频词的轻微加成（避免单字虚词挤入榜单）
        const size_t len = str::utf8Length(k.word);
        if (len >= 3) k.score *= 1.05;
        if (k.frequency >= 3) k.score *= 1.03;
        out.push_back(std::move(k));
    }

    std::sort(out.begin(), out.end(), [](const Keyword& a, const Keyword& b) {
        if (std::fabs(a.score - b.score) > 1e-9) return a.score > b.score;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        return a.word < b.word;
    });

    if (options_.topK > 0 && static_cast<int>(out.size()) > options_.topK) {
        out.resize(static_cast<size_t>(options_.topK));
    }
    return out;
}

std::vector<Keyword> KeywordExtractor::extract(const std::string& text,
                                               const Tokenizer& tokenizer) const {
    const std::vector<Sentence> sentences = SentenceSplitter{}.split(text);
    std::vector<std::vector<std::string>> tokens;
    tokens.reserve(sentences.size());
    for (const Sentence& s : sentences) {
        tokens.push_back(tokenizer.cut(s.text, false));
    }
    return extractFromTokens(tokens, tokenizer);
}

}  // namespace mm::nlp

#include "mm/nlp/sentence_splitter.h"

#include <algorithm>
#include <cmath>

#include "mm/common/string_utils.h"
#include "mm/nlp/tokenizer.h"

namespace mm::nlp {
namespace {

/// 在逗号/分号处找最接近 target 的切分点（码点索引）。
size_t findBreakNear(const std::u32string& u, size_t begin, size_t end, size_t target) {
    static const char32_t kBreaks[] = {U'，', U'；', U'、', U',', U';', U'：', U':'};
    size_t best = std::u32string::npos;
    size_t bestDist = static_cast<size_t>(-1);
    for (size_t i = begin; i < end; ++i) {
        for (char32_t b : kBreaks) {
            if (u[i] == b) {
                const size_t dist = (i > target) ? (i - target) : (target - i);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i + 1;  // 标点归入前一句
                }
            }
        }
    }
    return best;
}

/// 粗略估算时间分配比例（以字符数为权重）。
int64_t allocateMs(int totalMs, int partChars, int totalChars) {
    if (totalChars <= 0) return 0;
    return totalMs * static_cast<int64_t>(partChars) / static_cast<int64_t>(totalChars);
}

std::vector<std::string> splitCore(const std::string& text, const SentenceSplitOptions& options) {
    std::vector<std::string> out;
    const std::u32string u = str::toUtf32(text);
    if (u.empty()) return out;

    size_t start = 0;
    for (size_t i = 0; i < u.size(); ++i) {
        const char32_t c = u[i];
        bool isEnd = str::isSentenceEnd(c);
        // 省略号/引号收尾
        if (isEnd) {
            size_t end = i + 1;
            // 吸收紧随其后的成对引号/括号
            while (end < u.size() && (u[end] == U'”' || u[end] == U'）' || u[end] == U'"' ||
                                      u[end] == U'\'')) {
                ++end;
            }
            std::string piece = str::toUtf8(u.substr(start, end - start));
            const std::string trimmed = str::trim(piece);
            if (!trimmed.empty()) out.push_back(trimmed);
            start = end;
            i = end - 1;
            continue;
        }
        if (c == U'\n') {
            std::string piece = str::toUtf8(u.substr(start, i - start));
            const std::string trimmed = str::trim(piece);
            if (!trimmed.empty()) out.push_back(trimmed);
            start = i + 1;
        }
    }
    if (start < u.size()) {
        std::string piece = str::toUtf8(u.substr(start));
        const std::string trimmed = str::trim(piece);
        if (!trimmed.empty()) out.push_back(trimmed);
    }

    // 二次切分超长句
    if (options.maxSentenceChars > 0) {
        std::vector<std::string> refined;
        for (const std::string& s : out) {
            std::u32string su = str::toUtf32(s);
            if (static_cast<int>(su.size()) <= options.maxSentenceChars) {
                refined.push_back(s);
                continue;
            }
            size_t begin = 0;
            while (begin < su.size()) {
                const size_t remaining = su.size() - begin;
                if (static_cast<int>(remaining) <= options.maxSentenceChars) {
                    const std::string tail = str::trim(str::toUtf8(su.substr(begin)));
                    if (!tail.empty()) refined.push_back(tail);
                    break;
                }
                const size_t target = begin + static_cast<size_t>(options.maxSentenceChars);
                const size_t cut = findBreakNear(su, begin + options.maxSentenceChars / 2,
                                                 su.size(), target);
                const size_t useCut = (cut == std::u32string::npos) ? target : cut;
                const std::string piece = str::trim(str::toUtf8(su.substr(begin, useCut - begin)));
                if (!piece.empty()) refined.push_back(piece);
                begin = useCut;
            }
        }
        out = std::move(refined);
    }

    return out;
}

}  // namespace

SentenceSplitter::SentenceSplitter(SentenceSplitOptions options) : options_(options) {}

std::vector<Sentence> SentenceSplitter::split(const std::string& text) const {
    std::vector<Sentence> out;
    const std::vector<std::string> pieces = splitCore(text, options_);
    out.reserve(pieces.size());
    int idx = 0;
    for (const std::string& p : pieces) {
        Sentence s;
        s.text = p;
        s.index = idx++;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Sentence> SentenceSplitter::splitTimed(const std::vector<TimedText>& segments) const {
    std::vector<Sentence> out;
    int idx = 0;
    for (const TimedText& seg : segments) {
        const std::vector<std::string> pieces = splitCore(seg.text, options_);
        if (pieces.empty()) continue;

        const int64_t dur = seg.endMs > seg.startMs ? seg.endMs - seg.startMs : 0;
        int totalChars = 0;
        for (const std::string& p : pieces) totalChars += static_cast<int>(str::utf8Length(p));
        if (totalChars <= 0) totalChars = 1;

        int64_t cursor = seg.startMs;
        for (const std::string& p : pieces) {
            const int chars = static_cast<int>(str::utf8Length(p));
            Sentence s;
            s.text = p;
            s.index = idx++;
            s.startMs = cursor;
            s.endMs = cursor + allocateMs(dur, chars, totalChars);
            s.speakerId = seg.speakerId;
            if (s.endMs > seg.endMs) s.endMs = seg.endMs;
            cursor = s.endMs;
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<std::string> SentenceSplitter::splitByWeights(const std::string& text,
                                                         const std::vector<int64_t>& weights,
                                                         const Tokenizer* tokenizer) {
    std::vector<std::string> out(weights.size());
    if (weights.empty()) return out;
    if (weights.size() == 1) {
        out[0] = str::trim(text);
        return out;
    }

    const std::u32string u = str::toUtf32(text);
    if (u.empty()) return out;

    // 输入不足：字符数少于容器数时，前若干个容器各分到一个字符，其余为空
    const size_t buckets = weights.size();
    if (u.size() < buckets) {
        for (size_t i = 0; i < u.size(); ++i) {
            out[i] = str::toUtf8(std::u32string(1, u[i]));
        }
        return out;
    }

    // ---- 按权重换算切点 ----
    //
    // 关键设计（曾经踩过的坑）：早期实现是「先切句，再按句数分配」——
    // 一旦整段话只有 1 个句子（中文 ASR 输出很常见，甚至整段无标点），
    // 所有文本都会堆到第一个段，其余段全为空。后果是：
    //   * 一个转写单元跨多位说话人时，整段文本被错误地算给第一位说话人；
    //   * SRT 字幕时间轴全部挤在单元开头；
    //   * 92 段实测有 60 段为空文本。
    // 现在改为「按权重直接切字符」，保证文本完整覆盖所有段；切点优先落在标点上，
    // 找不到标点时退化为按字符切（宁可断词，也不丢归属）。
    std::vector<double> cum(buckets, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < buckets; ++i) {
        total += static_cast<double>(std::max<int64_t>(0, weights[i]));
    }
    const bool equalSplit = (total <= 0.0);
    if (equalSplit) {
        total = static_cast<double>(buckets);
    }
    {
        double acc = 0.0;
        for (size_t i = 0; i < buckets; ++i) {
            acc += equalSplit ? 1.0 : static_cast<double>(std::max<int64_t>(0, weights[i]));
            cum[i] = acc;
        }
    }

    // 词边界集合（码点索引 → 该词结束位置）。中文 ASR 输出常整段无标点，
    // 此时「词边界」是唯一不会把词语切成两半的安全切点。
    std::vector<uint8_t> isTokenEnd(u.size() + 1, 0);
    if (tokenizer != nullptr && tokenizer->loaded()) {
        for (const Token& t : tokenizer->tokenize(text, false)) {
            const size_t end = static_cast<size_t>(t.startChar + t.length);
            if (end <= u.size()) isTokenEnd[end] = 1;
        }
    }

    // 断点优先级：句末标点(3) > 逗号类标点(2) > 词边界(1) > 任意位置(0)
    auto findBreak = [&](size_t from, size_t target, size_t limit) -> size_t {
        const size_t window = 8;
        const size_t lo = std::max(from, target > window ? target - window : from);
        const size_t hi = std::min(limit, target + window);
        size_t bestEnd = 0;
        int bestRank = 0;
        size_t bestDist = static_cast<size_t>(-1);
        for (size_t i = lo; i < hi; ++i) {
            int rank = 0;
            if (str::isSentenceEnd(u[i])) {
                rank = 3;
            } else if (u[i] == U'，' || u[i] == U'、' || u[i] == U'；' || u[i] == U',') {
                rank = 2;
            } else if (isTokenEnd[i + 1] != 0) {
                rank = 1;
            }
            if (rank == 0) continue;
            const size_t dist = (i + 1 > target) ? (i + 1 - target) : (target - (i + 1));
            if (rank > bestRank || (rank == bestRank && dist < bestDist)) {
                bestRank = rank;
                bestDist = dist;
                bestEnd = i + 1;
            }
        }
        return bestEnd;
    };

    const size_t n = u.size();
    size_t cursor = 0;
    for (size_t k = 0; k + 1 < buckets; ++k) {
        const double frac = cum[k] / total;
        size_t target = static_cast<size_t>(std::llround(frac * static_cast<double>(n)));
        // 为后续每个容器至少留 1 个字符，且当前至少取 1 个字符
        const size_t remainingBuckets = buckets - k - 1;
        const size_t maxPos = (n > remainingBuckets) ? (n - remainingBuckets) : n;
        if (target < cursor + 1) target = cursor + 1;
        if (target > maxPos) target = maxPos;
        size_t cut = findBreak(cursor, target, maxPos);
        if (cut <= cursor) cut = target;
        if (cut > maxPos) cut = maxPos;
        if (cut <= cursor) cut = std::min(n, cursor + 1);

        out[k] = str::trim(str::toUtf8(u.substr(cursor, cut - cursor)));
        cursor = cut;
    }
    out[buckets - 1] = str::trim(str::toUtf8(u.substr(cursor)));
    return out;
}

}  // namespace mm::nlp

#include "mm/nlp/tokenizer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

#include "mm/common/path_utils.h"
#include "mm/common/config.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::nlp {
namespace {

constexpr double kUnknownLogProb = -30.0;

bool asciiTokenChar(char32_t c) {
    // 允许技术词中的连接符：C++、F-16、Wi-Fi、v2.0、50%
    return str::isAsciiAlnum(c) || c == U'.' || c == U'-' || c == U'_' || c == U'+' ||
           c == U'#' || c == U'%' || c == U'/' || c == U'@';
}

}  // namespace

Tokenizer::Tokenizer(TokenizerConfig config) : config_(config) {}

Result<void> Tokenizer::loadLexicon(const std::string& path) {
    if (path.empty() || !pathutil::exists(path)) {
        return fail(ErrorCode::FileNotFound, "词典文件不存在: " + path);
    }
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::IoError, "无法打开词典: " + path);

    std::unordered_map<std::string, double> freq;
    freq.reserve(200000);
    double total = 0.0;
    size_t maxLen = 1;
    std::string line;
    size_t lineNo = 0;
    size_t skipped = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos) {
            ++skipped;
            continue;
        }
        const std::string word = line.substr(0, tab1);
        if (word.empty()) {
            ++skipped;
            continue;
        }
        size_t freqEnd = line.find('\t', tab1 + 1);
        if (freqEnd == std::string::npos) freqEnd = line.size();
        double f = 0.0;
        try {
            f = std::stod(line.substr(tab1 + 1, freqEnd - tab1 - 1));
        } catch (...) {
            ++skipped;
            continue;
        }
        if (f <= 0.0) f = 1.0;

        const size_t cpLen = str::utf8Length(word);
        if (cpLen > config_.maxWordLength) {
            ++skipped;
            continue;
        }
        maxLen = std::max(maxLen, cpLen);
        freq[word] = f;
        total += f;
    }

    if (freq.empty()) {
        return fail(ErrorCode::InvalidArgument, "词典为空或格式不正确: " + path);
    }

    freq_ = std::move(freq);
    totalFreq_ = total > 0 ? total : 1.0;
    config_.maxWordLength = maxLen;
    lexiconPath_ = path;

    // 预计算对数概率
    const double logTotal = std::log(totalFreq_);
    for (auto& [w, f] : freq_) {
        f = std::log(f) - logTotal;
    }

    MM_LOG_INFO("tokenizer") << "词典加载完成: " << freq_.size() << " 词, 最长 "
                             << maxLen << " 字, 跳过 " << skipped << " 行 (" << path << ")";
    return okStatus();
}

Result<void> Tokenizer::mergeLexicon(const std::string& path) {
    if (path.empty() || !pathutil::exists(path)) {
        return fail(ErrorCode::FileNotFound, "补充词典不存在: " + path);
    }
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::IoError, "无法打开补充词典: " + path);

    size_t added = 0;
    size_t updated = 0;
    size_t maxLen = config_.maxWordLength;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos) continue;
        const std::string word = line.substr(0, tab1);
        if (word.empty()) continue;
        size_t freqEnd = line.find('\t', tab1 + 1);
        if (freqEnd == std::string::npos) freqEnd = line.size();
        double f = 1.0;
        try {
            f = std::stod(line.substr(tab1 + 1, freqEnd - tab1 - 1));
        } catch (...) {
            continue;
        }
        if (f <= 0.0) f = 1.0;

        const size_t cpLen = str::utf8Length(word);
        if (cpLen > 16) continue;
        maxLen = std::max(maxLen, cpLen);

        const double logProb = std::log(f) - std::log(std::max(1.0, totalFreq_));
        auto it = freq_.find(word);
        if (it == freq_.end()) {
            freq_.emplace(word, logProb);
            ++added;
        } else if (logProb > it->second) {
            it->second = logProb;  // 已有词条取更大概率
            ++updated;
        }
    }
    config_.maxWordLength = maxLen;
    MM_LOG_INFO("tokenizer") << "补充词典合并完成: 新增 " << added << " 词, 调整 " << updated
                             << " 词 (" << path << ")";
    return okStatus();
}

Result<void> Tokenizer::loadDefaultLexicon(const std::string& explicitPath) {
    std::string dir;
    std::string path = explicitPath;
    if (path.empty()) {
        dir = Config::locateDataDir();
        if (!dir.empty()) path = pathutil::join(dir, "lexicon_zh.txt");
    }
    if (path.empty()) {
        const std::string src = pathutil::join(MEETMIND_SOURCE_DIR, "data/lexicon_zh.txt");
        if (pathutil::exists(src)) {
            path = src;
            dir = pathutil::join(MEETMIND_SOURCE_DIR, "data");
        }
    }
    if (path.empty()) {
        return fail(ErrorCode::FileNotFound,
                    "未找到内置词典 data/lexicon_zh.txt，请运行 tools/prepare_data.py 生成");
    }
    Result<void> loaded = loadLexicon(path);
    if (!loaded.ok()) return loaded;

    // 追加会议领域补充词典（可选）：通用词典缺少「排期」等职场术语
    const std::string extra =
        pathutil::join(dir.empty() ? pathutil::parentPath(path) : dir, "lexicon_extra.txt");
    if (pathutil::exists(extra)) {
        Result<void> merged = mergeLexicon(extra);
        if (!merged.ok()) {
            MM_LOG_WARN("tokenizer") << "补充词典加载失败: " << merged.message();
        }
    }
    return okStatus();
}

void Tokenizer::setStopwords(std::vector<std::string> words) {
    stopwords_.clear();
    stopwords_.reserve(words.size());
    for (std::string& w : words) {
        if (!w.empty()) stopwords_.emplace(std::move(w), static_cast<uint8_t>(1));
    }
}

bool Tokenizer::isStopword(const std::string& word) const {
    return stopwords_.find(word) != stopwords_.end();
}

double Tokenizer::wordFrequency(const std::string& word) const {
    const auto it = freq_.find(word);
    if (it == freq_.end()) return 0.0;
    return std::exp(it->second);
}

void Tokenizer::segmentCjkRun(const std::string& text, const std::vector<size_t>& charStart,
                              int begin, int end, std::vector<Token>& out) const {
    const int n = end - begin;
    if (n <= 0) return;

    // 局部 DAG（坐标相对 begin）
    std::vector<std::vector<int>> dag(static_cast<size_t>(n));
    std::string buffer;
    for (int i = 0; i < n; ++i) {
        const int maxEnd = std::min(n, i + static_cast<int>(config_.maxWordLength));
        buffer.clear();
        for (int j = i; j < maxEnd; ++j) {
            const size_t gs = charStart[static_cast<size_t>(begin + j)];
            buffer.append(text, gs, charStart[static_cast<size_t>(begin + j) + 1] - gs);
            if (freq_.find(buffer) != freq_.end()) {
                dag[static_cast<size_t>(i)].push_back(j + 1);
            }
        }
        if (dag[static_cast<size_t>(i)].empty()) dag[static_cast<size_t>(i)].push_back(i + 1);
    }

    // 反向动态规划
    const double kNegInf = -std::numeric_limits<double>::infinity();
    std::vector<double> route(static_cast<size_t>(n) + 1, kNegInf);
    std::vector<int> nextIndex(static_cast<size_t>(n) + 1, 0);
    route[static_cast<size_t>(n)] = 0.0;

    const double unknownLog = std::log(std::max(1e-9, config_.unknownWordFrequency)) -
                              std::log(totalFreq_);
    const double effectiveUnknown =
        std::max(kUnknownLogProb, std::min(0.0, unknownLog));  // 稳定化

    for (int i = n - 1; i >= 0; --i) {
        double best = kNegInf;
        int bestEnd = i + 1;
        for (int j : dag[static_cast<size_t>(i)]) {
            if (route[static_cast<size_t>(j)] == kNegInf) continue;
            const size_t gs = charStart[static_cast<size_t>(begin + i)];
            const size_t ge = charStart[static_cast<size_t>(begin + j)];
            const auto it = freq_.find(text.substr(gs, ge - gs));
            const double p = (it != freq_.end()) ? it->second : effectiveUnknown;
            const double score = p + route[static_cast<size_t>(j)];
            if (score > best) {
                best = score;
                bestEnd = j;
            }
        }
        if (best == kNegInf) {
            best = effectiveUnknown + route[static_cast<size_t>(i + 1)];
            bestEnd = i + 1;
        }
        route[static_cast<size_t>(i)] = best;
        nextIndex[static_cast<size_t>(i)] = bestEnd;
    }

    // 回溯
    int i = 0;
    while (i < n) {
        int j = nextIndex[static_cast<size_t>(i)];
        if (j <= i) j = i + 1;
        const size_t gs = charStart[static_cast<size_t>(begin + i)];
        const size_t ge = charStart[static_cast<size_t>(begin + j)];
        Token t;
        t.text = text.substr(gs, ge - gs);
        t.startChar = begin + i;
        t.length = j - i;
        t.inLexicon = (freq_.find(t.text) != freq_.end());
        t.logProb = route[static_cast<size_t>(i)] - route[static_cast<size_t>(j)];
        t.isAscii = false;
        out.push_back(std::move(t));
        i = j;
    }
}

std::vector<Token> Tokenizer::tokenize(const std::string& text, bool filterStopwords) const {
    std::vector<Token> out;
    if (text.empty() || freq_.empty()) return out;

    const std::u32string u = str::toUtf32(text);
    // 码点起始字节偏移
    std::vector<size_t> charStart;
    charStart.reserve(u.size() + 1);
    {
        size_t bytePos = 0;
        for (char32_t c : u) {
            charStart.push_back(bytePos);
            bytePos += str::toUtf8(std::u32string(1, c)).size();
        }
        charStart.push_back(text.size());
    }

    const size_t n = u.size();
    size_t i = 0;
    while (i < n) {
        const char32_t c = u[i];
        if (str::isCjk(c)) {
            size_t j = i;
            while (j < n && str::isCjk(u[j])) ++j;
            segmentCjkRun(text, charStart, static_cast<int>(i), static_cast<int>(j), out);
            i = j;
            continue;
        }
        if (config_.groupAsciiRuns && asciiTokenChar(c)) {
            size_t j = i;
            while (j < n && asciiTokenChar(u[j])) ++j;
            // 修剪尾部的 . - _ 等连接符
            while (j > i + 1) {
                const char32_t last = u[j - 1];
                if (last == U'.' || last == U'-' || last == U'_' || last == U'+' || last == U'#' ||
                    last == U'/' || last == U'@') {
                    --j;
                } else {
                    break;
                }
            }
            const size_t bs = charStart[i];
            const size_t be = charStart[j];
            Token t;
            t.text = text.substr(bs, be - bs);
            t.startChar = static_cast<int>(i);
            t.length = static_cast<int>(j - i);
            t.inLexicon = (freq_.find(t.text) != freq_.end());
            t.isAscii = true;
            out.push_back(std::move(t));
            i = j;
            continue;
        }
        ++i;  // 空白与标点跳过
    }

    if (filterStopwords && !stopwords_.empty()) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [this](const Token& t) { return isStopword(t.text); }),
                  out.end());
    }
    return out;
}

std::vector<std::string> Tokenizer::cut(const std::string& text, bool filterStopwords) const {
    std::vector<std::string> words;
    for (const Token& t : tokenize(text, filterStopwords)) {
        words.push_back(t.text);
    }
    return words;
}

const Tokenizer& sharedTokenizer() {
    static Tokenizer* instance = nullptr;
    static std::once_flag flag;
    std::call_once(flag, [] {
        static Tokenizer tokenizer;
        Result<void> r = tokenizer.loadDefaultLexicon();
        if (!r.ok()) {
            MM_LOG_ERROR("tokenizer") << "内置词典加载失败: " << r.message();
        }
        instance = &tokenizer;
    });
    return *instance;
}

}  // namespace mm::nlp

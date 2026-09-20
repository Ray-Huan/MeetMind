// NLP 层测试：ITN / 标点恢复 / 分词 / 句切分 / TextRank / 关键词 / 摘要 / 待办 / 决议 / 话题 / 纪要
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "mm/common/string_utils.h"
#include "mm/nlp/action_item_extractor.h"
#include "mm/nlp/decision_extractor.h"
#include "mm/nlp/keyword_extractor.h"
#include "mm/nlp/minutes_builder.h"
#include "mm/nlp/punctuation_restorer.h"
#include "mm/nlp/sentence_splitter.h"
#include "mm/nlp/summarizer.h"
#include "mm/nlp/text_normalizer.h"
#include "mm/nlp/textrank.h"
#include "mm/nlp/t2s.h"
#include "mm/nlp/tokenizer.h"
#include "mm/nlp/topic_segmenter.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

namespace {

/// 共享分词器（惰性加载内置词典，只加载一次）
const nlp::Tokenizer& tk() {
    static nlp::Tokenizer tokenizer;
    static bool loaded = false;
    if (!loaded) {
        Result<void> r = tokenizer.loadDefaultLexicon();
        MM_EXPECT_TRUE(r.ok());
        loaded = true;
    }
    return tokenizer;
}

std::vector<nlp::Sentence> makeSentences(const std::vector<std::string>& texts,
                                         const std::vector<int>& speakers = {}) {
    std::vector<nlp::Sentence> out;
    int64_t t = 0;
    for (size_t i = 0; i < texts.size(); ++i) {
        nlp::Sentence s;
        s.text = texts[i];
        s.index = static_cast<int>(i);
        s.startMs = t;
        s.endMs = t + 3000;
        s.speakerId = speakers.empty() ? 0 : speakers[std::min(i, speakers.size() - 1)];
        t += 3500;
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

namespace {

/// 测试用自定义摘要后端：验证 ISummarizer 的插拔能力
/// （真实场景下可替换为其它摘要实现）
class StubSummarizer final : public nlp::ISummarizer {
public:
    StubSummarizer(bool available, bool remote) : available_(available), remote_(remote) {}
    std::string id() const override { return "stub"; }
    std::string displayName() const override { return "测试用自定义摘要后端"; }
    bool available() const override { return available_; }
    bool isRemote() const override { return remote_; }
    Result<nlp::SummaryDraft> summarize(const nlp::SummaryRequest& req) override {
        nlp::SummaryDraft d;
        d.engine = id();
        d.overview = "【自定义后端】" + req.title;
        if (!req.sentences.empty()) {
            nlp::SummarySentence s;
            s.text = req.sentences.front().text;
            s.startMs = req.sentences.front().startMs;
            s.endMs = req.sentences.front().endMs;
            s.score = 1.0;
            d.keyPoints.push_back(s);
        }
        return d;
    }

private:
    bool available_ = true;
    bool remote_ = false;
};

}  // namespace

// ============================ ITN ============================

MM_TEST(nlp_itn, 中文数词转阿拉伯数字) {
    nlp::TextNormalizer itn;
    MM_EXPECT_EQ(itn.normalize("百分之三十"), std::string("30%"));
    MM_EXPECT_EQ(itn.normalize("百分之三点五"), std::string("3.5%"));
    MM_EXPECT_EQ(itn.normalize("三千万"), std::string("30000000"));
    MM_EXPECT_EQ(itn.normalize("两千零五"), std::string("2005"));
    MM_EXPECT_EQ(itn.normalize("一百二十"), std::string("120"));
    MM_EXPECT_EQ(itn.normalize("十五"), std::string("15"));
    MM_EXPECT_EQ(itn.normalize("二零二六年"), std::string("2026年"));
    MM_EXPECT_EQ(itn.normalize("三点五"), std::string("3.5"));
}

MM_TEST(nlp_itn, 已是数字时保持幂等) {
    nlp::TextNormalizer itn;
    const char* cases[] = {"30%", "2026年9月14日", "v2.0", "共 15 人", "3.5%", "100 万"};
    for (const char* c : cases) {
        MM_EXPECT_EQ(itn.normalize(c), std::string(c));
    }
}

MM_TEST(nlp_itn, 全角转半角与空白折叠) {
    nlp::TextNormalizer itn;
    MM_EXPECT_EQ(itn.normalize("ＡＢＣ１２３"), std::string("ABC123"));
    MM_EXPECT_EQ(itn.normalize("共   15   人"), std::string("共 15 人"));
}

MM_TEST(nlp_itn, 不误伤量词与序数) {
    nlp::TextNormalizer itn;
    // 单字数字+量词不应被转换
    MM_EXPECT_EQ(itn.normalize("一个人"), std::string("一个人"));
    MM_EXPECT_EQ(itn.normalize("三个需求"), std::string("三个需求"));
    MM_EXPECT_EQ(itn.normalize("一起推进"), std::string("一起推进"));
}

MM_TEST(nlp_itn, 数词解析边界) {
    long long v = 0;
    MM_EXPECT_TRUE(nlp::TextNormalizer::parseChineseInteger("一万零五", &v));
    MM_EXPECT_EQ(v, 10005LL);
    MM_EXPECT_TRUE(nlp::TextNormalizer::parseChineseInteger("一亿", &v));
    MM_EXPECT_EQ(v, 100000000LL);
    MM_EXPECT_FALSE(nlp::TextNormalizer::parseChineseInteger("", &v));
    MM_EXPECT_FALSE(nlp::TextNormalizer::parseChineseInteger("abc", &v));

    double d = 0.0;
    MM_EXPECT_TRUE(nlp::TextNormalizer::parseChineseNumber("三点一四", &d));
    MM_EXPECT_NEAR(d, 3.14, 1e-9);
}

// ============================ 标点恢复 ============================

MM_TEST(nlp_punct, 已有标点时幂等) {
    nlp::PunctuationRestorer r;
    MM_EXPECT_EQ(r.restore("这是一句话。", {}), std::string("这是一句话。"));
    MM_EXPECT_EQ(r.restore("可以吗？", {}), std::string("可以吗？"));
    MM_EXPECT_EQ(r.restore("", {}), std::string(""));
}

MM_TEST(nlp_punct, 依据停顿长度补标点) {
    nlp::PunctuationRestorer r;
    nlp::PunctuationContext ctx;
    ctx.gapToNextMs = 1000;
    MM_EXPECT_EQ(r.restore("我们开始吧", ctx), std::string("我们开始吧。"));

    ctx.gapToNextMs = 500;
    MM_EXPECT_EQ(r.restore("我们开始吧", ctx), std::string("我们开始吧，"));

    ctx.gapToNextMs = 100;
    MM_EXPECT_EQ(r.restore("我们开始吧", ctx), std::string("我们开始吧"));

    nlp::PunctuationContext last;
    last.isLast = true;
    MM_EXPECT_EQ(r.restore("这就是今天的结论", last), std::string("这就是今天的结论。"));
}

MM_TEST(nlp_punct, 疑问与祈使句判定) {
    nlp::PunctuationRestorer r;
    nlp::PunctuationContext ctx;
    ctx.gapToNextMs = 1200;
    MM_EXPECT_EQ(r.restore("这个方案可以吗", ctx), std::string("这个方案可以吗？"));
    MM_EXPECT_EQ(r.restore("请尽快确认排期", ctx), std::string("请尽快确认排期。"));

    MM_EXPECT_TRUE(nlp::PunctuationRestorer::looksInterrogative("这样行不行"));
    MM_EXPECT_TRUE(nlp::PunctuationRestorer::looksInterrogative("为什么要延期"));
    MM_EXPECT_FALSE(nlp::PunctuationRestorer::looksInterrogative("就这样定了"));
    MM_EXPECT_TRUE(nlp::PunctuationRestorer::looksImperative("麻烦你跟进一下"));
}

MM_TEST(nlp_punct, 超长无标点串插入内部标点) {
    nlp::PunctuationOptions opt;
    opt.longTextChars = 10;
    opt.veryLongTextChars = 100;
    nlp::PunctuationRestorer r(opt);
    const std::string longText = "我们先把需求梳理清楚但是时间比较紧张所以我们优先保证核心功能上线";
    const std::string out = r.restore(longText, {});
    MM_EXPECT_TRUE(out.find("，") != std::string::npos);
    MM_EXPECT_GT(static_cast<int>(mm::str::utf8Length(out)),
                 static_cast<int>(mm::str::utf8Length(longText)));
    // 除插入标点外不应丢失内容
    std::string stripped = out;
    stripped.erase(std::remove(stripped.begin(), stripped.end(), ','), stripped.end());
    MM_EXPECT_EQ(mm::str::utf8Length(stripped) >= mm::str::utf8Length(longText), true);
}

MM_TEST(nlp_punct, 批量恢复) {
    std::vector<std::string> texts = {"第一句", "第二句", "第三句"};
    std::vector<int64_t> gaps = {1000, 120};
    nlp::PunctuationRestorer{}.restoreSequence(texts, gaps);
    MM_EXPECT_EQ(texts[0], std::string("第一句。"));
    MM_EXPECT_EQ(texts[1], std::string("第二句"));
    MM_EXPECT_EQ(texts[2], std::string("第三句。"));
}

// ============================ 分词 ============================

MM_TEST(nlp_tokenizer, 词典加载成功) {
    MM_EXPECT_TRUE(tk().loaded());
    MM_EXPECT_GT(tk().lexiconSize(), static_cast<size_t>(50000));
    MM_EXPECT_GT(tk().totalFrequency(), 1.0);
    MM_EXPECT_TRUE(tk().inLexicon("我们"));
    MM_EXPECT_TRUE(tk().inLexicon("项目"));
}

MM_TEST(nlp_tokenizer, 分词保持原文可还原) {
    const std::string text = "我们讨论一下项目的排期和上线时间";
    const std::vector<std::string> words = tk().cut(text);
    MM_EXPECT_GT(words.size(), static_cast<size_t>(3));
    MM_EXPECT_EQ(mm::str::join(words, ""), text);
}

MM_TEST(nlp_tokenizer, 常见词组合成整词) {
    const std::vector<std::string> w1 = tk().cut("我们今天开项目评审会");
    MM_EXPECT_GT(std::count(w1.begin(), w1.end(), std::string("我们")), 0);
    MM_EXPECT_GT(std::count(w1.begin(), w1.end(), std::string("项目")), 0);
    // 「需求评审」在补充词典中是整体词；若词典未覆盖则退化为「需求」+「评审」
    const std::vector<std::string> w2 = tk().cut("需求评审");
    const bool asWhole = std::count(w2.begin(), w2.end(), std::string("需求评审")) > 0;
    const bool asParts = std::count(w2.begin(), w2.end(), std::string("需求")) > 0;
    MM_EXPECT_TRUE(asWhole || asParts);
    MM_EXPECT_EQ(mm::str::join(w2, ""), std::string("需求评审"));
}

MM_TEST(nlp_tokenizer, 英文数字作为整体词元) {
    const std::vector<std::string> w = tk().cut("使用API接口v2.0处理100条数据");
    bool hasApi = false;
    bool hasVersion = false;
    for (const std::string& t : w) {
        if (t == "API") hasApi = true;
        if (t == "v2.0") hasVersion = true;
    }
    MM_EXPECT_TRUE(hasApi);
    MM_EXPECT_TRUE(hasVersion);
}

MM_TEST(nlp_tokenizer, 停用词过滤) {
    nlp::Tokenizer local;
    MM_REQUIRE_TRUE(local.loadDefaultLexicon().ok());
    local.setStopwords({"的", "了", "我们"});
    const std::vector<std::string> filtered = local.cut("我们的项目已经完成了", true);
    for (const std::string& t : filtered) {
        MM_EXPECT_NE(t, std::string("的"));
        MM_EXPECT_NE(t, std::string("了"));
        MM_EXPECT_NE(t, std::string("我们"));
    }
    MM_EXPECT_TRUE(local.isStopword("的"));
    MM_EXPECT_FALSE(local.isStopword("项目"));
}

MM_TEST(nlp_tokenizer, 词频查询与未知词) {
    MM_EXPECT_GT(tk().wordFrequency("我们"), 0.0);
    MM_EXPECT_LT(tk().wordFrequency("我们"), 1.0);  // 返回归一化相对频率
    MM_EXPECT_NEAR(tk().wordFrequency("一个不存在的虚构词语xyz"), 0.0, 1e-12);
    MM_EXPECT_FALSE(tk().inLexicon("一个不存在的虚构词语xyz"));
}

MM_TEST(nlp_tokenizer, 空输入与超短输入) {
    MM_EXPECT_EQ(tk().cut("").size(), static_cast<size_t>(0));
    MM_EXPECT_EQ(tk().cut("   ").size(), static_cast<size_t>(0));
    MM_EXPECT_EQ(tk().cut("。，！").size(), static_cast<size_t>(0));
    const std::vector<std::string> single = tk().cut("好");
    MM_EXPECT_EQ(single.size(), static_cast<size_t>(1));
}

MM_TEST(nlp_tokenizer, 无词典时安全返回空) {
    nlp::Tokenizer empty;
    MM_EXPECT_FALSE(empty.loaded());
    MM_EXPECT_EQ(empty.cut("我们讨论项目").size(), static_cast<size_t>(0));
    MM_EXPECT_FALSE(empty.loadLexicon("不存在的词典.txt").ok());
}

// ============================ 句切分 ============================

MM_TEST(nlp_sentence, 按中文标点切分) {
    const auto s = nlp::SentenceSplitter{}.split("今天开会。明天继续！可以吗？最后总结一下；结束");
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(5));
    MM_EXPECT_EQ(s[0].text, std::string("今天开会。"));
    MM_EXPECT_EQ(s[1].text, std::string("明天继续！"));
    MM_EXPECT_EQ(s[2].text, std::string("可以吗？"));
}

MM_TEST(nlp_sentence, 无终止标点的尾句也被保留) {
    const auto s = nlp::SentenceSplitter{}.split("第一句。第二句还没有结束");
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(2));
    MM_EXPECT_EQ(s[1].text, std::string("第二句还没有结束"));
}

MM_TEST(nlp_sentence, 吸收尾随引号与括号) {
    const auto s = nlp::SentenceSplitter{}.split("他说“可以”。下一句");
    MM_REQUIRE_TRUE(s.size() >= 2);
    MM_EXPECT_TRUE(mm::str::contains(s[0].text, "”"));
}

MM_TEST(nlp_sentence, 超长句二次切分) {
    nlp::SentenceSplitOptions opt;
    opt.maxSentenceChars = 20;
    std::string longSent;
    for (int i = 0; i < 6; ++i) longSent += "这是一个较长的子句片段";
    longSent += "。";
    const auto s = nlp::SentenceSplitter{opt}.split(longSent);
    MM_EXPECT_GT(s.size(), static_cast<size_t>(1));
    for (const nlp::Sentence& x : s) {
        MM_EXPECT_LE(static_cast<int>(mm::str::utf8Length(x.text)), opt.maxSentenceChars + 12);
    }
}

MM_TEST(nlp_sentence, 带时间戳切分与时间分配) {
    std::vector<nlp::SentenceSplitter::TimedText> segs;
    nlp::SentenceSplitter::TimedText t;
    t.text = "第一句话。第二句话。";
    t.startMs = 1000;
    t.endMs = 3000;
    t.speakerId = 2;
    segs.push_back(t);

    const auto out = nlp::SentenceSplitter{}.splitTimed(segs);
    MM_EXPECT_EQ(out.size(), static_cast<size_t>(2));
    MM_EXPECT_EQ(out[0].startMs, static_cast<int64_t>(1000));
    MM_EXPECT_EQ(out[0].speakerId, 2);
    MM_EXPECT_GE(out[1].startMs, out[0].startMs);
    MM_EXPECT_LE(out[1].endMs, static_cast<int64_t>(3000));
}

// ============================ TextRank ============================

MM_TEST(nlp_textrank, 通用图排序收敛且非负) {
    std::vector<std::tuple<int, int, double>> edges = {{0, 1, 1.0}, {1, 2, 2.0}, {0, 2, 0.5}};
    const std::vector<double> s = nlp::TextRank::rank(3, edges, {});
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(3));
    for (double v : s) MM_EXPECT_GT(v, 0.0);
    // 2 号节点连接更强 → 得分更高
    MM_EXPECT_GT(s[2], s[0]);
}

MM_TEST(nlp_textrank, 空图与孤立节点安全) {
    const std::vector<double> s = nlp::TextRank::rank(0, {}, {});
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(0));
    const std::vector<double> iso = nlp::TextRank::rank(3, {}, {});
    MM_EXPECT_EQ(iso.size(), static_cast<size_t>(3));
    // 无边的节点按 PageRank 定义收敛到 (1 - d)
    for (double v : iso) MM_EXPECT_NEAR(v, 0.15, 1e-9);
}

MM_TEST(nlp_textrank, 越界边被忽略) {
    std::vector<std::tuple<int, int, double>> edges = {{0, 99, 1.0}, {-1, 1, 1.0}, {0, 0, 5.0}};
    const std::vector<double> s = nlp::TextRank::rank(2, edges, {});
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(2));
}

MM_TEST(nlp_textrank, 词级排序识别核心词) {
    std::vector<std::vector<std::string>> sentences = {
        {"上线", "排期", "项目"},
        {"排期", "上线", "确认"},
        {"排期", "风险", "上线"},
    };
    const auto scores = nlp::TextRank::rankWords(sentences, 5);
    MM_REQUIRE_TRUE(scores.count("排期") > 0);
    MM_REQUIRE_TRUE(scores.count("上线") > 0);
    MM_REQUIRE_TRUE(scores.count("风险") > 0);
    MM_EXPECT_GT(scores.at("排期"), scores.at("风险"));
}

MM_TEST(nlp_textrank, 句子相似度符合定义) {
    const std::vector<std::string> a = {"排期", "上线", "项目"};
    const std::vector<std::string> b = {"排期", "上线", "风险"};
    const std::vector<std::string> c = {"完全", "不同", "词汇"};
    const double simAB = nlp::TextRank::sentenceSimilarity(a, b);
    const double simAC = nlp::TextRank::sentenceSimilarity(a, c);
    MM_EXPECT_GT(simAB, simAC);
    MM_EXPECT_NEAR(simAC, 0.0, 1e-12);
    MM_EXPECT_NEAR(nlp::TextRank::sentenceSimilarity(a, {}), 0.0, 1e-12);
}

MM_TEST(nlp_textrank, 归一化处理常量序列) {
    std::vector<double> flat = {5.0, 5.0, 5.0};
    nlp::TextRank::normalize(flat);
    for (double v : flat) MM_EXPECT_NEAR(v, 0.5, 1e-12);

    std::vector<double> varying = {1.0, 2.0, 3.0};
    nlp::TextRank::normalize(varying);
    MM_EXPECT_NEAR(varying.front(), 0.0, 1e-12);
    MM_EXPECT_NEAR(varying.back(), 1.0, 1e-12);
}

// ============================ 关键词 ============================

MM_TEST(nlp_keyword, 抽取会议关键词) {
    const std::string text =
        "今天我们讨论项目排期。排期需要提前确认，因为上线时间比较紧张。"
        "上线之前要完成需求评审。需求评审由产品负责，排期和上线都要对齐。"
        "风险点在于测试资源不足，排期可能延后。";
    nlp::KeywordExtractor ex(nlp::KeywordOptions{});
    const auto kws = ex.extract(text, tk());
    MM_EXPECT_GT(kws.size(), static_cast<size_t>(3));

    std::vector<std::string> words;
    for (const auto& k : kws) words.push_back(k.word);
    bool hasKey = std::count(words.begin(), words.end(), std::string("排期")) > 0 ||
                  std::count(words.begin(), words.end(), std::string("上线")) > 0;
    MM_EXPECT_TRUE(hasKey);

    // 权重应单调不增
    for (size_t i = 0; i + 1 < kws.size(); ++i) {
        MM_EXPECT_GE(kws[i].score, kws[i + 1].score - 1e-9);
    }
}

MM_TEST(nlp_keyword, topK限制与空输入) {
    nlp::KeywordOptions opt;
    opt.topK = 3;
    nlp::KeywordExtractor ex(opt);
    const std::string text = "项目 排期 上线 需求 评审 测试 资源 风险 沟通 对齐 "
                             "项目 排期 上线 需求 评审";
    const auto kws = ex.extract(text, tk());
    MM_EXPECT_LE(kws.size(), static_cast<size_t>(3));

    MM_EXPECT_EQ(ex.extract("", tk()).size(), static_cast<size_t>(0));
    MM_EXPECT_EQ(ex.extract("的的的的的", tk()).size(), static_cast<size_t>(0));
}

// ============================ 摘要 ============================

MM_TEST(nlp_summary, 抽取式摘要选出相关句) {
    const std::vector<std::string> texts = {
        "好我们今天开始周会。",
        "情绪上大家最近状态不错。",
        "本周的排期已经确认了。",
        "排期上我们决定把上线时间推迟到下个月。",
        "中午吃什么还没定。",
        "所以最终的结论是排期调整到十月上线。",
    };
    const auto sentences = makeSentences(texts);
    nlp::ExtractiveSummarizer sum(tk());
    nlp::SummaryRequest req;
    req.sentences = sentences;
    Result<nlp::SummaryDraft> draft = sum.summarize(req);
    MM_REQUIRE_TRUE(draft.ok());
    MM_EXPECT_GT(draft.value().keyPoints.size(), static_cast<size_t>(0));
    MM_EXPECT_FALSE(draft.value().overview.empty());
    MM_EXPECT_EQ(draft.value().engine, std::string("extractive"));

    // 选出的句子必须来自原文，且时间戳被保留
    for (const auto& p : draft.value().keyPoints) {
        MM_EXPECT_TRUE(std::count(texts.begin(), texts.end(), p.text) > 0);
        MM_EXPECT_GE(p.startMs, static_cast<int64_t>(0));
    }
}

MM_TEST(nlp_summary, 短文本全部保留) {
    const auto sentences = makeSentences({"第一句。", "第二句。"});
    nlp::ExtractiveSummarizer sum(tk());
    nlp::SummaryRequest req;
    req.sentences = sentences;
    Result<nlp::SummaryDraft> draft = sum.summarize(req);
    MM_REQUIRE_TRUE(draft.ok());
    MM_EXPECT_EQ(draft.value().keyPoints.size(), static_cast<size_t>(2));
}

MM_TEST(nlp_summary, 空输入返回空摘要) {
    nlp::ExtractiveSummarizer sum(tk());
    nlp::SummaryRequest req;
    Result<nlp::SummaryDraft> draft = sum.summarize(req);
    MM_REQUIRE_TRUE(draft.ok());
    MM_EXPECT_EQ(draft.value().keyPoints.size(), static_cast<size_t>(0));
    MM_EXPECT_TRUE(draft.value().overview.empty());
}

MM_TEST(nlp_summary, 目标句数受压缩比与上限约束) {
    nlp::ExtractiveOptions opt;
    opt.ratio = 0.5;
    opt.minSentences = 3;
    opt.maxSentences = 12;
    nlp::ExtractiveSummarizer sum(tk(), opt);

    MM_EXPECT_EQ(sum.targetSentenceCount(0), static_cast<size_t>(0));
    MM_EXPECT_EQ(sum.targetSentenceCount(2), static_cast<size_t>(2));
    MM_EXPECT_EQ(sum.targetSentenceCount(10), static_cast<size_t>(5));
    MM_EXPECT_EQ(sum.targetSentenceCount(100), static_cast<size_t>(12));

    nlp::ExtractiveOptions tiny;
    tiny.ratio = 0.01;
    tiny.minSentences = 1;
    tiny.maxSentences = 3;
    nlp::ExtractiveSummarizer s2(tk(), tiny);
    MM_EXPECT_EQ(s2.targetSentenceCount(100), static_cast<size_t>(1));
}

MM_TEST(nlp_summary, MMR抑制冗余) {
    // 前 6 句高度重复，后 2 句各不相同；MMR 应避免全部选中重复句
    std::vector<std::string> texts;
    for (int i = 0; i < 6; ++i) texts.push_back("排期需要确认上线时间。");
    texts.push_back("测试资源不足是主要风险。");
    texts.push_back("预算方面没有问题。");
    const auto sentences = makeSentences(texts);

    nlp::ExtractiveOptions opt;
    opt.ratio = 0.5;
    opt.minSentences = 2;
    opt.maxSentences = 12;
    opt.lambda = 0.7;
    nlp::ExtractiveSummarizer sum(tk(), opt);

    nlp::SummaryRequest req;
    req.sentences = sentences;
    Result<nlp::SummaryDraft> draft = sum.summarize(req);
    MM_REQUIRE_TRUE(draft.ok());
    MM_EXPECT_GE(draft.value().keyPoints.size(), static_cast<size_t>(2));
    // 去重后不得出现完全相同的要点
    std::vector<std::string> chosen;
    for (const auto& p : draft.value().keyPoints) chosen.push_back(p.text);
    MM_EXPECT_TRUE(std::unique(chosen.begin(), chosen.end()) == chosen.end());
}

// ============================ 待办 ============================

MM_TEST(nlp_action, 触发词识别) {
    std::string trigger;
    MM_EXPECT_TRUE(nlp::ActionItemExtractor::isActionSentence("请尽快确认排期", &trigger));
    MM_EXPECT_FALSE(trigger.empty());
    MM_EXPECT_TRUE(nlp::ActionItemExtractor::isActionSentence("这个由李四负责跟进"));
    MM_EXPECT_FALSE(nlp::ActionItemExtractor::isActionSentence("今天天气不错"));
    MM_EXPECT_FALSE(nlp::ActionItemExtractor::isActionSentence("这样可以吗？"));
}

MM_TEST(nlp_action, 责任人抽取) {
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("这个由李四负责"), std::string("李四"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("请张三跟进一下进度"),
                 std::string("张三"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("让王五来落实这件事"),
                 std::string("王五"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("@赵六 记得提交报告"),
                 std::string("赵六"));
    // 非人名不应被误抽
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("产品负责这块"), std::string(""));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("团队负责推进"), std::string(""));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractOwner("没有责任人"), std::string(""));
}

MM_TEST(nlp_action, 截止时间抽取) {
    const std::string meeting = "2026-09-14";  // 周一
    std::string raw;

    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("九月二十日前提交", meeting, &raw),
                 std::string("2026-09-20"));
    MM_EXPECT_FALSE(raw.empty());

    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("下周三之前完成", meeting, &raw),
                 std::string("2026-09-23"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("明天给我", meeting, &raw),
                 std::string("2026-09-15"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("后天回复", meeting, &raw),
                 std::string("2026-09-16"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("10月8日上线", meeting, &raw),
                 std::string("2026-10-08"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("月底前完成", meeting, &raw),
                 std::string("2026-09-30"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("3个工作日内反馈", meeting, &raw),
                 std::string("2026-09-17"));  // 周二→周四（跳过周末）
    MM_EXPECT_EQ(nlp::ActionItemExtractor::extractDueDate("这件事再说", meeting, &raw),
                 std::string(""));
}

MM_TEST(nlp_action, 优先级判定) {
    MM_EXPECT_EQ(nlp::ActionItemExtractor::detectPriority("请尽快处理"), std::string("高"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::detectPriority("必须今天完成"), std::string("高"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::detectPriority("有空可以看一下"), std::string("低"));
    MM_EXPECT_EQ(nlp::ActionItemExtractor::detectPriority("周五前提交"), std::string("中"));
}

MM_TEST(nlp_action, 完整抽取并支持说话人推断) {
    const auto sentences = makeSentences(
        {
            "这个需求由李四负责。",
            "排期我需要明天确认一下。",
            "下周三之前完成测试。",
            "今天天气不错。",
        },
        {0, 1, 0, 1});
    const std::vector<std::string> speakers = {"说话人1", "说话人2"};

    nlp::ActionItemOptions opt;
    opt.meetingDate = "2026-09-14";
    nlp::ActionItemExtractor ex(opt);
    const auto items = ex.extract(sentences, speakers);
    MM_EXPECT_GT(items.size(), static_cast<size_t>(1));

    bool foundLi = false;
    bool foundSpeakerInfer = false;
    for (const auto& it : items) {
        if (it.owner == std::string("李四")) {
            foundLi = true;
        }
        if (it.owner == std::string("说话人2")) {
            foundSpeakerInfer = true;
            MM_EXPECT_EQ(it.dueDate, std::string("2026-09-15"));
        }
    }
    MM_EXPECT_TRUE(foundLi);
    MM_EXPECT_TRUE(foundSpeakerInfer);
}

// ============================ 决议 ============================

MM_TEST(nlp_decision, 分类与抽取) {
    std::string kind;
    MM_EXPECT_TRUE(nlp::DecisionExtractor::classify("我们决定采用方案A", &kind));
    MM_EXPECT_EQ(kind, std::string("确定"));
    MM_EXPECT_TRUE(nlp::DecisionExtractor::classify("这个方案通过了", &kind));
    MM_EXPECT_EQ(kind, std::string("通过"));
    MM_EXPECT_TRUE(nlp::DecisionExtractor::classify("我不同意这个做法", &kind));
    MM_EXPECT_EQ(kind, std::string("否决"));
    MM_EXPECT_FALSE(nlp::DecisionExtractor::classify("今天天气不错"));

    const auto sentences = makeSentences({
        "我们决定采用方案A。",
        "这个需求先不做。",
        "方案B通过了评审。",
        "预算方面大家达成一致。",
        "你能确定吗？",
    });
    const auto decisions = nlp::DecisionExtractor{}.extract(sentences);
    MM_EXPECT_EQ(decisions.size(), static_cast<size_t>(3));
    for (const auto& d : decisions) {
        MM_EXPECT_FALSE(d.kind.empty());
        MM_EXPECT_GE(d.startMs, static_cast<int64_t>(0));
    }
}

MM_TEST(nlp_decision, 否定句不被误判为同意) {
    const auto sentences = makeSentences({"我不同意在这个时间点上线。"});
    const auto decisions = nlp::DecisionExtractor{}.extract(sentences);
    MM_REQUIRE_TRUE(decisions.size() == 1);
    MM_EXPECT_EQ(decisions[0].kind, std::string("否决"));
}

// ============================ 话题分段 ============================

MM_TEST(nlp_topic, 分段结果结构合法) {
    std::vector<std::string> texts;
    for (int i = 0; i < 5; ++i) texts.push_back("排期和上线时间是本段重点。");
    for (int i = 0; i < 5; ++i) texts.push_back("预算和成本控制是另一个议题。");
    for (int i = 0; i < 5; ++i) texts.push_back("测试资源和质量保障是第三个议题。");
    const auto sentences = makeSentences(texts);

    nlp::TopicSegmenter seg(tk());
    const auto topics = seg.segment(sentences);
    MM_EXPECT_GT(topics.size(), static_cast<size_t>(0));
    for (const auto& t : topics) {
        MM_EXPECT_GT(t.sentenceCount(), 0);
        MM_EXPECT_FALSE(t.title.empty());
        MM_EXPECT_GE(t.startSentence, 0);
        MM_EXPECT_LE(t.endSentence, static_cast<int>(sentences.size()));
    }
    // 分段应覆盖全部句子
    MM_EXPECT_EQ(topics.front().startSentence, 0);
    MM_EXPECT_EQ(topics.back().endSentence, static_cast<int>(sentences.size()));
}

MM_TEST(nlp_topic, 短文本退化为单议题) {
    const auto sentences = makeSentences({"只有一个句子。"});
    const auto topics = nlp::TopicSegmenter{tk()}.segment(sentences);
    MM_EXPECT_GE(topics.size(), static_cast<size_t>(1));
    MM_EXPECT_EQ(topics.front().startSentence, 0);
}

MM_TEST(nlp_topic, 块相似度序列计算) {
    std::vector<std::vector<std::string>> tokens;
    for (int i = 0; i < 4; ++i) tokens.push_back({"排期", "上线"});
    for (int i = 0; i < 4; ++i) tokens.push_back({"预算", "成本"});
    const auto sims = nlp::TopicSegmenter{tk()}.blockSimilarities(tokens);
    MM_EXPECT_GT(sims.size(), static_cast<size_t>(1));
    for (double s : sims) {
        MM_EXPECT_GE(s, 0.0);
        MM_EXPECT_LE(s, 1.0 + 1e-9);
    }
}

// ============================ 纪要 ============================

MM_TEST(nlp_minutes, 端到端纪要结构完整) {
    const auto sentences = makeSentences(
        {
            "今天我们开周会，先过一下本周的排期。",
            "本周排期整体是可控的，主要风险在测试资源。",
            "这个由李四负责跟进测试资源。",
            "我们决定把上线时间调整到十月八日。",
            "请张三在九月二十日前提交测试方案。",
            "下周我们再同步一次进展。",
        },
        {0, 0, 1, 0, 1, 0});
    const std::vector<std::string> speakers = {"说话人1", "说话人2"};

    nlp::Statistics stats;
    stats.totalDurationMs = 60000;
    stats.speechDurationMs = 45000;
    stats.speechRatio = 0.75;
    stats.segmentCount = 6;
    stats.speakerCount = 2;
    stats.speakerTalkMs = {30000, 15000};
    stats.speakerCharCount = {80, 40};

    nlp::MinutesOptions opt;
    opt.meetingDate = "2026-09-14";
    nlp::MinutesBuilder builder(tk(), opt);
    Result<nlp::Minutes> minutes = builder.build(sentences, speakers, stats, nullptr);
    MM_REQUIRE_TRUE(minutes.ok());

    const nlp::Minutes& m = minutes.value();
    MM_EXPECT_FALSE(m.title.empty());
    MM_EXPECT_EQ(m.meetingDate, std::string("2026-09-14"));
    MM_EXPECT_FALSE(m.overview.empty());
    MM_EXPECT_GT(m.keyPoints.size(), static_cast<size_t>(0));
    MM_EXPECT_GT(m.keywords.size(), static_cast<size_t>(0));
    MM_EXPECT_GT(m.topics.size(), static_cast<size_t>(0));
    MM_EXPECT_GT(m.decisions.size(), static_cast<size_t>(0));
    MM_EXPECT_GT(m.actionItems.size(), static_cast<size_t>(0));
    MM_EXPECT_EQ(m.summaryEngine, std::string("extractive"));
    MM_EXPECT_EQ(m.stats.speakerCount, 2);

    const Json j = m.toJson();
    MM_EXPECT_EQ(j.getString("schemaVersion"), std::string("1.0"));
    MM_EXPECT_GT(j.get("keyPoints").size(), static_cast<size_t>(0));
    MM_EXPECT_FALSE(j.dump(2).empty());
}

MM_TEST(nlp_minutes, 空输入给出风险提示) {
    nlp::Statistics stats;
    nlp::MinutesBuilder builder(tk());
    Result<nlp::Minutes> m = builder.build({}, {}, stats, nullptr);
    MM_REQUIRE_TRUE(m.ok());
    MM_EXPECT_FALSE(m.value().risks.empty());
    MM_EXPECT_EQ(m.value().summaryEngine.empty(), true);
}

MM_TEST(nlp_minutes, 标题推导) {
    MM_EXPECT_EQ(nlp::MinutesBuilder::deriveTitle("产品评审会今天开始。后续讨论排期。", "兜底"),
                 std::string("产品评审会今天开始"));
    MM_EXPECT_EQ(nlp::MinutesBuilder::deriveTitle("", "兜底标题"), std::string("兜底标题"));
    MM_EXPECT_EQ(nlp::MinutesBuilder::deriveTitle("好。", "兜底标题"), std::string("兜底标题"));
}

MM_TEST(nlp_minutes, 无决议与无责任人生成风险提示) {
    const auto sentences = makeSentences({
        "我们讨论了很久但没有结论。",
        "待办是要继续跟进这件事。",
    });
    nlp::Statistics stats;
    stats.speakerCount = 1;
    nlp::MinutesBuilder builder(tk());
    Result<nlp::Minutes> m = builder.build(sentences, {"发言人"}, stats, nullptr);
    MM_REQUIRE_TRUE(m.ok());
    bool hasNoDecision = false;
    bool hasNoOwner = false;
    for (const std::string& r : m.value().risks) {
        if (r.find("决议") != std::string::npos) hasNoDecision = true;
        if (r.find("责任人") != std::string::npos) hasNoOwner = true;
    }
    MM_EXPECT_TRUE(hasNoDecision);
    MM_EXPECT_TRUE(hasNoOwner);
}

MM_TEST(nlp_minutes, 自定义摘要后端可插拔) {
    const auto sentences = makeSentences(
        {"排期已经确认了。", "上线时间调整到十月八日。", "测试资源需要补充。"});
    nlp::Statistics stats;
    stats.speakerCount = 1;

    // 1) 后端可用 → 使用后端产出
    StubSummarizer stub(true, /*remote=*/false);
    nlp::MinutesBuilder builder(tk());
    Result<nlp::Minutes> m1 = builder.build(sentences, {"发言人"}, stats, &stub);
    MM_REQUIRE_TRUE(m1.ok());
    MM_EXPECT_EQ(m1.value().summaryEngine, std::string("stub"));
    MM_EXPECT_CONTAINS(m1.value().overview, "【自定义后端】");
    MM_EXPECT_GT(m1.value().keyPoints.size(), static_cast<size_t>(0));

    // 2) 后端不可用 → 自动回退抽取式，不留空
    StubSummarizer dead(false, /*remote=*/false);
    Result<nlp::Minutes> m2 = builder.build(sentences, {"发言人"}, stats, &dead);
    MM_REQUIRE_TRUE(m2.ok());
    MM_EXPECT_EQ(m2.value().summaryEngine, std::string("extractive"));
    MM_EXPECT_FALSE(m2.value().overview.empty());
    MM_EXPECT_GT(m2.value().keyPoints.size(), static_cast<size_t>(0));

    // 3) 空指针 → 使用内置抽取式
    Result<nlp::Minutes> m3 = builder.build(sentences, {"发言人"}, stats, nullptr);
    MM_REQUIRE_TRUE(m3.ok());
    MM_EXPECT_EQ(m3.value().summaryEngine, std::string("extractive"));
}

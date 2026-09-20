// NLP 词典与文本规范化测试（从 test_02_nlp.cpp 拆出，避免单翻译单元过大）
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "mm/common/string_utils.h"
#include "mm/nlp/t2s.h"
#include "mm/nlp/sentence_splitter.h"
#include "mm/nlp/tokenizer.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

namespace {

/// 共享分词器（惰性加载内置词典，只加载一次）
const nlp::Tokenizer& tk2() {
    static nlp::Tokenizer tokenizer;
    static bool loaded = false;
    if (!loaded) {
        MM_EXPECT_TRUE(tokenizer.loadDefaultLexicon().ok());
        loaded = true;
    }
    return tokenizer;
}

}  // namespace

MM_TEST(nlp_sentence, 按权重切分文本) {
    const std::string text = "第一句话已经说完了。第二句话也比较长一些。第三句结束。";

    // 权重数量为 1 → 原样返回
    const auto one = nlp::SentenceSplitter::splitByWeights(text, {1000});
    MM_REQUIRE_TRUE(one.size() == 1);
    MM_EXPECT_EQ(one[0], text);

    // 数量匹配 → 不丢内容
    const auto three = nlp::SentenceSplitter::splitByWeights(text, {1000, 1000, 1000});
    MM_REQUIRE_TRUE(three.size() == 3);
    std::string joined;
    for (const std::string& p : three) joined += mm::str::utf8Length(p) > 0 ? p : "";
    MM_EXPECT_EQ(mm::str::utf8Length(joined) >= mm::str::utf8Length(text) - 6, true);
    for (const std::string& p : three) MM_EXPECT_FALSE(p.empty());

    // 权重悬殊 → 先分配的段拿到更多句子
    const auto skewed = nlp::SentenceSplitter::splitByWeights(text, {10000, 500});
    MM_REQUIRE_TRUE(skewed.size() == 2);
    MM_EXPECT_FALSE(skewed[0].empty());
    MM_EXPECT_FALSE(skewed[1].empty());

    // 空输入与空权重
    MM_EXPECT_EQ(nlp::SentenceSplitter::splitByWeights("", {1, 1}).size(), static_cast<size_t>(2));
    MM_EXPECT_EQ(nlp::SentenceSplitter::splitByWeights(text, {}).size(), static_cast<size_t>(0));

    // 单句文本 + 多权重 → 按权重切字符，**不允许出现空段**
    // （真实录音回归：中文 ASR 常整段无句号，早期实现会把全部文本堆在首段，
    //   导致一个跨说话人的转写单元里，文本全被算给第一位说话人）
    const auto single = nlp::SentenceSplitter::splitByWeights("只有一句话没有标点", {1, 1});
    MM_REQUIRE_TRUE(single.size() == 2);
    MM_EXPECT_FALSE(single[0].empty());
    MM_EXPECT_FALSE(single[1].empty());
    MM_EXPECT_EQ(mm::str::utf8Length(single[0]) + mm::str::utf8Length(single[1]),
                 mm::str::utf8Length("只有一句话没有标点"));
}

MM_TEST(nlp_sentence, 权重切分不丢字且无空段) {
    const std::string text = "甲方的包装设计需要重新评估成本控制方案也要同步调整否则明年的预算会超支";
    const std::vector<int64_t> weights = {1500, 3200, 800, 2400, 900, 1800};

    const auto parts = nlp::SentenceSplitter::splitByWeights(text, weights);
    MM_REQUIRE_TRUE(parts.size() == weights.size());

    // 1) 无空段
    for (const std::string& p : parts) MM_EXPECT_FALSE(p.empty());

    // 2) 字符总数守恒（不丢字、不重复）
    int sum = 0;
    for (const std::string& p : parts) sum += static_cast<int>(mm::str::utf8Length(p));
    MM_EXPECT_EQ(sum, static_cast<int>(mm::str::utf8Length(text)));

    // 3) 权重大的段拿到更多字符
    const size_t len1 = mm::str::utf8Length(parts[1]);  // weight 3200，最大
    const size_t len2 = mm::str::utf8Length(parts[2]);  // weight 800，最小
    MM_EXPECT_GE(len1, len2);
}

MM_TEST(nlp_sentence, 权重切分优先落在标点上) {
    const std::string text = "第一点已经说完。第二点还需要确认，第三点暂时搁置。";
    const auto parts = nlp::SentenceSplitter::splitByWeights(text, {1200, 1200});
    MM_REQUIRE_TRUE(parts.size() == 2);
    // 切点应落在句末标点之后，而不是把句子拦腰截断
    MM_EXPECT_TRUE(mm::str::endsWith(parts[0], "。") ||
                   mm::str::endsWith(parts[0], "，"));
    MM_EXPECT_FALSE(parts[1].empty());
}

MM_TEST(nlp_sentence, 字符数少于段数时前若干段各得一字符) {
    const auto parts = nlp::SentenceSplitter::splitByWeights("甲乙", {1, 1, 1, 1});
    MM_REQUIRE_TRUE(parts.size() == 4);
    MM_EXPECT_EQ(parts[0], std::string("甲"));
    MM_EXPECT_EQ(parts[1], std::string("乙"));
    MM_EXPECT_EQ(parts[2], std::string(""));
    MM_EXPECT_EQ(parts[3], std::string(""));
}

// ============================ 繁→简转换 ============================

MM_TEST(nlp_t2s, 映射表加载与转换) {
    nlp::TraditionalConverter conv;
    Result<void> loaded = conv.loadDefault(mmtest::dataPath("t2s_zh.txt"));
    MM_REQUIRE_TRUE(loaded.ok());
    MM_EXPECT_GT(conv.mappingSize(), static_cast<size_t>(1000));

    // whisper 实测输出：整句繁体
    const std::string traditional =
        "首先過一下排期，本周的排期整體是可控的。主要的風險在於測試資源不足。";
    MM_EXPECT_TRUE(conv.needsConversion(traditional));
    const std::string simplified = conv.convert(traditional);
    MM_EXPECT_EQ(simplified,
                 std::string("首先过一下排期，本周的排期整体是可控的。主要的风险在于测试资源不足。"));
    MM_EXPECT_FALSE(conv.needsConversion(simplified));

    // 幂等：对简体文本再转一次不变
    MM_EXPECT_EQ(conv.convert(simplified), simplified);
    // 未映射字符原样保留
    MM_EXPECT_EQ(conv.convert("abc123，。"), std::string("abc123，。"));
    MM_EXPECT_EQ(conv.convert(""), std::string(""));
}

MM_TEST(nlp_t2s, 非法映射表被拒绝) {
    nlp::TraditionalConverter conv;
    MM_EXPECT_FALSE(conv.loadMapping("").ok());
    MM_EXPECT_FALSE(conv.loadMapping(mmtest::outPath("t2s/missing.txt")).ok());

    const std::string empty = mmtest::outPath("t2s/empty.txt");
    {
        std::ofstream out(empty, std::ios::binary);
        out << "# 只有注释\n";
    }
    MM_EXPECT_FALSE(conv.loadMapping(empty).ok());
    MM_EXPECT_FALSE(conv.loaded());
}

MM_TEST(nlp_tokenizer, 领域补充词典生效) {
    // 「排期」在通用词典中缺失，须由 data/lexicon_extra.txt 补齐
    const std::vector<std::string> words = tk2().cut("首先过一下排期，本周的排期整体可控");
    MM_EXPECT_GT(std::count(words.begin(), words.end(), std::string("排期")), 0);
    // 补充词典不应破坏原有切分：分词会跳过标点，因此拼接结果应等于「去掉标点的原文」
    std::string expected;
    for (char32_t c : mm::str::toUtf32("首先过一下排期，本周的排期整体可控")) {
        if (mm::str::isCjk(c) || mm::str::isAsciiAlnum(c)) {
            expected += mm::str::toUtf8(std::u32string(1, c));
        }
    }
    MM_EXPECT_EQ(mm::str::join(words, ""), expected);

    const std::vector<std::string> w2 = tk2().cut("需求评审和技术方案都要对齐");
    bool hasReview = std::count(w2.begin(), w2.end(), std::string("需求评审")) > 0;
    MM_EXPECT_TRUE(hasReview);
}

// 基础层测试：JSON / 字符串 / 时间 / 配置
#include <filesystem>

#include "mm/common/config.h"
#include "mm/common/json.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

MM_TEST(common_json, 对象读写与顺序保持) {
    Json j = Json::object();
    j.set("name", std::string("周会"));
    j.set("count", 3);
    j.set("ratio", 0.25);
    j.set("enabled", true);

    MM_EXPECT_TRUE(j.isObject());
    MM_EXPECT_EQ(j.size(), static_cast<size_t>(4));
    MM_EXPECT_EQ(j.keyAt(0), std::string("name"));
    MM_EXPECT_EQ(j.keyAt(3), std::string("enabled"));
    MM_EXPECT_EQ(j.getString("name"), std::string("周会"));
    MM_EXPECT_EQ(j.getInt("count"), 3);
    MM_EXPECT_NEAR(j.getDouble("ratio"), 0.25, 1e-12);
    MM_EXPECT_TRUE(j.getBool("enabled"));
    MM_EXPECT_EQ(j.getString("missing", "默认值"), std::string("默认值"));
}

MM_TEST(common_json, 数组操作) {
    Json arr = Json::array();
    arr.push(Json(1));
    arr.push(Json("文本"));
    arr.push(Json(2.5));
    MM_EXPECT_EQ(arr.size(), static_cast<size_t>(3));
    MM_EXPECT_EQ(arr.at(0).asInt(), 1);
    MM_EXPECT_EQ(arr.at(1).asString(), std::string("文本"));
    MM_EXPECT_NEAR(arr.at(2).asDouble(), 2.5, 1e-12);
    MM_EXPECT_TRUE(arr.at(99).isNull());
}

MM_TEST(common_json, 序列化与反序列化往返) {
    Json root = Json::object();
    root.set("title", std::string("产品评审会"));
    Json segs = Json::array();
    Json s1 = Json::object();
    s1.set("startMs", 1000);
    s1.set("text", std::string("我们讨论一下排期。"));
    s1.set("confidence", 0.93);
    segs.push(s1);
    Json s2 = Json::object();
    s2.set("startMs", 5000);
    s2.set("text", std::string("带\"引号\"与\n换行"));
    segs.push(s2);
    root.set("segments", segs);

    const std::string dump = root.dump(2);
    MM_EXPECT_CONTAINS(dump, "\"title\"");
    MM_EXPECT_CONTAINS(dump, "\\\"引号\\\"");

    Result<Json> parsed = Json::parse(dump);
    MM_REQUIRE_TRUE(parsed.ok());
    MM_EXPECT_EQ(parsed.value().getString("title"), std::string("产品评审会"));
    const Json& back = parsed.value().get("segments");
    MM_REQUIRE_TRUE(back.isArray());
    MM_EXPECT_EQ(back.size(), static_cast<size_t>(2));
    MM_EXPECT_EQ(back.at(0).getString("text"), std::string("我们讨论一下排期。"));
    MM_EXPECT_EQ(back.at(1).getString("text"), std::string("带\"引号\"与\n换行"));
}

MM_TEST(common_json, 非法输入返回错误而非崩溃) {
    const char* bad[] = {
        "", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "[1,2", "{\"a\":1,}",
    };
    for (const char* text : bad) {
        Result<Json> r = Json::parse(text);
        MM_EXPECT_FALSE(r.ok());
        MM_EXPECT_FALSE(r.message().empty());
    }
}

MM_TEST(common_json, 深层嵌套被限制) {
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    Result<Json> r = Json::parse(deep);
    MM_EXPECT_FALSE(r.ok());
}

MM_TEST(common_json, Unicode转义解析) {
    Result<Json> r = Json::parse("\"\\u4e2d\\u6587\\u6d4b\\u8bd5\"");
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().asString(), std::string("中文测试"));

    Result<Json> r2 = Json::parse("\"tab\\tnewline\\n\"");
    MM_REQUIRE_TRUE(r2.ok());
    MM_EXPECT_EQ(r2.value().asString(), std::string("tab\tnewline\n"));
}

MM_TEST(common_json, 原子写入与读回) {
    const std::string path = mmtest::outPath("common/atomic.json");
    Json j = Json::object();
    j.set("value", 42);
    Result<void> w = j.writeFile(path);
    MM_REQUIRE_TRUE(w.ok());
    MM_EXPECT_TRUE(std::filesystem::exists(path));
    MM_EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

    Result<Json> back = Json::parseFile(path);
    MM_REQUIRE_TRUE(back.ok());
    MM_EXPECT_EQ(back.value().getInt("value"), 42);
}

MM_TEST(common_string, 基础文本工具) {
    MM_EXPECT_EQ(str::trim("  hello \t\n"), std::string("hello"));
    MM_EXPECT_EQ(str::toLowerAscii("AbC"), std::string("abc"));
    MM_EXPECT_TRUE(str::startsWith("meeting.txt", "meet"));
    MM_EXPECT_TRUE(str::endsWith("meeting.txt", ".txt"));
    MM_EXPECT_EQ(str::replaceAll("a-b-c", "-", "_"), std::string("a_b_c"));

    const auto parts = str::split("a,b,,c", ',');
    MM_EXPECT_EQ(parts.size(), static_cast<size_t>(4));
    MM_EXPECT_EQ(parts[2], std::string(""));
    MM_EXPECT_EQ(str::join({"x", "y", "z"}, "+"), std::string("x+y+z"));

    const auto words = str::splitAny("已 完成,下一步;排期", " ,;");
    MM_EXPECT_EQ(words.size(), static_cast<size_t>(4));
    MM_EXPECT_EQ(words[0], std::string("已"));
}

MM_TEST(common_string, UTF8码点操作) {
    const std::string s = "会议纪要abc";
    MM_EXPECT_EQ(str::utf8Length(s), static_cast<size_t>(7));  // 4 个汉字 + 3 个字母
    MM_EXPECT_EQ(s.size(), static_cast<size_t>(4 * 3 + 3));
    MM_EXPECT_EQ(str::utf8Substr(s, 0, 2), std::string("会议"));
    MM_EXPECT_EQ(str::utf8Substr(s, 4, 2), std::string("ab"));
    MM_EXPECT_EQ(str::utf8Substr(s, 99, 1), std::string(""));

    // 非法字节应替换为 U+FFFD 而非崩溃
    std::string bad;
    bad.push_back(static_cast<char>(0xFF));
    bad += "ok";
    MM_EXPECT_TRUE(str::utf8Length(bad) >= 2);
}

MM_TEST(common_string, 字符分类) {
    MM_EXPECT_TRUE(str::isCjk(U'会'));
    MM_EXPECT_TRUE(str::isCjk(U'议'));
    MM_EXPECT_FALSE(str::isCjk(U'a'));
    MM_EXPECT_TRUE(str::isCjkPunct(U'。'));
    MM_EXPECT_TRUE(str::isCjkPunct(U'，'));
    MM_EXPECT_FALSE(str::isCjkPunct(U'a'));
    MM_EXPECT_TRUE(str::isSentenceEnd(U'？'));
    MM_EXPECT_TRUE(str::isSentenceEnd(U'.'));
    MM_EXPECT_FALSE(str::isSentenceEnd(U'，'));
}

MM_TEST(common_string, 全角转半角) {
    MM_EXPECT_EQ(str::fullWidthToHalfWidth("ＡＢＣ１２３"), std::string("ABC123"));
    MM_EXPECT_EQ(str::fullWidthToHalfWidth("（测试）"), std::string("(测试)"));
    MM_EXPECT_EQ(str::fullWidthToHalfWidth("中文保持不变"), std::string("中文保持不变"));
}

MM_TEST(common_string, 文件名净化) {
    MM_EXPECT_EQ(str::sanitizeFileName("周会/纪要:2026"), std::string("周会_纪要_2026"));
    MM_EXPECT_EQ(str::sanitizeFileName("..\\..\\etc\\passwd"), std::string("_.._etc_passwd"));
    MM_EXPECT_EQ(str::sanitizeFileName("../../x"), std::string("_.._x"));
    MM_EXPECT_EQ(str::sanitizeFileName("CON"), std::string("untitled_CON"));
    MM_EXPECT_EQ(str::sanitizeFileName("   "), std::string("untitled"));
    MM_EXPECT_EQ(str::sanitizeFileName("正常名称"), std::string("正常名称"));
    MM_EXPECT_EQ(str::sanitizeFileName("trailing..."), std::string("trailing"));
}

MM_TEST(common_string, 数字格式化) {
    MM_EXPECT_EQ(str::numberToString(3.0), std::string("3"));
    MM_EXPECT_EQ(str::numberToString(3.1400, 4), std::string("3.14"));
    MM_EXPECT_EQ(str::numberToString(0.5, 2), std::string("0.5"));
    MM_EXPECT_EQ(str::numberToString(-0.0, 2), std::string("0"));
    MM_EXPECT_EQ(str::numberToString(1.0 / 0.0), std::string("0"));
}

MM_TEST(common_time, 时长格式化) {
    MM_EXPECT_EQ(timeutil::formatDuration(0), std::string("00:00:00"));
    MM_EXPECT_EQ(timeutil::formatDuration(1000), std::string("00:00:01"));
    MM_EXPECT_EQ(timeutil::formatDuration(61000), std::string("00:01:01"));
    MM_EXPECT_EQ(timeutil::formatDuration(3661000), std::string("01:01:01"));
    MM_EXPECT_EQ(timeutil::formatSrt(3661500), std::string("01:01:01,500"));
    MM_EXPECT_EQ(timeutil::formatShort(90000), std::string("01:30"));
    MM_EXPECT_EQ(timeutil::formatClock(1500), std::string("00:00:01.500"));
}

MM_TEST(common_time, 日期解析与推算) {
    const auto d = timeutil::parseYmd("2026-09-14");
    MM_REQUIRE_TRUE(d.valid);
    MM_EXPECT_EQ(d.year, 2026);
    MM_EXPECT_EQ(d.month, 9);
    MM_EXPECT_EQ(d.day, 14);
    MM_EXPECT_EQ(timeutil::formatYmd(d), std::string("2026-09-14"));

    const auto next = timeutil::addDays(d, 7);
    MM_EXPECT_EQ(timeutil::formatYmd(next), std::string("2026-09-21"));
    MM_EXPECT_EQ(timeutil::daysBetween(d, next), 7);

    // 跨月/跨年
    MM_EXPECT_EQ(timeutil::formatYmd(timeutil::addDays(timeutil::parseYmd("2026-01-31"), 1)),
                 std::string("2026-02-01"));
    MM_EXPECT_EQ(timeutil::formatYmd(timeutil::addDays(timeutil::parseYmd("2026-12-31"), 1)),
                 std::string("2027-01-01"));
    // 闰年
    MM_EXPECT_EQ(timeutil::formatYmd(timeutil::addDays(timeutil::parseYmd("2024-02-28"), 1)),
                 std::string("2024-02-29"));
    MM_EXPECT_EQ(timeutil::formatYmd(timeutil::addDays(timeutil::parseYmd("2025-02-28"), 1)),
                 std::string("2025-03-01"));

    MM_EXPECT_FALSE(timeutil::parseYmd("2026-13-01").valid);
    MM_EXPECT_FALSE(timeutil::parseYmd("").valid);
    MM_EXPECT_FALSE(timeutil::parseYmd("abc").valid);
}

MM_TEST(common_time, 星期计算) {
    // 2026-09-14 为星期一
    MM_EXPECT_EQ(timeutil::weekdayOf(timeutil::parseYmd("2026-09-14")), 1);
    MM_EXPECT_EQ(timeutil::weekdayOf(timeutil::parseYmd("2026-09-20")), 7);
    MM_EXPECT_EQ(timeutil::weekdayNameCn(1), std::string("星期一"));
    MM_EXPECT_EQ(timeutil::weekdayNameCn(7), std::string("星期日"));
}

MM_TEST(common_config, 默认值与序列化往返) {
    Config c;
    c.language = "zh";
    c.minSpeechMs = 250;
    c.speakerMode = SpeakerMode::Fixed;
    c.speakerCount = 3;
    c.summaryRatio = 0.3;

    const Json j = c.toJson();
    const Config back = Config::fromJson(j);
    MM_EXPECT_EQ(back.language, std::string("zh"));
    MM_EXPECT_EQ(back.minSpeechMs, 250);
    MM_EXPECT_TRUE(back.speakerMode == SpeakerMode::Fixed);
    MM_EXPECT_EQ(back.speakerCount, 3);
    MM_EXPECT_NEAR(back.summaryRatio, 0.3, 1e-9);
    MM_EXPECT_TRUE(back.exportMarkdown);
}

MM_TEST(common_config, 后端枚举解析) {
    MM_EXPECT_TRUE(asrBackendFromString("whisper") == AsrBackend::Whisper);
    MM_EXPECT_TRUE(asrBackendFromString("WHISPER.CPP") == AsrBackend::Whisper);
    MM_EXPECT_TRUE(asrBackendFromString("replay") == AsrBackend::Replay);
    MM_EXPECT_TRUE(asrBackendFromString("null") == AsrBackend::Null);
    MM_EXPECT_TRUE(asrBackendFromString("未知", AsrBackend::Replay) == AsrBackend::Replay);
    MM_EXPECT_EQ(std::string(toString(AsrBackend::Whisper)), std::string("whisper"));
}

MM_TEST(common_config, 线程数与人数生效值) {
    Config c;
    c.threads = 0;
    MM_EXPECT_GE(c.effectiveThreads(), 2);
    c.threads = 6;
    MM_EXPECT_EQ(c.effectiveThreads(), 6);

    c.speakerMode = SpeakerMode::Auto;
    MM_EXPECT_EQ(c.effectiveSpeakerCount(), 0);
    c.speakerMode = SpeakerMode::Fixed;
    c.speakerCount = 100;
    MM_EXPECT_EQ(c.effectiveSpeakerCount(), 8);
    c.speakerCount = 0;
    MM_EXPECT_EQ(c.effectiveSpeakerCount(), 1);
}

MM_TEST(common_config, 校验给出修正提示) {
    Config c;
    c.summaryRatio = 3.0;
    c.diarMergeThreshold = 5.0;
    const auto notes = c.validate();
    MM_EXPECT_GE(notes.size(), static_cast<size_t>(2));

    Config ok;
    MM_EXPECT_EQ(ok.validate().size(), static_cast<size_t>(0));
}

MM_TEST(common_config, 离线标志读取) {
    // 未设置或设为 0 时不应判定为强制离线
    MM_EXPECT_FALSE(Config::offlineForced());
}

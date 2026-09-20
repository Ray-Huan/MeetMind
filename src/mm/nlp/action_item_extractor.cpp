#include "mm/nlp/action_item_extractor.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "mm/nlp/text_normalizer.h"

namespace mm::nlp {
namespace {

const std::vector<std::string>& actionTriggers() {
    // 只保留「施动/祈使」语义的触发词。
    // 刻意排除「排期」「上线」「评审」「方案」等名词：它们在会议里几乎每句都出现，
    // 作为触发词会把大量陈述句误判为待办（实测误报率极高）。
    static const std::vector<std::string> kTriggers = {
        "待办", "行动项", "行动计划", "跟进", "负责", "责任人", "务必", "请", "需要", "尽快",
        "落实", "安排", "提交", "输出", "整理", "补充", "对接", "推动",
        "修复", "优化", "准备", "调研", "确认", "明确", "制定", "完成", "推进",
        "同步", "核销", "锁定", "敲定", "抓一下", "盯一下", "排一下", "拉个",
    };
    return kTriggers;
}

/// 非人名黑名单：这些词出现在「X负责」的 X 位置时不视为人名。
const std::unordered_set<std::string>& nonNameWords() {
    static const std::unordered_set<std::string> kWords = {
        // 代词/集体
        "我们", "你们", "他们", "大家", "各位", "团队", "小组", "部门", "公司", "这个", "那个",
        "本次", "这次", "下次", "后续", "相关", "有关", "主要", "重点", "整体", "统一", "专人",
        "自己", "本人", "对方", "双方", "甲方", "乙方", "产品", "研发", "测试", "运营", "设计",
        "前端", "后端", "算法", "项目", "会议", "议题", "方案", "需求", "问题", "工作", "任务",
        "什么", "谁", "哪个", "由谁", "需要", "必须", "可以", "应该", "是否", "怎么",
        // 时间/方位（常出现在「X负责」位置但并非人名）
        "日前", "之前", "之后", "以后", "以前", "现在", "目前", "当时", "会上", "会后",
        "当天", "到时", "届时", "近期", "近期内", "下周", "本周", "这周", "上周", "月底",
        "季度末", "周内", "年内", "明天", "后天", "今天", "今日", "明日", "下个月", "这个月",
        // 动词/虚词残留
        "尽快", "具体", "抓紧", "统一来", "先把", "先由", "然后", "接着", "直接", "马上",
    };
    return kWords;
}

/// 人名边界字符：出现在候选字符串中即截断（说明进入下一个语义单元）。
bool isNameBoundary(char32_t c) {
    switch (c) {
        case U'在': case U'于': case U'的': case U'要': case U'需': case U'请': case U'把':
        case U'给': case U'和': case U'与': case U'并': case U'且': case U'就': case U'能':
        case U'会': case U'负': case U'跟': case U'落': case U'推': case U'主': case U'牵':
        case U'对': case U'完': case U'准': case U'提': case U'整': case U'排': case U'确':
        case U'同': case U'告': case U'做': case U'去': case U'来': case U'再': case U'先':
        case U'尽': case U'下': case U'上': case U'本': case U'该': case U'等': case U'或':
        case U'是': case U'有': case U'没': case U'不': case U'很': case U'这': case U'那':
        // 施动前缀作为姓名左边界（「请」已在上方列出）
        case U'由': case U'让': case U'叫':
            return true;
        default:
            return false;
    }
}

/// 中文姓名绝不会以这些功能字/副词开头——用于剔除「继续跟进」「尽快落实」等动词短语。
bool isNonNameStart(char32_t c) {
    switch (c) {
        case U'继': case U'尽': case U'抓': case U'统': case U'及': case U'优': case U'认':
        case U'积': case U'共': case U'按': case U'随': case U'直': case U'赶': case U'先':
        case U'再': case U'又': case U'也': case U'还': case U'就': case U'才': case U'只':
        case U'更': case U'最': case U'很': case U'太': case U'不': case U'没': case U'别':
        case U'务': case U'必': case U'应': case U'该': case U'能': case U'会': case U'想':
        case U'要': case U'需': case U'请': case U'把': case U'给': case U'和': case U'与':
        case U'并': case U'且': case U'从': case U'向': case U'对': case U'为': case U'以':
        case U'于': case U'在': case U'的': case U'地': case U'得': case U'了': case U'着':
        case U'过': case U'自': case U'各': case U'全': case U'多': case U'少': case U'好':
        case U'落': case U'推': case U'翻': case U'排': case U'完': case U'提': case U'整':
        case U'确': case U'做': case U'去': case U'来': case U'下': case U'上': case U'本':
        case U'等': case U'或': case U'是': case U'有': case U'同': case U'针': case U'关':
        case U'由': case U'被': case U'替': case U'跟':
            return true;
        default:
            return false;
    }
}

struct CpIndex {
    std::u32string u;
    std::vector<size_t> byteStart;  ///< 每个码点的起始字节；末尾附加总字节数
};

CpIndex buildIndex(const std::string& text) {
    CpIndex idx;
    idx.u = str::toUtf32(text);
    idx.byteStart.reserve(idx.u.size() + 1);
    size_t pos = 0;
    for (char32_t c : idx.u) {
        idx.byteStart.push_back(pos);
        pos += str::toUtf8(std::u32string(1, c)).size();
    }
    idx.byteStart.push_back(text.size());
    return idx;
}

std::string cpRange(const CpIndex& idx, size_t from, size_t to) {
    if (from >= to || from >= idx.u.size()) return {};
    to = std::min(to, idx.u.size());
    return str::toUtf8(idx.u.substr(from, to - from));
}

bool isCjkAt(const std::u32string& u, size_t i) { return i < u.size() && str::isCjk(u[i]); }

bool containsAny(const std::string& text, const std::vector<std::string>& needles,
                 std::string* hit = nullptr) {
    for (const std::string& n : needles) {
        if (str::contains(text, n)) {
            if (hit) *hit = n;
            return true;
        }
    }
    return false;
}

std::string trimDiscourse(const std::string& s) {
    static const std::vector<std::string> kPrefixes = {
        "那个", "然后", "其实", "我觉得", "我认为", "那么", "所以", "另外", "另外呢",
        "嗯", "呃", "就是", "就是说", "我们",
    };
    std::string out = str::trim(s);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string& p : kPrefixes) {
            if (str::startsWith(out, p) && str::utf8Length(out) > str::utf8Length(p) + 2) {
                out = str::trim(out.substr(p.size()));
                changed = true;
            }
        }
    }
    return out;
}

/// 在 [begin, end) 码点区间内查找 needle（码点级），返回起始码点索引或 npos。
size_t findSub(const std::u32string& u, const std::u32string& needle, size_t begin) {
    if (needle.empty() || u.size() < needle.size()) return std::u32string::npos;
    for (size_t i = begin; i + needle.size() <= u.size(); ++i) {
        if (u.compare(i, needle.size(), needle) == 0) return i;
    }
    return std::u32string::npos;
}

}  // namespace

ActionItemExtractor::ActionItemExtractor(ActionItemOptions options) : options_(options) {}

bool ActionItemExtractor::isActionSentence(const std::string& text, std::string* trigger) {
    const std::string t = str::trim(text);
    if (t.empty()) return false;
    // 疑问句一般不构成待办
    if (str::endsWith(t, "？") || str::endsWith(t, "?")) return false;
    return containsAny(t, actionTriggers(), trigger);
}

std::string ActionItemExtractor::extractOwner(const std::string& text) {
    // 优先使用「强人名标志词」——其左侧大概率是人名
    static const std::vector<std::string> kPersonMarkers = {"负责", "跟进", "落实", "推动",
                                                           "主责", "牵头", "对接", "盯一下",
                                                           "抓一下", "担纲", "承办"};
    static const std::vector<std::string> kLeaderPrefixes = {"由", "请", "让", "叫"};

    const CpIndex idx = buildIndex(text);
    const std::u32string& u = idx.u;

    auto acceptable = [&](const std::string& candidate) {
        const size_t len = str::utf8Length(candidate);
        if (len < 2 || len > 4) return false;
        if (nonNameWords().count(candidate) > 0) return false;
        const std::u32string cp = str::toUtf32(candidate);
        // 候选内部不应含边界字符（避免「张三在」这类粘连带）
        for (char32_t c : cp) {
            if (isNameBoundary(c)) return false;
        }
        // 姓名不会以功能字/副词开头（剔除「继续」「尽快」等）
        if (!cp.empty() && isNonNameStart(cp.front())) return false;
        return true;
    };

    // ---- 规则 1：X + 强人名标志词 ----
    for (const std::string& marker : kPersonMarkers) {
        const std::u32string m = str::toUtf32(marker);
        size_t pos = findSub(u, m, 0);
        while (pos != std::u32string::npos) {
            size_t back = pos;
            size_t taken = 0;
            while (back > 0 && taken < 4 && isCjkAt(u, back - 1) &&
                   !isNameBoundary(u[back - 1])) {
                --back;
                ++taken;
            }
            std::string candidate = cpRange(idx, back, pos);
            // 剥离「由/请/让」前缀
            for (const std::string& lp : kLeaderPrefixes) {
                if (str::startsWith(candidate, lp) &&
                    str::utf8Length(candidate) > str::utf8Length(lp)) {
                    candidate = candidate.substr(lp.size());
                }
            }
            // 剥离尾部助词
            static const std::vector<std::string> kTail = {"来", "去", "再", "先"};
            bool trimmed = true;
            while (trimmed) {
                trimmed = false;
                for (const std::string& t : kTail) {
                    if (str::endsWith(candidate, t) &&
                        str::utf8Length(candidate) > str::utf8Length(t) + 1) {
                        candidate = candidate.substr(0, candidate.size() - t.size());
                        trimmed = true;
                    }
                }
            }
            if (acceptable(candidate)) return candidate;
            pos = findSub(u, m, pos + 1);
        }
    }

    // ---- 规则 2：「由/请/让」+ 姓名 ----
    for (const std::string& lp : kLeaderPrefixes) {
        const std::u32string p = str::toUtf32(lp);
        size_t pos = findSub(u, p, 0);
        while (pos != std::u32string::npos) {
            size_t k = pos + p.size();
            size_t start = k;
            while (k < u.size() && isCjkAt(u, k) && !isNameBoundary(u[k]) &&
                   (k - start) < 4) {
                ++k;
            }
            const std::string candidate = cpRange(idx, start, k);
            if (acceptable(candidate)) return candidate;
            pos = findSub(u, p, pos + 1);
        }
    }

    // ---- 规则 3：@某人 ----
    const size_t at = text.find('@');
    if (at != std::string::npos) {
        std::string name;
        for (char32_t c : str::toUtf32(text.substr(at + 1))) {
            if (str::isCjk(c) || str::isAsciiAlnum(c)) {
                name += str::toUtf8(std::u32string(1, c));
            } else {
                break;
            }
            if (str::utf8Length(name) >= 8) break;
        }
        if (!name.empty() && str::utf8Length(name) >= 2) return name;
    }
    return {};
}

std::string ActionItemExtractor::extractDueDate(const std::string& text,
                                                const std::string& meetingDate,
                                                std::string* rawText) {
    if (rawText) rawText->clear();
    const timeutil::Date parsed = timeutil::parseYmd(meetingDate);
    const timeutil::Date base = parsed.valid ? parsed : timeutil::today();

    // 先把中文数词转成阿拉伯数字，统一在码点层面匹配，避免 UTF-8 多字节切片问题
    const TextNormalizer itn;
    const std::u32string u = str::toUtf32(itn.normalize(text));
    const size_t n = u.size();

    auto setRaw = [&](size_t from, size_t to) {
        if (rawText) *rawText = str::toUtf8(u.substr(from, to - from));
    };
    auto isDigit = [&](size_t i) { return i < n && str::isAsciiDigit(u[i]); };
    auto parseInt = [&](size_t from, size_t to) -> int {
        if (from >= to) return -1;
        return std::stoi(str::toUtf8(u.substr(from, to - from)));
    };
    auto findCp = [&](char32_t c, size_t from) -> size_t {
        for (size_t i = from; i < n; ++i) {
            if (u[i] == c) return i;
        }
        return std::u32string::npos;
    };

    // ---- 1) 具体日期：M月D日 / M月D号 ----
    for (size_t i = 0; i < n; ++i) {
        if (u[i] != U'月') continue;
        size_t mi = i;
        while (mi > 0 && isDigit(mi - 1)) --mi;
        if (mi == i) continue;
        const int month = parseInt(mi, i);
        if (month < 1 || month > 12) continue;
        size_t dj = i + 1;
        while (dj < n && isDigit(dj)) ++dj;
        if (dj == i + 1) continue;
        if (dj >= n || (u[dj] != U'日' && u[dj] != U'号')) continue;
        const int day = parseInt(i + 1, dj);
        if (day < 1 || day > 31) continue;

        timeutil::Date d;
        d.year = base.year;
        d.month = month;
        d.day = day;
        d.valid = true;
        if (timeutil::daysBetween(base, d) < 0) d = timeutil::addDays(d, 365);
        setRaw(mi, dj + 1);
        return timeutil::formatYmd(d);
    }

    // ---- 2) YYYY年M月 ----
    {
        const size_t y = findCp(U'年', 0);
        if (y != std::u32string::npos && y >= 4) {
            bool numericYear = true;
            for (size_t k = y - 4; k < y; ++k) {
                if (!isDigit(k)) { numericYear = false; break; }
            }
            if (numericYear) {
                const int year = parseInt(y - 4, y);
                size_t mj = y + 1;
                while (mj < n && isDigit(mj)) ++mj;
                if (mj > y + 1 && mj < n && u[mj] == U'月') {
                    const int month = parseInt(y + 1, mj);
                    if (month >= 1 && month <= 12) {
                        timeutil::Date d;
                        d.year = year;
                        d.month = month;
                        d.day = 1;
                        d.valid = true;
                        setRaw(y - 4, mj + 1);
                        return timeutil::formatYmd(d);
                    }
                }
            }
        }
    }

    // ---- 3) 相对日 ----
    struct Rel {
        const char* word;
        int days;
    };
    static const Rel kRel[] = {
        {"今天", 0}, {"今日", 0}, {"明天", 1}, {"明日", 1},
        {"后天", 2}, {"大后天", 3},
    };
    // 「今天/明天」也会出现在收尾语里（「今天就到这里」），那并非截止时间。
    // 采用收尾语黑名单而非「必须有施动动词」的门槛：后者会把
    // 「明天给我」「后天回复」这类正常待办一并误杀。
    static const std::vector<std::string> kClosing = {
        "就到这里", "就到这儿", "就结束", "就散会", "先这样", "先到这", "到此为止",
        "再聊", "再说吧", "下次再说", "今天就到这",
    };
    const bool closing = containsAny(text, kClosing);
    for (const Rel& r : kRel) {
        if (closing) break;
        const std::u32string w = str::toUtf32(r.word);
        const size_t pos = findSub(u, w, 0);
        if (pos == std::u32string::npos) continue;
        setRaw(pos, pos + w.size());
        return timeutil::formatYmd(timeutil::addDays(base, r.days));
    }

    // ---- 4) 周几 ----
    {
        struct WeekPrefix {
            const char* text;
            int weekOffset;
        };
        static const WeekPrefix kPrefixes[] = {
            {"下下周", 2}, {"下周", 1}, {"本周", 0}, {"这周", 0}, {"周", 0}};
        static const std::pair<const char*, int> kWeek[] = {
            {"一", 1}, {"二", 2}, {"三", 3}, {"四", 4},
            {"五", 5}, {"六", 6}, {"日", 7}, {"天", 7}};

        for (const WeekPrefix& p : kPrefixes) {
            const std::u32string prefix = str::toUtf32(p.text);
            size_t pos = findSub(u, prefix, 0);
            while (pos != std::u32string::npos) {
                const size_t after = pos + prefix.size();
                for (const auto& [cn, wd] : kWeek) {
                    const std::u32string cnCp = str::toUtf32(cn);
                    if (after + cnCp.size() > n) continue;
                    if (u.compare(after, cnCp.size(), cnCp) != 0) continue;

                    const int cur = timeutil::weekdayOf(base);
                    int offset = (wd - cur) + p.weekOffset * 7;
                    if (offset < 0) offset += 7;
                    const bool explicitWeek = (p.weekOffset != 0) ||
                                              str::contains(str::toUtf8(u), "本周") ||
                                              str::contains(str::toUtf8(u), "这周");
                    if (offset == 0 && !explicitWeek) offset = 7;
                    setRaw(pos, after + cnCp.size());
                    return timeutil::formatYmd(timeutil::addDays(base, offset));
                }
                pos = findSub(u, prefix, pos + 1);
            }
        }
    }

    // ---- 5) X 个工作日 ----
    {
        const std::u32string marker = str::toUtf32("个工作日");
        const size_t idx = findSub(u, marker, 0);
        if (idx != std::u32string::npos) {
            size_t bi = idx;
            while (bi > 0 && isDigit(bi - 1)) --bi;
            if (bi < idx) {
                int cnt = parseInt(bi, idx);
                cnt = std::max(1, std::min(180, cnt));
                int added = 0;
                timeutil::Date d = base;
                while (added < cnt) {
                    d = timeutil::addDays(d, 1);
                    if (timeutil::weekdayOf(d) <= 5) ++added;
                }
                setRaw(bi, idx + marker.size());
                return timeutil::formatYmd(d);
            }
        }
    }

    // ---- 6) 月底 / 季度末 ----
    auto monthEnd = [](timeutil::Date d) {
        d.day = 28;
        while (timeutil::addDays(d, 1).month == d.month) d = timeutil::addDays(d, 1);
        return d;
    };
    for (const char* w : {"月底", "月末"}) {
        const std::u32string cp = str::toUtf32(w);
        const size_t pos = findSub(u, cp, 0);
        if (pos != std::u32string::npos) {
            setRaw(pos, pos + cp.size());
            return timeutil::formatYmd(monthEnd(base));
        }
    }
    for (const char* w : {"季度末", "季末"}) {
        const std::u32string cp = str::toUtf32(w);
        const size_t pos = findSub(u, cp, 0);
        if (pos != std::u32string::npos) {
            static const int kEndMonth[4] = {3, 6, 9, 12};
            timeutil::Date d;
            d.year = base.year;
            d.month = kEndMonth[(base.month - 1) / 3];
            d.day = 28;
            d.valid = true;
            setRaw(pos, pos + cp.size());
            return timeutil::formatYmd(monthEnd(d));
        }
    }

    return {};
}

std::string ActionItemExtractor::detectPriority(const std::string& text) {
    static const std::vector<std::string> kHigh = {"尽快", "紧急", "优先", "必须", "立刻",
                                                   "马上", "今天", "今日", "明天", "立即",
                                                   "加急", "最高"};
    static const std::vector<std::string> kLow = {"可以", "可选", "有空", "不急", "不急吧",
                                                  "有时间", "顺带", "顺便"};
    if (containsAny(text, kHigh)) return "高";
    if (containsAny(text, kLow)) return "低";
    return "中";
}

std::vector<ActionItem> ActionItemExtractor::extract(
    const std::vector<Sentence>& sentences, const std::vector<std::string>& speakerLabels) const {
    std::vector<ActionItem> items;
    std::string meetingDate = options_.meetingDate;
    if (meetingDate.empty()) meetingDate = timeutil::formatYmd(timeutil::today());

    for (const Sentence& s : sentences) {
        std::string trigger;
        const bool isAction = isActionSentence(s.text, &trigger);
        if (!isAction && options_.requireTrigger) continue;

        ActionItem item;
        item.sourceText = s.text;
        item.task = trimDiscourse(s.text);
        if (item.task.empty()) continue;

        item.startMs = s.startMs;
        item.speakerId = s.speakerId;
        item.priority = detectPriority(s.text);

        // 责任人：显式 > 说话人推断
        item.owner = extractOwner(s.text);
        if (item.owner.empty() && options_.inferOwnerFromSpeaker && s.speakerId >= 0 &&
            static_cast<size_t>(s.speakerId) < speakerLabels.size()) {
            const std::string& spk = speakerLabels[static_cast<size_t>(s.speakerId)];
            if (containsAny(s.text, {"我", "我来", "我负责", "我跟进", "我去"})) {
                item.owner = spk;
            }
        }

        std::string raw;
        item.dueDate = extractDueDate(s.text, meetingDate, &raw);
        item.dueText = raw;

        items.push_back(std::move(item));
    }

    // 去重：任务文本完全相同只保留一条
    std::vector<ActionItem> unique;
    std::unordered_set<std::string> seen;
    for (ActionItem& it : items) {
        if (seen.insert(it.task).second) unique.push_back(std::move(it));
    }

    if (options_.maxItems > 0 && static_cast<int>(unique.size()) > options_.maxItems) {
        unique.resize(static_cast<size_t>(options_.maxItems));
    }
    MM_LOG_INFO("action") << "待办抽取完成: " << unique.size() << " 条（候选 " << items.size()
                          << "）";
    return unique;
}

}  // namespace mm::nlp

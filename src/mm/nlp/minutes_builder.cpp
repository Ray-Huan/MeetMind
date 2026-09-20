#include "mm/nlp/minutes_builder.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"
#include "mm/nlp/action_item_extractor.h"
#include "mm/nlp/decision_extractor.h"
#include "mm/nlp/keyword_extractor.h"
#include "mm/nlp/summarizer.h"
#include "mm/nlp/topic_segmenter.h"

namespace mm::nlp {

Json Statistics::toJson() const {
    Json j = Json::object();
    j.set("totalDurationMs", totalDurationMs);
    j.set("totalDurationText", timeutil::formatDuration(totalDurationMs));
    j.set("speechDurationMs", speechDurationMs);
    j.set("speechRatio", speechRatio);
    j.set("totalCharacters", totalCharacters);
    j.set("cjkCharacters", cjkCharacters);
    j.set("segmentCount", segmentCount);
    j.set("sentenceCount", sentenceCount);
    j.set("speakerCount", speakerCount);
    j.set("lowConfidenceSegments", lowConfidenceSegments);

    Json talk = Json::array();
    for (size_t i = 0; i < speakerTalkMs.size(); ++i) {
        Json s = Json::object();
        s.set("speakerId", static_cast<int64_t>(i));
        s.set("talkMs", speakerTalkMs[i]);
        s.set("talkText", timeutil::formatDuration(speakerTalkMs[i]));
        if (i < speakerCharCount.size()) s.set("chars", speakerCharCount[i]);
        talk.push(s);
    }
    j.set("speakers", talk);
    return j;
}

Json Minutes::toJson() const {
    Json j = Json::object();
    j.set("schemaVersion", "1.0");
    j.set("title", title);
    j.set("meetingDate", meetingDate);
    j.set("summaryEngine", summaryEngine);
    j.set("overview", overview);

    Json topicArr = Json::array();
    for (const Topic& t : topics) {
        Json tj = Json::object();
        tj.set("title", t.title);
        tj.set("keywords", Json::of(t.keywords));
        tj.set("startSentence", t.startSentence);
        tj.set("endSentence", t.endSentence);
        tj.set("sentenceCount", t.sentenceCount());
        tj.set("startMs", t.startMs);
        tj.set("endMs", t.endMs);
        topicArr.push(tj);
    }
    j.set("topics", topicArr);

    Json pointArr = Json::array();
    for (const SummarySentence& s : keyPoints) {
        Json sj = Json::object();
        sj.set("text", s.text);
        sj.set("score", s.score);
        sj.set("startMs", s.startMs);
        sj.set("endMs", s.endMs);
        sj.set("speakerId", s.speakerId);
        pointArr.push(sj);
    }
    j.set("keyPoints", pointArr);

    Json decisionArr = Json::array();
    for (const Decision& d : decisions) {
        Json dj = Json::object();
        dj.set("text", d.text);
        dj.set("kind", d.kind);
        dj.set("startMs", d.startMs);
        dj.set("speakerId", d.speakerId);
        decisionArr.push(dj);
    }
    j.set("decisions", decisionArr);

    Json actionArr = Json::array();
    for (const ActionItem& a : actionItems) {
        Json aj = Json::object();
        aj.set("task", a.task);
        aj.set("owner", a.owner);
        aj.set("dueDate", a.dueDate);
        aj.set("dueText", a.dueText);
        aj.set("priority", a.priority);
        aj.set("sourceText", a.sourceText);
        aj.set("startMs", a.startMs);
        aj.set("speakerId", a.speakerId);
        actionArr.push(aj);
    }
    j.set("actionItems", actionArr);

    Json kwArr = Json::array();
    for (const Keyword& k : keywords) {
        Json kj = Json::object();
        kj.set("word", k.word);
        kj.set("score", k.score);
        kj.set("textRank", k.textRank);
        kj.set("tfidf", k.tfidf);
        kj.set("frequency", k.frequency);
        kwArr.push(kj);
    }
    j.set("keywords", kwArr);

    j.set("risks", Json::of(risks));
    j.set("stats", stats.toJson());
    return j;
}

MinutesBuilder::MinutesBuilder(const Tokenizer& tokenizer, MinutesOptions options)
    : tokenizer_(tokenizer), options_(options) {}

std::string MinutesBuilder::deriveTitle(const std::string& fullText, const std::string& fallback) {
    const std::string t = str::trim(fullText);
    if (t.empty()) return fallback;
    // 取首句（≤ 24 码点）作为标题候选
    const std::vector<Sentence> first = SentenceSplitter{}.split(t);
    if (!first.empty()) {
        std::string head = str::trim(first.front().text);
        std::string cleaned;
        for (char32_t c : str::toUtf32(head)) {
            if (str::isSentenceEnd(c)) continue;
            cleaned += str::toUtf8(std::u32string(1, c));
        }
        cleaned = str::trim(cleaned);
        if (str::utf8Length(cleaned) >= 4) {
            if (str::utf8Length(cleaned) > 24) cleaned = str::utf8Substr(cleaned, 0, 24);
            return cleaned;
        }
    }
    return fallback;
}

Result<Minutes> MinutesBuilder::build(const std::vector<Sentence>& sentences,
                                      const std::vector<std::string>& speakerLabels,
                                      const Statistics& stats, ISummarizer* summarizer) const {
    Minutes m;
    m.stats = stats;
    m.meetingDate = options_.meetingDate.empty() ? timeutil::formatYmd(timeutil::today())
                                                 : options_.meetingDate;

    if (sentences.empty()) {
        MM_LOG_WARN("minutes") << "输入句子为空，生成的纪要仅含统计信息";
        m.title = options_.title.empty() ? "空会议" : options_.title;
        m.risks.push_back("未获得任何转写文本，请检查音频质量或识别后端配置");
        return m;
    }

    // ---- 标题 ----
    std::string fullText;
    for (const Sentence& s : sentences) {
        fullText += s.text;
    }
    if (!options_.title.empty()) {
        m.title = options_.title;
    } else {
        const std::string base = m.meetingDate + " 会议纪要";
        m.title = deriveTitle(fullText, base);
    }

    // ---- 关键词 ----
    std::vector<std::vector<std::string>> sentenceTokens;
    sentenceTokens.reserve(sentences.size());
    for (const Sentence& s : sentences) {
        sentenceTokens.push_back(tokenizer_.cut(s.text, false));
    }
    KeywordOptions kwOpts;
    kwOpts.topK = options_.maxKeywords;
    m.keywords = KeywordExtractor{kwOpts}.extractFromTokens(sentenceTokens, tokenizer_);

    // ---- 摘要 ----
    SummaryRequest request;
    request.title = m.title;
    request.sentences = sentences;
    request.keywords = m.keywords;
    request.speakerLabels = speakerLabels;

    if (options_.enableSummarization) {
        std::unique_ptr<ExtractiveSummarizer> owned;
        ISummarizer* impl = summarizer;
        if (impl != nullptr && !impl->available()) {
            MM_LOG_WARN("minutes") << "指定摘要后端不可用，回退抽取式摘要";
            impl = nullptr;
        }
        if (impl == nullptr) {
            ExtractiveOptions eo;
            eo.ratio = options_.summaryRatio;
            eo.maxSentences = options_.summaryMaxSentences;
            owned = std::make_unique<ExtractiveSummarizer>(tokenizer_, eo);
            impl = owned.get();
        }
        Result<SummaryDraft> draft = impl->summarize(request);
        if (draft.ok()) {
            m.overview = draft.value().overview;
            m.keyPoints = draft.value().keyPoints;
            m.summaryEngine = draft.value().engine;
        } else {
            MM_LOG_WARN("minutes") << "摘要生成失败: " << draft.message();
            m.risks.push_back("摘要生成失败：" + draft.message());
        }
    } else {
        m.summaryEngine = "disabled";
    }

    // 概览兜底：摘要不可用时取前两句
    if (m.overview.empty()) {
        std::string fallbackOverview;
        for (size_t i = 0; i < sentences.size() && i < 2; ++i) {
            if (!fallbackOverview.empty()) fallbackOverview += " ";
            fallbackOverview += sentences[i].text;
        }
        m.overview = fallbackOverview;
    }

    // ---- 议题 ----
    TopicSegmentOptions tsOpts;
    tsOpts.blockSentences = std::max(1, options_.topicBlockSentences);
    m.topics = TopicSegmenter{tokenizer_, tsOpts}.segment(sentences);

    // ---- 决议 ----
    DecisionOptions dOpts;
    dOpts.maxItems = 20;
    m.decisions = DecisionExtractor{dOpts}.extract(sentences);

    // ---- 待办 ----
    ActionItemOptions aOpts;
    aOpts.meetingDate = m.meetingDate;
    aOpts.maxItems = options_.maxActionItems;
    m.actionItems = ActionItemExtractor{aOpts}.extract(sentences, speakerLabels);

    // ---- 风险提示 ----
    if (stats.lowConfidenceSegments > 0) {
        m.risks.push_back("存在 " + std::to_string(stats.lowConfidenceSegments) +
                          " 个低置信度转写片段，建议人工核对对应时间点");
    }
    int noOwner = 0;
    for (const ActionItem& a : m.actionItems) {
        if (a.owner.empty()) ++noOwner;
    }
    if (noOwner > 0) {
        m.risks.push_back("有 " + std::to_string(noOwner) + " 条待办未指明责任人");
    }
    if (m.decisions.empty()) {
        m.risks.push_back("未识别到明确决议，若会议有结论请在纪要中补充");
    }
    if (stats.totalDurationMs > 3 * 3600 * 1000) {
        m.risks.push_back("会议时长超过 3 小时，建议拆分议题分别归档");
    }
    if (stats.speechRatio > 0.0 && stats.speechRatio < 0.3) {
        m.risks.push_back("有效语音占比偏低（" +
                          str::numberToString(stats.speechRatio * 100.0, 1) +
                          "%），转写完整性可能受影响");
    }

    MM_LOG_INFO("minutes") << "纪要生成完成: 议题 " << m.topics.size() << " 决策 "
                           << m.decisions.size() << " 待办 " << m.actionItems.size()
                           << " 关键词 " << m.keywords.size();
    return m;
}

}  // namespace mm::nlp

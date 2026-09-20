#include "mm/asr/asr_types.h"

#include <algorithm>

#include "mm/common/string_utils.h"

namespace mm::asr {

std::string AsrResult::fullText(const std::string& sep) const {
    std::string out;
    for (const AsrSegment& s : segments) {
        const std::string t = str::trim(s.text);
        if (t.empty()) continue;
        if (!out.empty()) out += sep;
        out += t;
    }
    return out;
}

int AsrResult::lowConfidenceCount(float threshold) const {
    int count = 0;
    for (const AsrSegment& s : segments) {
        if (!str::trim(s.text).empty() && s.confidence < threshold) ++count;
    }
    return count;
}

double AsrResult::realTimeFactor() const {
    if (elapsedMs <= 0 || audioMs <= 0) return 0.0;
    return static_cast<double>(audioMs) / static_cast<double>(elapsedMs);
}

Json AsrResult::toJson() const {
    Json j = Json::object();
    j.set("language", language);
    j.set("backend", backend);
    j.set("audioMs", audioMs);
    j.set("elapsedMs", elapsedMs);
    j.set("modelLoadMs", modelLoadMs);
    j.set("realTimeFactor", realTimeFactor());
    j.set("segmentCount", static_cast<int64_t>(segments.size()));
    j.set("lowConfidenceCount", lowConfidenceCount());

    Json arr = Json::array();
    for (const AsrSegment& s : segments) {
        Json item = Json::object();
        item.set("startMs", s.startMs);
        item.set("endMs", s.endMs);
        item.set("durationMs", s.durationMs());
        item.set("text", s.text);
        item.set("confidence", static_cast<double>(s.confidence));
        // noSpeechProb 由识别后端给出（whisper 的 no_speech 概率），
        // 是判断「该段是否为幻觉输出」的重要依据，必须随结果一起导出
        item.set("noSpeechProb", static_cast<double>(s.noSpeechProb));
        item.set("speakerId", s.speakerId);
        if (!s.words.empty()) {
            Json words = Json::array();
            for (const AsrWord& w : s.words) {
                Json wj = Json::object();
                wj.set("text", w.text);
                wj.set("startMs", w.startMs);
                wj.set("endMs", w.endMs);
                wj.set("p", static_cast<double>(w.probability));
                words.push(wj);
            }
            item.set("words", words);
        }
        arr.push(item);
    }
    j.set("segments", arr);
    return j;
}

}  // namespace mm::asr

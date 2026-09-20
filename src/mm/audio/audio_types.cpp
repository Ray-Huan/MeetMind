#include "mm/audio/audio_types.h"

#include <algorithm>
#include <cmath>

#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm {
namespace {

std::string fmtDb(double v) {
    if (v <= -99.5) return "-inf";
    return str::numberToString(v, 1);
}

}  // namespace

Json AudioQualityReport::toJson() const {
    Json j = Json::object();
    j.set("durationMs", durationMs);
    j.set("durationText", timeutil::formatDuration(durationMs));
    j.set("sampleRate", sampleRate);
    j.set("channels", channels);
    j.set("bitsPerSample", bitsPerSample);
    j.set("encoding", encoding);
    j.set("totalSamples", totalSamples);
    // 指标型字段一律输出 JSON 数字，便于下游程序直接消费（不做字符串包装）
    j.set("peakDbfs", peakDbfs);
    j.set("rmsDbfs", rmsDbfs);
    j.set("clippingRatio", clippingRatio);
    j.set("nearSilentMs", nearSilentMs);
    j.set("dcOffset", dcOffset);
    return j;
}

std::string AudioQualityReport::toSummary() const {
    std::string s;
    s += "时长 " + timeutil::formatDuration(durationMs);
    s += " | " + std::to_string(sampleRate) + " Hz";
    s += " | " + std::to_string(bitsPerSample) + " bit " + encoding;
    if (channels > 1) s += " | " + std::to_string(channels) + " 声道";
    s += " | 峰值 " + fmtDb(peakDbfs) + " dBFS";
    s += " | RMS " + fmtDb(rmsDbfs) + " dBFS";
    if (clippingRatio > 0.001) {
        s += " | 削波 " + str::numberToString(clippingRatio * 100.0, 2) + "%";
    }
    if (nearSilentMs > 0) {
        s += " | 静音 " + timeutil::formatDuration(nearSilentMs);
    }
    return s;
}

}  // namespace mm

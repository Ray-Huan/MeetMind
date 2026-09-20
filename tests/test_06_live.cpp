// 实时（流式）链路测试：在线 VAD / 实时转写器 / 文件音频源
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "mm/asr/iasr_engine.h"
#include "mm/audio/wav_io.h"
#include "mm/live/audio_source.h"
#include "mm/live/online_vad.h"
#include "mm/live/realtime_transcriber.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

namespace {

/// 记录调用情况并返回固定文本的假后端——用于验证「实时链路」本身，
/// 避免测试依赖真实模型（真实模型在端到端脚本里验证）。
class MockAsr final : public asr::IAsrEngine {
public:
    std::string id() const override { return "mock"; }
    std::string displayName() const override { return "mock（测试用）"; }
    asr::AsrCapabilities capabilities() const override { return asr::AsrCapabilities{}; }

    Result<void> initialize(const asr::AsrModelConfig&) override {
        ready_ = true;
        return okStatus();
    }
    bool ready() const override { return ready_; }

    Result<asr::AsrSegment> transcribe(const AudioBuffer& chunk, int64_t startMs,
                                       const CancelToken*) override {
        const int n = ++calls;
        asr::AsrSegment s;
        s.startMs = startMs;
        s.endMs = startMs + chunk.durationMs();
        s.text = "第" + std::to_string(n) + "段";
        s.confidence = 0.9f;
        {
            std::lock_guard<std::mutex> lk(mu);
            starts.push_back(startMs);
            sampleCounts.push_back(chunk.samples.size());
        }
        return success(std::move(s));
    }

    int callCount() const { return calls.load(); }

    std::atomic<int> calls{0};
    mutable std::mutex mu;
    std::vector<int64_t> starts;
    std::vector<size_t> sampleCounts;

private:
    bool ready_ = false;
};

}  // namespace

// ============================ 在线 VAD ============================

MM_TEST(live_vad, 交替语音按静音断句) {
    live::OnlineVadConfig cfg;
    cfg.exitFrames = 30;   // 300 ms 静音即断句
    live::OnlineVad vad(cfg);

    // 300ms 静音 + 3×(1000ms 语音 + 500ms 静音) + 800ms 静音
    AudioBuffer clip = mmtest::makeAlternatingClip(1000, 500, 3, 300, 800);
    std::vector<SpeechSegment> segs = vad.feed(clip.samples.data(), clip.samples.size());

    MM_EXPECT_EQ(segs.size(), static_cast<size_t>(3));
    for (const SpeechSegment& s : segs) {
        MM_EXPECT_GE(s.durationMs(), static_cast<int64_t>(800));   // 不应把语音切碎
        MM_EXPECT_LT(s.durationMs(), static_cast<int64_t>(1400));  // 也不应跨越静音并段
    }
    // 时间戳递增
    for (size_t i = 1; i < segs.size(); ++i) {
        MM_EXPECT_GT(segs[i].startMs, segs[i - 1].endMs);
    }
}

MM_TEST(live_vad, 分块喂入与整体喂入结果一致) {
    AudioBuffer clip = mmtest::makeAlternatingClip(700, 450, 3, 250, 700);

    live::OnlineVadConfig cfg;
    cfg.exitFrames = 25;
    live::OnlineVad whole(cfg);
    std::vector<SpeechSegment> a = whole.feed(clip.samples.data(), clip.samples.size());

    live::OnlineVad chunked(cfg);
    std::vector<SpeechSegment> b;
    const size_t step = 137;   // 刻意取非整数倍的块长
    for (size_t i = 0; i < clip.samples.size(); i += step) {
        const size_t n = std::min(step, clip.samples.size() - i);
        std::vector<SpeechSegment> part = chunked.feed(clip.samples.data() + i, n);
        b.insert(b.end(), part.begin(), part.end());
    }

    MM_REQUIRE_TRUE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        MM_EXPECT_EQ(a[i].startMs, b[i].startMs);
        MM_EXPECT_EQ(a[i].endMs, b[i].endMs);
    }
}

MM_TEST(live_vad, 过短突发被丢弃) {
    live::OnlineVadConfig cfg;
    cfg.exitFrames = 20;      // 200 ms
    cfg.minSpeechMs = 250;
    live::OnlineVad vad(cfg);

    AudioBuffer clip = mmtest::makeSilence(300);
    mmtest::appendSpeechLike(clip, 100);   // 100 ms 突发 < minSpeechMs
    mmtest::appendSilence(clip, 600);

    std::vector<SpeechSegment> segs = vad.feed(clip.samples.data(), clip.samples.size());
    MM_EXPECT_EQ(segs.size(), static_cast<size_t>(0));
}

MM_TEST(live_vad, 强制闭合产出尾段) {
    live::OnlineVadConfig cfg;
    cfg.exitFrames = 30;
    live::OnlineVad vad(cfg);

    AudioBuffer clip = mmtest::makeSilence(200);
    mmtest::appendSpeechLike(clip, 900);
    // 结尾没有静音 → 常规 feed 不会闭合
    std::vector<SpeechSegment> segs = vad.feed(clip.samples.data(), clip.samples.size());
    MM_EXPECT_EQ(segs.size(), static_cast<size_t>(0));
    MM_EXPECT_TRUE(vad.inSpeech());

    SpeechSegment tail;
    MM_REQUIRE_TRUE(vad.forceClose(&tail));
    MM_EXPECT_GE(tail.durationMs(), static_cast<int64_t>(700));
    MM_EXPECT_FALSE(vad.inSpeech());
    MM_EXPECT_FALSE(vad.forceClose(&tail));   // 不再在语音中，应返回 false
}

// ============================ 实时转写器 ============================

MM_TEST(live_rt, 流式产出多段且时间戳有序) {
    MockAsr engine;
    engine.initialize(asr::AsrModelConfig{});

    live::RealtimeConfig cfg;
    cfg.vad.exitFrames = 30;
    cfg.maxSegmentMs = 8000;

    live::RealtimeTranscriber rt(&engine, cfg);

    std::vector<live::RealtimeEvent> events;
    std::mutex mu;
    rt.setCallback([&](const live::RealtimeEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(ev);
    });
    MM_REQUIRE_TRUE(rt.start().ok());

    AudioBuffer clip = mmtest::makeAlternatingClip(900, 500, 3, 300, 700);
    rt.push(clip.samples.data(), clip.samples.size());
    rt.flush();
    rt.stop();

    MM_EXPECT_EQ(events.size(), static_cast<size_t>(3));
    for (size_t i = 1; i < events.size(); ++i) {
        MM_EXPECT_GE(events[i].segment.startMs, events[i - 1].segment.startMs);
    }
    const live::RealtimeStats st = rt.stats();
    MM_EXPECT_EQ(st.segments, static_cast<int64_t>(3));
    MM_EXPECT_GT(st.chars, static_cast<int64_t>(0));
}

MM_TEST(live_rt, 超长语音被强制切段以限时延) {
    MockAsr engine;
    engine.initialize(asr::AsrModelConfig{});

    live::RealtimeConfig cfg;
    cfg.vad.exitFrames = 100;    // 1 s 才断句：句内停顿不会提前切
    cfg.maxSegmentMs = 1000;     // 强制 1 s 一段

    live::RealtimeTranscriber rt(&engine, cfg);
    MM_REQUIRE_TRUE(rt.start().ok());

    // 3.5 s 连续语音，按 100 ms 分块推送（模拟实时到达）
    AudioBuffer clip = mmtest::makeSpeechLike(3500, 0.25, 16000, 7u);
    const size_t chunk = 1600;   // 100 ms
    for (size_t i = 0; i < clip.samples.size(); i += chunk) {
        const size_t n = std::min(chunk, clip.samples.size() - i);
        rt.push(clip.samples.data() + i, n);
    }
    rt.flush();
    rt.stop();

    MM_EXPECT_GE(engine.callCount(), 3);   // 连续语音也被切成多段
    const live::RealtimeStats st = rt.stats();
    MM_EXPECT_GE(st.segments, static_cast<int64_t>(3));
}

MM_TEST(live_rt, 未启动推送不产出且统计为零) {
    MockAsr engine;
    engine.initialize(asr::AsrModelConfig{});
    live::RealtimeConfig cfg;
    cfg.vad.exitFrames = 20;

    live::RealtimeTranscriber rt(&engine, cfg);
    int callbacks = 0;
    rt.setCallback([&](const live::RealtimeEvent&) { ++callbacks; });

    AudioBuffer clip = mmtest::makeAlternatingClip(800, 400, 2, 200, 600);
    rt.push(clip.samples.data(), clip.samples.size());   // 未 start
    MM_EXPECT_EQ(callbacks, 0);
    MM_EXPECT_EQ(rt.stats().segments, static_cast<int64_t>(0));
}

// ============================ 文件音频源 ============================

MM_TEST(live_source, 文件音频源完整推送且采样率正确) {
    AudioBuffer src = mmtest::makeTone(440.0, 600, 0.3, 16000);
    const std::string path = mmtest::outPath("live/tone.wav");
    MM_REQUIRE_TRUE(wav::write16(path, src).ok());

    live::FileAudioSource source(path, /*speed=*/50.0, /*chunkMs=*/100);
    MM_REQUIRE_TRUE(source.loaded());
    MM_EXPECT_EQ(source.sampleRate(), 16000);
    MM_EXPECT_EQ(source.durationMs(), static_cast<int64_t>(600));

    std::vector<float> collected;
    std::mutex mu;
    Result<void> started = source.start([&](const float* samples, size_t count) {
        std::lock_guard<std::mutex> lk(mu);
        collected.insert(collected.end(), samples, samples + count);
    });
    MM_REQUIRE_TRUE(started.ok());

    while (source.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    source.stop();

    MM_EXPECT_EQ(collected.size(), src.samples.size());
}

MM_TEST(live_source, 不存在的文件载入失败) {
    live::FileAudioSource source(mmtest::outPath("live/not_exist.wav"), 1.0, 100);
    MM_EXPECT_FALSE(source.loaded());
    MM_EXPECT_FALSE(source.loadError().empty());
}

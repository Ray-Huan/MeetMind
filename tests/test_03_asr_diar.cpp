// ASR 与说话人分离测试
#include <algorithm>
#include <cmath>
#include <vector>

#include "mm/asr/asr_factory.h"
#include "mm/asr/null_engine.h"
#include "mm/asr/replay_engine.h"
#include "mm/asr/whisper_engine.h"
#include "mm/diar/cluster.h"
#include "mm/diar/diarizer.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

// ============================ 回放引擎 ============================

MM_TEST(asr_replay, 按顺序输出脚本且时间戳跟随音频) {
    asr::ReplayAsrEngine engine;
    engine.setScript({"第一句话", "第二句话", ""});
    MM_REQUIRE_TRUE(engine.initialize({}).ok());
    MM_EXPECT_TRUE(engine.ready());
    MM_EXPECT_EQ(engine.id(), std::string("replay"));

    AudioBuffer chunk = mmtest::makeSilence(1000, 16000);

    Result<asr::AsrSegment> r1 = engine.transcribe(chunk, 0, nullptr);
    MM_REQUIRE_TRUE(r1.ok());
    MM_EXPECT_EQ(r1.value().text, std::string("第一句话"));
    MM_EXPECT_EQ(r1.value().startMs, static_cast<int64_t>(0));
    MM_EXPECT_EQ(r1.value().endMs, static_cast<int64_t>(1000));

    Result<asr::AsrSegment> r2 = engine.transcribe(chunk, 5000, nullptr);
    MM_REQUIRE_TRUE(r2.ok());
    MM_EXPECT_EQ(r2.value().text, std::string("第二句话"));
    MM_EXPECT_EQ(r2.value().startMs, static_cast<int64_t>(5000));

    // 空行表示该段无语音
    Result<asr::AsrSegment> r3 = engine.transcribe(chunk, 9000, nullptr);
    MM_REQUIRE_TRUE(r3.ok());
    MM_EXPECT_TRUE(r3.value().text.empty());
    MM_EXPECT_NEAR(r3.value().noSpeechProb, 1.0, 1e-6);

    // 脚本耗尽后返回空段，不越界
    Result<asr::AsrSegment> r4 = engine.transcribe(chunk, 12000, nullptr);
    MM_REQUIRE_TRUE(r4.ok());
    MM_EXPECT_TRUE(r4.value().text.empty());
}

MM_TEST(asr_replay, 脚本文件加载与注释处理) {
    const std::string path = mmtest::outPath("asr/script.txt");
    {
        std::ofstream out(path, std::ios::binary);
        out << "# 这是注释\n";
        out << "我们开始今天的周会\n";
        out << "\n";  // 空行 = 无语音段
        out << "  第二句带前后空格  \n";
        out << "# 又一条注释\n";
    }
    asr::ReplayAsrEngine engine;
    Result<void> loaded = engine.loadScript(path);
    MM_REQUIRE_TRUE(loaded.ok());
    MM_EXPECT_EQ(engine.scriptSize(), static_cast<size_t>(3));

    MM_REQUIRE_TRUE(engine.initialize({}).ok());
    const AudioBuffer chunk = mmtest::makeSilence(500, 16000);
    MM_EXPECT_EQ(engine.transcribe(chunk, 0, nullptr).value().text,
                 std::string("我们开始今天的周会"));
    MM_EXPECT_TRUE(engine.transcribe(chunk, 500, nullptr).value().text.empty());
    MM_EXPECT_EQ(engine.transcribe(chunk, 1000, nullptr).value().text,
                 std::string("第二句带前后空格"));
    MM_EXPECT_EQ(engine.consumed(), static_cast<size_t>(3));
}

MM_TEST(asr_replay, 脚本文件不存在或为空时报错) {
    asr::ReplayAsrEngine engine;
    MM_EXPECT_FALSE(engine.loadScript(mmtest::outPath("asr/none.txt")).ok());

    const std::string empty = mmtest::outPath("asr/empty.txt");
    {
        std::ofstream out(empty, std::ios::binary);
        out << "# 只有注释\n";
    }
    MM_EXPECT_FALSE(engine.loadScript(empty).ok());
}

MM_TEST(asr_replay, 取消令牌生效) {
    asr::ReplayAsrEngine engine;
    engine.setScript({"一句话"});
    MM_REQUIRE_TRUE(engine.initialize({}).ok());

    CancelToken cancel;
    cancel.cancel();
    Result<asr::AsrSegment> r = engine.transcribe(mmtest::makeSilence(100, 16000), 0, &cancel);
    MM_EXPECT_FALSE(r.ok());
    MM_EXPECT_TRUE(r.code() == ErrorCode::Cancelled);
}

MM_TEST(asr_replay, 循环模式) {
    asr::ReplayAsrEngine engine;
    engine.setScript({"A", "B"});
    engine.setLoop(true);
    MM_REQUIRE_TRUE(engine.initialize({}).ok());
    const AudioBuffer chunk = mmtest::makeSilence(200, 16000);
    MM_EXPECT_EQ(engine.transcribe(chunk, 0, nullptr).value().text, std::string("A"));
    MM_EXPECT_EQ(engine.transcribe(chunk, 0, nullptr).value().text, std::string("B"));
    MM_EXPECT_EQ(engine.transcribe(chunk, 0, nullptr).value().text, std::string("A"));
}

// ============================ 空引擎 ============================

MM_TEST(asr_null, 返回空文本段且时间戳正确) {
    asr::NullAsrEngine engine;
    MM_REQUIRE_TRUE(engine.initialize({}).ok());
    MM_EXPECT_TRUE(engine.ready());
    Result<asr::AsrSegment> r = engine.transcribe(mmtest::makeSilence(400, 16000), 2000, nullptr);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_TRUE(r.value().text.empty());
    MM_EXPECT_EQ(r.value().startMs, static_cast<int64_t>(2000));
    MM_EXPECT_EQ(r.value().endMs, static_cast<int64_t>(2400));
}

// ============================ 工厂 ============================

MM_TEST(asr_factory, 显式指定replay后端) {
    Config cfg;
    cfg.backend = AsrBackend::Replay;
    Result<asr::EngineSelection> sel = asr::AsrFactory::create(cfg);
    MM_REQUIRE_TRUE(sel.ok());
    MM_EXPECT_EQ(sel.value().engine->id(), std::string("replay"));
    MM_EXPECT_FALSE(sel.value().reason.empty());
    MM_EXPECT_FALSE(sel.value().degraded);  // 显式指定不算降级
}

MM_TEST(asr_factory, 显式指定null后端) {
    Config cfg;
    cfg.backend = AsrBackend::Null;
    Result<asr::EngineSelection> sel = asr::AsrFactory::create(cfg);
    MM_REQUIRE_TRUE(sel.ok());
    MM_EXPECT_EQ(sel.value().engine->id(), std::string("null"));
}

MM_TEST(asr_factory, 模型缺失时自动降级) {
    Config cfg;
    cfg.backend = AsrBackend::Auto;
    cfg.modelPath = mmtest::outPath("asr/不存在.bin");
    std::string reason;
    const AsrBackend resolved = asr::AsrFactory::resolveBackend(cfg, &reason);
    MM_EXPECT_FALSE(reason.empty());

    Result<asr::EngineSelection> sel = asr::AsrFactory::create(cfg);
    MM_REQUIRE_TRUE(sel.ok());
    if (!asr::WhisperCppEngine::compiledIn()) {
        MM_EXPECT_TRUE(resolved == AsrBackend::Replay);
        MM_EXPECT_TRUE(sel.value().degraded);
    } else {
        MM_EXPECT_TRUE(resolved == AsrBackend::Replay);
    }
}

MM_TEST(asr_factory, 模型探测返回合理结果) {
    const std::string detected = asr::AsrFactory::detectDefaultModel();
    // 无法保证测试环境存在模型；若探测到则必须是已存在的文件
    if (!detected.empty()) {
        MM_EXPECT_TRUE(std::filesystem::exists(detected));
    }
    // 探测函数会在多个候选目录间回退；无论命中与否，返回值必须是磁盘上真实存在的文件
    const std::string viaHint =
        asr::AsrFactory::detectDefaultModel(mmtest::outPath("no-such-dir"));
    MM_EXPECT_TRUE(viaHint.empty() || std::filesystem::exists(viaHint));
}

// ============================ AsrResult ============================

MM_TEST(asr_result, 全文拼接与统计) {
    asr::AsrResult r;
    r.audioMs = 10000;
    r.elapsedMs = 2000;
    asr::AsrSegment a;
    a.text = "第一句。";
    a.confidence = 0.9f;
    asr::AsrSegment b;
    b.text = " 第二句。 ";
    b.confidence = 0.3f;
    r.segments = {a, b};

    MM_EXPECT_EQ(r.fullText(), std::string("第一句。第二句。"));
    MM_EXPECT_EQ(r.lowConfidenceCount(0.45f), 1);
    MM_EXPECT_NEAR(r.realTimeFactor(), 5.0, 1e-9);

    const Json j = r.toJson();
    MM_EXPECT_EQ(j.get("segmentCount").asInt(), 2);
    MM_EXPECT_EQ(j.get("lowConfidenceCount").asInt(), 1);
}

MM_TEST(asr_result, 空结果与零耗时) {
    asr::AsrResult r;
    MM_EXPECT_EQ(r.fullText(), std::string(""));
    MM_EXPECT_NEAR(r.realTimeFactor(), 0.0, 1e-12);
    MM_EXPECT_EQ(r.lowConfidenceCount(), 0);
}

// ============================ 聚类 ============================

MM_TEST(diar_cluster, 余弦距离性质) {
    const std::vector<float> a = {1.0f, 0.0f, 0.0f};
    const std::vector<float> b = {1.0f, 0.0f, 0.0f};
    const std::vector<float> c = {0.0f, 1.0f, 0.0f};
    const std::vector<float> d = {-1.0f, 0.0f, 0.0f};
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance(a, b), 0.0, 1e-6);
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance(a, c), 1.0, 1e-6);
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance(a, d), 2.0, 1e-6);
    // 维度不一致与零向量
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance(a, {1.0f}), 1.0, 1e-6);
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance(a, {}), 1.0, 1e-6);
    MM_EXPECT_NEAR(diar::AgglomerativeClusterer::cosineDistance({0.0f, 0.0f}, {1.0f, 0.0f}),
                   1.0, 1e-6);
}

MM_TEST(diar_cluster, 两个可分簇被正确识别) {
    std::vector<std::vector<float>> features;
    // 簇 A：接近 (1, 0, 0)
    for (int i = 0; i < 6; ++i) {
        const float jitter = 0.02f * static_cast<float>(i % 3);
        features.push_back({1.0f, jitter, -jitter});
    }
    // 簇 B：接近 (0, 1, 0)
    for (int i = 0; i < 6; ++i) {
        const float jitter = 0.02f * static_cast<float>(i % 3);
        features.push_back({jitter, 1.0f, jitter});
    }

    diar::ClusterConfig cfg;
    cfg.mergeThreshold = 0.3;
    const diar::ClusterResult r = diar::AgglomerativeClusterer::cluster(features, cfg);
    MM_EXPECT_EQ(r.clusterCount, 2);
    MM_EXPECT_EQ(r.labels.size(), static_cast<size_t>(12));
    for (int i = 0; i < 6; ++i) {
        MM_EXPECT_EQ(r.labels[static_cast<size_t>(i)], r.labels[0]);
    }
    for (int i = 6; i < 12; ++i) {
        MM_EXPECT_EQ(r.labels[static_cast<size_t>(i)], r.labels[6]);
    }
    MM_EXPECT_NE(r.labels[0], r.labels[6]);
}

MM_TEST(diar_cluster, 指定簇数优先于阈值) {
    std::vector<std::vector<float>> features;
    for (int i = 0; i < 9; ++i) {
        features.push_back({1.0f, 0.01f * static_cast<float>(i), 0.0f});
    }
    diar::ClusterConfig cfg;
    cfg.targetClusters = 3;
    cfg.mergeThreshold = 0.9;  // 阈值极大，但目标簇数优先
    const diar::ClusterResult r = diar::AgglomerativeClusterer::cluster(features, cfg);
    MM_EXPECT_EQ(r.clusterCount, 3);
}

MM_TEST(diar_cluster, 边界输入安全) {
    const diar::ClusterResult empty = diar::AgglomerativeClusterer::cluster({}, {});
    MM_EXPECT_EQ(empty.clusterCount, 0);
    MM_EXPECT_EQ(empty.labels.size(), static_cast<size_t>(0));

    const diar::ClusterResult single = diar::AgglomerativeClusterer::cluster({{1.0f, 2.0f}}, {});
    MM_EXPECT_EQ(single.clusterCount, 1);
    MM_EXPECT_EQ(single.labels.size(), static_cast<size_t>(1));

    // 全零向量不应崩溃
    const diar::ClusterResult zeros =
        diar::AgglomerativeClusterer::cluster({{0.0f, 0.0f}, {0.0f, 0.0f}}, {});
    MM_EXPECT_EQ(zeros.labels.size(), static_cast<size_t>(2));
}

MM_TEST(diar_cluster, 簇大小统计与跳变估计) {
    std::vector<std::vector<float>> features;
    for (int i = 0; i < 5; ++i) features.push_back({1.0f, 0.0f});
    for (int i = 0; i < 3; ++i) features.push_back({0.0f, 1.0f});
    diar::ClusterConfig cfg;
    cfg.mergeThreshold = 0.3;
    const diar::ClusterResult r = diar::AgglomerativeClusterer::cluster(features, cfg);
    const auto sizes = r.clusterSizes();
    MM_EXPECT_EQ(sizes.size(), static_cast<size_t>(2));
    MM_EXPECT_EQ(sizes.at(0), 5);
    MM_EXPECT_EQ(sizes.at(1), 3);

    MM_EXPECT_EQ(diar::AgglomerativeClusterer::estimateClusters({}), 1);
    // 距离序列在 index 1 处跳变 → 距离序列长度 4 → 建议 4-1+1 = 4 簇
    const std::vector<double> jumps = {0.1, 0.5, 0.55, 0.6};
    MM_EXPECT_EQ(diar::AgglomerativeClusterer::estimateClusters(jumps, 1.5, 8), 4);
    MM_EXPECT_EQ(diar::AgglomerativeClusterer::estimateClusters({0.1, 0.11, 0.12}, 1.5, 8), 1);
}

// ============================ 说话人分离 ============================

MM_TEST(diar_diarizer, 声纹嵌入维度与归一化) {
    diar::DiarizationConfig cfg;
    const AudioBuffer seg = mmtest::makeSpeechLike(1500, 0.25, 16000, 3u);
    const std::vector<float> emb = diar::SpeakerDiarizer::computeEmbedding(seg, cfg);
    MM_EXPECT_GT(emb.size(), static_cast<size_t>(0));
    // MFCC 13 维 × (均值 + 标准差 + 一阶差分均值) = 39
    MM_EXPECT_EQ(emb.size(), static_cast<size_t>(39));

    double norm = 0.0;
    for (float v : emb) norm += static_cast<double>(v) * v;
    MM_EXPECT_NEAR(std::sqrt(norm), 1.0, 1e-3);
}

MM_TEST(diar_diarizer, 相同音色嵌入距离小于不同音色) {
    diar::DiarizationConfig cfg;
    const auto a = diar::SpeakerDiarizer::computeEmbedding(
        mmtest::makeSpeechLike(1500, 0.25, 16000, 5u, 100.0), cfg);
    const auto b = diar::SpeakerDiarizer::computeEmbedding(
        mmtest::makeSpeechLike(1500, 0.25, 16000, 5u, 100.0), cfg);
    const auto c = diar::SpeakerDiarizer::computeEmbedding(
        mmtest::makeSpeechLike(1500, 0.25, 16000, 5u, 320.0), cfg);
    const double same = diar::AgglomerativeClusterer::cosineDistance(a, b);
    const double diff = diar::AgglomerativeClusterer::cosineDistance(a, c);
    MM_EXPECT_NEAR(same, 0.0, 1e-6);
    MM_EXPECT_GT(diff, same);
}

MM_TEST(diar_diarizer, 段数不足时退化为单说话人) {
    const AudioBuffer audio = mmtest::makeSpeechLike(1000, 0.25, 16000, 1u);
    std::vector<SpeechSegment> segs;
    SpeechSegment s;
    s.startMs = 0;
    s.endMs = 1000;
    segs.push_back(s);

    diar::SpeakerDiarizer d;
    Result<diar::DiarizationResult> r = d.process(audio, segs);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().speakerCount, 1);
    MM_EXPECT_TRUE(r.value().degraded);
    MM_EXPECT_EQ(r.value().speakerIds.size(), static_cast<size_t>(1));
    MM_EXPECT_FALSE(r.value().note.empty());
    MM_EXPECT_EQ(r.value().turns.size(), static_cast<size_t>(1));
}

MM_TEST(diar_diarizer, 关闭分离时直接返回单说话人) {
    const AudioBuffer audio = mmtest::makeSpeechLike(2000, 0.25, 16000, 1u);
    std::vector<SpeechSegment> segs;
    for (int i = 0; i < 4; ++i) {
        SpeechSegment s;
        s.startMs = i * 500;
        s.endMs = i * 500 + 400;
        segs.push_back(s);
    }
    diar::DiarizationConfig cfg;
    cfg.enabled = false;
    diar::SpeakerDiarizer d(cfg);
    Result<diar::DiarizationResult> r = d.process(audio, segs);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().speakerCount, 1);
    MM_EXPECT_TRUE(r.value().degraded);
}

MM_TEST(diar_diarizer, 输出标签数量与段数一致且轮次连续) {
    // 交替音色：低音调 / 高音调各 4 段
    AudioBuffer audio;
    audio.sampleRate = 16000;
    std::vector<SpeechSegment> segs;
    for (int i = 0; i < 8; ++i) {
        const double f0 = (i % 2 == 0) ? 95.0 : 300.0;
        const size_t start = audio.samples.size();
        mmtest::appendSpeechLike(audio, 1200, 0.25, 20u + static_cast<uint32_t>(i));
        // 用不同基频重写该段以制造音色差异
        for (size_t k = start; k < audio.samples.size(); ++k) {
            const double t = static_cast<double>(k - start) / 16000.0;
            audio.samples[k] = static_cast<float>(0.25 * std::sin(2.0 * mmtest::kPi * f0 * t));
        }
        SpeechSegment s;
        s.startMs = samplesToMs(start, 16000);
        s.endMs = samplesToMs(audio.samples.size(), 16000);
        segs.push_back(s);
    }

    diar::DiarizationConfig cfg;
    cfg.targetSpeakers = 2;      // 明确告知是 2 人，避免依赖自动估计
    cfg.mergeThreshold = 0.6;
    cfg.smoothingWindow = 1;     // 关闭平滑以检验聚类本身
    diar::SpeakerDiarizer d(cfg);
    Result<diar::DiarizationResult> r = d.process(audio, segs);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().speakerIds.size(), segs.size());
    for (int id : r.value().speakerIds) {
        MM_EXPECT_GE(id, 0);
        MM_EXPECT_LT(id, r.value().speakerCount);
    }
    MM_EXPECT_EQ(r.value().speakerCount, 2);
    MM_EXPECT_GE(r.value().turns.size(), static_cast<size_t>(2));

    int totalSegments = 0;
    for (const auto& t : r.value().turns) {
        MM_EXPECT_GT(t.durationMs(), 0);
        totalSegments += t.segmentCount;
        // 相邻轮次说话人必须不同
    }
    MM_EXPECT_EQ(totalSegments, static_cast<int>(segs.size()));
    for (size_t i = 1; i < r.value().turns.size(); ++i) {
        MM_EXPECT_NE(r.value().turns[i].speakerId, r.value().turns[i - 1].speakerId);
    }
}

MM_TEST(diar_diarizer, 空段输入安全) {
    diar::SpeakerDiarizer d;
    Result<diar::DiarizationResult> r = d.process(mmtest::makeSilence(500, 16000), {});
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().speakerCount, 0);
    MM_EXPECT_TRUE(r.value().degraded);
    MM_EXPECT_FALSE(r.value().toJson().dump().empty());
}

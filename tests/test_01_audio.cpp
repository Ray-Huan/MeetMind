// 音频层测试：WAV 读写 / 重采样 / FFT / Mel-MFCC / VAD / 分段
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "mm/audio/fft.h"
#include "mm/audio/mel.h"
#include "mm/audio/resampler.h"
#include "mm/audio/segmenter.h"
#include "mm/audio/vad.h"
#include "mm/audio/wav_io.h"
#include "test_helpers.h"
#include "testing.h"

using namespace mm;

// ============================ WAV ============================

MM_TEST(audio_wav, 写入后读回且误差在量化范围内) {
    const int sr = 16000;
    AudioBuffer src = mmtest::makeTone(440.0, 500, 0.5, sr);

    const std::string path = mmtest::outPath("wav/roundtrip.wav");
    Result<void> w = wav::write16(path, src);
    MM_REQUIRE_TRUE(w.ok());

    AudioQualityReport report;
    Result<AudioBuffer> back = wav::read(path, &report);
    MM_REQUIRE_TRUE(back.ok());
    MM_EXPECT_EQ(back.value().sampleRate, sr);
    MM_EXPECT_EQ(back.value().samples.size(), src.samples.size());
    MM_EXPECT_EQ(report.sampleRate, sr);
    MM_EXPECT_EQ(report.channels, 1);

    double maxErr = 0.0;
    for (size_t i = 0; i < src.samples.size(); ++i) {
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(src.samples[i]) -
                                            static_cast<double>(back.value().samples[i])));
    }
    // int16 量化步长约为 3.05e-5
    MM_EXPECT_LT(maxErr, 1e-4);
}

MM_TEST(audio_wav, 探针读取元信息) {
    AudioBuffer src = mmtest::makeTone(1000.0, 300, 0.3, 44100);
    const std::string path = mmtest::outPath("wav/probe.wav");
    MM_REQUIRE_TRUE(wav::write16(path, src).ok());

    Result<WavInfo> info = wav::probe(path);
    MM_REQUIRE_TRUE(info.ok());
    MM_EXPECT_EQ(info.value().sampleRate, 44100);
    MM_EXPECT_EQ(info.value().channels, 1);
    MM_EXPECT_EQ(info.value().bitsPerSample, 16);
    MM_EXPECT_EQ(info.value().encodingName, std::string("PCM16"));
    MM_EXPECT_NEAR(static_cast<double>(info.value().frameCount),
                   static_cast<double>(src.samples.size()), 1.0);

    // 探针只需读文件头，不应把整个文件读入内存（此处验证结果正确即可）
    MM_EXPECT_GT(info.value().dataBytes, 0);
}

MM_TEST(audio_wav, float32读写) {
    AudioBuffer src = mmtest::makeTone(750.0, 200, 0.8, 16000);
    const std::string path = mmtest::outPath("wav/float32.wav");
    MM_REQUIRE_TRUE(wav::write(path, src, 32).ok());

    Result<WavInfo> info = wav::probe(path);
    MM_REQUIRE_TRUE(info.ok());
    MM_EXPECT_EQ(info.value().formatCode, 3);
    MM_EXPECT_EQ(info.value().bitsPerSample, 32);
    MM_EXPECT_EQ(info.value().encodingName, std::string("FLOAT32"));

    Result<AudioBuffer> back = wav::read(path);
    MM_REQUIRE_TRUE(back.ok());
    MM_EXPECT_EQ(back.value().samples.size(), src.samples.size());
    MM_EXPECT_NEAR(back.value().samples[100], src.samples[100], 1e-6);
}

MM_TEST(audio_wav, 多声道下混为单声道) {
    // 手工构造立体声 WAV：L = 正弦, R = -正弦 → 下混应接近 0
    const int sr = 16000;
    const size_t frames = 1600;
    const int bits = 16;
    const uint32_t dataBytes = static_cast<uint32_t>(frames * 2 * (bits / 8));

    std::vector<uint8_t> bytes;
    auto push16 = [&](uint16_t v) {
        bytes.push_back(static_cast<uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto push32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    auto pushText = [&](const char* s) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(s[i]));
    };

    pushText("RIFF");
    push32(36 + dataBytes);
    pushText("WAVE");
    pushText("fmt ");
    push32(16);
    push16(1);                              // PCM
    push16(2);                              // 立体声
    push32(static_cast<uint32_t>(sr));
    push32(static_cast<uint32_t>(sr * 2 * (bits / 8)));  // byte rate
    push16(static_cast<uint16_t>(2 * (bits / 8)));       // block align
    push16(static_cast<uint16_t>(bits));
    pushText("data");
    push32(dataBytes);

    for (size_t i = 0; i < frames; ++i) {
        const double v = 0.5 * std::sin(2.0 * mmtest::kPi * 500.0 * static_cast<double>(i) / sr);
        const int16_t l = static_cast<int16_t>(v * 32767.0);
        const int16_t r = static_cast<int16_t>(-v * 32767.0);
        push16(static_cast<uint16_t>(l));
        push16(static_cast<uint16_t>(r));
    }

    AudioQualityReport report;
    Result<AudioBuffer> buf = wav::readFromMemory(bytes, &report);
    MM_REQUIRE_TRUE(buf.ok());
    MM_EXPECT_EQ(buf.value().samples.size(), frames);
    MM_EXPECT_EQ(report.channels, 2);

    double peak = 0.0;
    for (float s : buf.value().samples) peak = std::max(peak, std::fabs(static_cast<double>(s)));
    MM_EXPECT_LT(peak, 0.01);  // 左右反相 → 下混后接近静音
}

MM_TEST(audio_wav, 损坏与不支持的文件返回错误) {
    // 非 RIFF
    std::vector<uint8_t> junk(100, 0x41);
    MM_EXPECT_FALSE(wav::readFromMemory(junk).ok());

    // 过短
    std::vector<uint8_t> tiny{'R', 'I', 'F', 'F'};
    MM_EXPECT_FALSE(wav::readFromMemory(tiny).ok());

    // 不存在的路径
    MM_EXPECT_FALSE(wav::read(mmtest::outPath("wav/not-exist.wav")).ok());
    MM_EXPECT_FALSE(wav::probe(mmtest::outPath("wav/not-exist.wav")).ok());

    // 非法采样率
    AudioBuffer bad;
    bad.sampleRate = 0;
    bad.samples.assign(100, 0.0f);
    MM_EXPECT_FALSE(wav::write(mmtest::outPath("wav/bad.wav"), bad).ok());
    MM_EXPECT_FALSE(wav::write(mmtest::outPath("wav/bad.wav"), bad, 24).ok());
}

MM_TEST(audio_wav, 质量报告指标合理) {
    AudioBuffer buf = mmtest::makeTone(1000.0, 400, 0.9, 16000);
    for (size_t i = 0; i < 1600; ++i) buf.samples[i] = 0.0f;      // 前 100 ms 静音
    for (size_t i = 1600; i < 3200; ++i) buf.samples[i] = 1.5f;   // 再 100 ms 人为削波

    AudioQualityReport r;
    WavInfo info;
    info.sampleRate = buf.sampleRate;
    info.channels = 1;
    info.bitsPerSample = 16;
    info.encodingName = "PCM16";
    r = wav::analyzeQuality(buf, info);

    MM_EXPECT_GT(r.peakDbfs, -1.0);
    MM_EXPECT_GT(r.clippingRatio, 0.1);
    MM_EXPECT_GT(r.nearSilentMs, 0);
    MM_EXPECT_GT(r.durationMs, 0);
    MM_EXPECT_FALSE(r.toSummary().empty());
    MM_EXPECT_FALSE(r.toJson().dump().empty());
}

// ============================ 重采样 ============================

MM_TEST(audio_resample, 相同采样率直接复制) {
    AudioBuffer src = mmtest::makeTone(500.0, 200, 0.4, 16000);
    Result<AudioBuffer> out = audio::Resampler::resample(src, 16000);
    MM_REQUIRE_TRUE(out.ok());
    MM_EXPECT_EQ(out.value().samples.size(), src.samples.size());
    MM_EXPECT_NEAR(out.value().samples[500], src.samples[500], 1e-9);
}

MM_TEST(audio_resample, 降采样长度与频率保持) {
    const int srcRate = 48000;
    const double freq = 1000.0;
    AudioBuffer src = mmtest::makeTone(freq, 1000, 0.5, srcRate);

    audio::ResampleStats stats;
    Result<AudioBuffer> out = audio::Resampler::resample(src, 16000, 2, &stats);
    MM_REQUIRE_TRUE(out.ok());
    MM_EXPECT_EQ(out.value().sampleRate, 16000);
    MM_EXPECT_NEAR(static_cast<double>(out.value().samples.size()), 16000.0, 40.0);
    MM_EXPECT_GT(stats.filterTaps, 0);
    MM_EXPECT_NEAR(stats.cutoffHz, 7200.0, 200.0);  // 0.45 × 16000

    // 通带内 1 kHz 分量应被保留：主频仍出现在 1 kHz 附近
    const std::vector<float> mag = dsp::magnitudeSpectrum(out.value().samples);
    size_t peakBin = 0;
    float peak = 0.0f;
    for (size_t i = 1; i + 1 < mag.size(); ++i) {
        if (mag[i] > peak) {
            peak = mag[i];
            peakBin = i;
        }
    }
    const double binHz = 16000.0 / static_cast<double>(out.value().samples.size());
    MM_EXPECT_NEAR(static_cast<double>(peakBin) * binHz, freq, 40.0);
}

MM_TEST(audio_resample, 升采样长度正确) {
    AudioBuffer src = mmtest::makeTone(300.0, 500, 0.4, 8000);
    Result<AudioBuffer> out = audio::Resampler::resample(src, 16000);
    MM_REQUIRE_TRUE(out.ok());
    MM_EXPECT_EQ(out.value().sampleRate, 16000);
    MM_EXPECT_NEAR(static_cast<double>(out.value().samples.size()), 8000.0, 30.0);
    MM_EXPECT_NEAR(static_cast<double>(out.value().durationMs()), 500.0, 5.0);
}

MM_TEST(audio_resample, 非法参数被拒绝) {
    AudioBuffer src = mmtest::makeTone(500.0, 100, 0.3, 16000);
    MM_EXPECT_FALSE(audio::Resampler::resample(src, 0).ok());
    MM_EXPECT_FALSE(audio::Resampler::resample(src, -8000).ok());

    AudioBuffer empty;
    empty.sampleRate = 16000;
    Result<AudioBuffer> out = audio::Resampler::resample(empty, 8000);
    MM_EXPECT_TRUE(out.ok());
    MM_EXPECT_EQ(out.value().samples.size(), static_cast<size_t>(0));
}

// ============================ FFT ============================

MM_TEST(audio_fft, 幂次判断与进位) {
    MM_EXPECT_TRUE(dsp::Fft::isPowerOfTwo(1));
    MM_EXPECT_TRUE(dsp::Fft::isPowerOfTwo(1024));
    MM_EXPECT_FALSE(dsp::Fft::isPowerOfTwo(0));
    MM_EXPECT_FALSE(dsp::Fft::isPowerOfTwo(1000));
    MM_EXPECT_EQ(dsp::Fft::nextPowerOfTwo(1), static_cast<size_t>(1));
    MM_EXPECT_EQ(dsp::Fft::nextPowerOfTwo(1000), static_cast<size_t>(1024));
    MM_EXPECT_EQ(dsp::Fft::nextPowerOfTwo(1024), static_cast<size_t>(1024));
}

MM_TEST(audio_fft, 正逆变换可还原) {
    std::vector<dsp::Complex> data(256);
    mmtest::Rng rng(42u);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = dsp::Complex(static_cast<float>(rng.uniform()), static_cast<float>(rng.uniform()));
    }
    const std::vector<dsp::Complex> original = data;
    dsp::Fft::forward(data);
    dsp::Fft::inverse(data);
    for (size_t i = 0; i < data.size(); ++i) {
        MM_EXPECT_NEAR(data[i].real(), original[i].real(), 1e-3);
        MM_EXPECT_NEAR(data[i].imag(), original[i].imag(), 1e-3);
    }
}

MM_TEST(audio_fft, 单音信号谱峰位置正确) {
    const int n = 1024;
    const int sr = 16000;
    std::vector<float> frame(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        frame[static_cast<size_t>(i)] =
            static_cast<float>(std::sin(2.0 * mmtest::kPi * 1000.0 * i / sr));
    }
    const std::vector<float> mag = dsp::magnitudeSpectrum(frame);
    size_t peakBin = 0;
    float peak = 0.0f;
    for (size_t i = 1; i < mag.size(); ++i) {
        if (mag[i] > peak) {
            peak = mag[i];
            peakBin = i;
        }
    }
    MM_EXPECT_EQ(mag.size(), static_cast<size_t>(n / 2 + 1));
    MM_EXPECT_NEAR(static_cast<double>(peakBin), 64.0, 1.0);  // 1000 Hz / (16000/1024)
}

MM_TEST(audio_fft, 冲激响应谱平坦) {
    std::vector<float> impulse(128, 0.0f);
    impulse[0] = 1.0f;
    const std::vector<float> mag = dsp::magnitudeSpectrum(impulse);
    for (size_t i = 1; i < mag.size(); ++i) {
        MM_EXPECT_NEAR(mag[i], 1.0, 1e-4);
    }
}

// ============================ Mel / MFCC ============================

MM_TEST(audio_mel, 滤波器组结构合法) {
    dsp::MfccConfig cfg;
    cfg.sampleRate = 16000;
    cfg.numFilters = 24;
    dsp::MelFilterBank bank(cfg);
    MM_EXPECT_EQ(bank.filterCount(), static_cast<size_t>(24));

    double prev = -1.0;
    for (size_t i = 0; i < bank.filterCount(); ++i) {
        const double c = bank.centerFrequencyHz(i);
        MM_EXPECT_GT(c, prev);
        prev = c;
    }
    MM_EXPECT_GT(bank.centerFrequencyHz(0), 0.0);
    MM_EXPECT_LT(bank.centerFrequencyHz(23), 8000.0);

    // Mel 换算单调且在已知点接近理论值
    MM_EXPECT_NEAR(dsp::MelFilterBank::hzToMel(0.0), 0.0, 1e-6);
    MM_EXPECT_NEAR(dsp::MelFilterBank::hzToMel(1000.0), 999.99, 1.0);
    MM_EXPECT_NEAR(dsp::MelFilterBank::melToHz(dsp::MelFilterBank::hzToMel(4000.0)), 4000.0, 1e-3);
}

MM_TEST(audio_mel, MFCC维度与帧数正确) {
    dsp::MfccConfig cfg;
    cfg.sampleRate = 16000;
    cfg.numCoeffs = 13;
    dsp::MfccExtractor ex(cfg);

    MM_EXPECT_EQ(ex.frameLengthSamples(), 400);
    MM_EXPECT_EQ(ex.frameShiftSamples(), 160);

    AudioBuffer audio = mmtest::makeTone(500.0, 1000, 0.4, 16000);
    const auto frames = ex.compute(audio);
    MM_EXPECT_GT(frames.size(), static_cast<size_t>(50));
    MM_EXPECT_EQ(frames.front().size(), static_cast<size_t>(13));

    // 帧数公式：(N - frameLen) / shift + 1
    const int expected = (static_cast<int>(audio.samples.size()) - 400) / 160 + 1;
    MM_EXPECT_EQ(static_cast<int>(frames.size()), expected);
}

MM_TEST(audio_mel, 不同频率的特征可区分) {
    dsp::MfccConfig cfg;
    dsp::MfccExtractor ex(cfg);
    const auto a = ex.compute(mmtest::makeTone(300.0, 600, 0.4, 16000));
    const auto b = ex.compute(mmtest::makeTone(3000.0, 600, 0.4, 16000));
    MM_REQUIRE_TRUE(!a.empty() && !b.empty());

    // 同一频率的两段应高度相似，不同频率应差异明显
    const auto c = ex.compute(mmtest::makeTone(300.0, 600, 0.4, 16000));
    auto dist = [](const std::vector<float>& x, const std::vector<float>& y) {
        double s = 0.0;
        const size_t n = std::min(x.size(), y.size());
        for (size_t i = 0; i < n; ++i) s += std::fabs(x[i] - y[i]);
        return s / static_cast<double>(n);
    };
    MM_EXPECT_LT(dist(a.front(), c.front()), dist(a.front(), b.front()));
}

MM_TEST(audio_mel, 差分与均值归一化) {
    std::vector<std::vector<float>> f = {{1.0f, 2.0f}, {2.0f, 4.0f}, {3.0f, 6.0f}};
    const auto d = dsp::MfccExtractor::delta(f, 1);
    MM_EXPECT_EQ(d.size(), static_cast<size_t>(3));
    MM_EXPECT_EQ(d.front().size(), static_cast<size_t>(2));
    // 标准差分公式：Δ(t) = (c(t+1) - c(t-1)) / 2（边界处退化为单侧）
    MM_EXPECT_NEAR(d[1][0], 1.0, 1e-5);
    MM_EXPECT_NEAR(d[0][0], 0.5, 1e-5);
    MM_EXPECT_NEAR(d[2][0], 0.5, 1e-5);
    MM_EXPECT_NEAR(d[1][1], 2.0, 1e-5);

    dsp::MfccExtractor::meanNormalize(f);
    MM_EXPECT_NEAR(f[0][0] + f[1][0] + f[2][0], 0.0, 1e-5);
}

// ============================ VAD ============================

MM_TEST(audio_vad, 交替语音静音可正确分段) {
    AudioBuffer clip = mmtest::makeAlternatingClip(1000, 800, 4, 300, 300);
    audio::VadConfig cfg;
    audio::VoiceActivityDetector vad(cfg);
    Result<audio::VadResult> r = vad.process(clip);
    MM_REQUIRE_TRUE(r.ok());

    const auto& segs = r.value().segments;
    MM_EXPECT_GE(segs.size(), static_cast<size_t>(3));
    MM_EXPECT_LE(segs.size(), static_cast<size_t>(5));
    MM_EXPECT_GT(r.value().speechRatio, 0.3);
    MM_EXPECT_LT(r.value().speechRatio, 0.9);
    MM_EXPECT_GT(r.value().thresholdDb, r.value().noiseFloorDb);

    for (const SpeechSegment& s : segs) {
        MM_EXPECT_GE(s.startMs, static_cast<int64_t>(0));
        MM_EXPECT_LE(s.endMs, clip.durationMs());
        MM_EXPECT_GE(s.durationMs(), static_cast<int64_t>(cfg.minSpeechMs));
    }
    // 时间戳精度：首段起点应落在 300 ms 附近（领先静音之后）
    MM_EXPECT_NEAR(static_cast<double>(segs.front().startMs), 300.0, 60.0);

    // 帧级指标
    MM_EXPECT_GT(r.value().frames.size(), static_cast<size_t>(10));
    MM_EXPECT_LT(r.value().frames.front().rmsDb, -60.0f);
}

MM_TEST(audio_vad, 纯静音退化为整段) {
    AudioBuffer silent = mmtest::makeSilence(2000, 16000);
    audio::VoiceActivityDetector vad;
    Result<audio::VadResult> r = vad.process(silent);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(r.value().segments.size(), static_cast<size_t>(1));
    MM_EXPECT_NEAR(static_cast<double>(r.value().segments.front().durationMs()), 2000.0, 40.0);
}

MM_TEST(audio_vad, 稳态噪声不产生语音段) {
    AudioBuffer noise = mmtest::makeNoise(2000, 0.01, 16000, 7u);
    audio::VoiceActivityDetector vad;
    Result<audio::VadResult> r = vad.process(noise);
    MM_REQUIRE_TRUE(r.ok());
    // 噪声底与信号电平接近 → 无稳定语音段 → 退化为整段（1 段）
    MM_EXPECT_EQ(r.value().segments.size(), static_cast<size_t>(1));
}

MM_TEST(audio_vad, 空音频与非法采样率报错) {
    AudioBuffer empty;
    empty.sampleRate = 16000;
    audio::VoiceActivityDetector vad;
    MM_EXPECT_FALSE(vad.process(empty).ok());

    AudioBuffer bad;
    bad.sampleRate = 0;
    bad.samples.assign(16000, 0.1f);
    MM_EXPECT_FALSE(vad.process(bad).ok());
}

MM_TEST(audio_vad, 固定门限模式生效) {
    AudioBuffer clip = mmtest::makeAlternatingClip(800, 600, 3, 200, 200);
    audio::VadConfig cfg;
    cfg.thresholdDeltaDb = 12.0;
    audio::VoiceActivityDetector vad(cfg);
    Result<audio::VadResult> r = vad.process(clip);
    MM_REQUIRE_TRUE(r.ok());
    MM_EXPECT_EQ(static_cast<int>(r.value().thresholdDb),
                 static_cast<int>(r.value().noiseFloorDb + 12.0));
}

// ============================ 分段器 ============================

MM_TEST(audio_segmenter, 补白与切片) {
    AudioBuffer clip = mmtest::makeAlternatingClip(1000, 800, 2, 400, 400);
    audio::SpeechSegmenter seg(audio::SegmenterConfig{200, 30000, 100}, clip.sampleRate);

    SpeechSegment s;
    s.startMs = 1000;
    s.endMs = 2000;
    const AudioBuffer sliced = seg.slice(clip, s);
    MM_EXPECT_EQ(sliced.sampleRate, clip.sampleRate);
    MM_EXPECT_NEAR(static_cast<double>(sliced.durationMs()), 1400.0, 20.0);  // 1000 + 2×200

    // 边界裁剪：从 0 开始的段不应产生负起点
    SpeechSegment s0;
    s0.startMs = 0;
    s0.endMs = 500;
    const AudioBuffer sliced0 = seg.slice(clip, s0);
    MM_EXPECT_NEAR(static_cast<double>(sliced0.durationMs()), 700.0, 20.0);
}

MM_TEST(audio_segmenter, 过长段按能量最低点切分) {
    // 构造 3 秒语音，中间含 300 ms 低能量"呼吸"间隙
    AudioBuffer clip = mmtest::makeSilence(200, 16000);
    mmtest::appendSpeechLike(clip, 1400, 0.25, 11u);
    mmtest::appendSpeechLike(clip, 300, 0.02, 12u);  // 低能量
    mmtest::appendSpeechLike(clip, 1400, 0.25, 13u);
    mmtest::appendSilence(clip, 200);

    audio::SegmenterConfig cfg;
    cfg.padMs = 0;
    cfg.maxSegmentMs = 1500;
    cfg.minSegmentMs = 100;
    audio::SpeechSegmenter sg(cfg, clip.sampleRate);

    std::vector<SpeechSegment> raw;
    SpeechSegment whole;
    whole.startMs = 200;
    whole.endMs = 3300;
    raw.push_back(whole);

    const auto refined = sg.refine(raw, clip);
    MM_EXPECT_GE(refined.size(), static_cast<size_t>(2));
    for (const SpeechSegment& s : refined) {
        MM_EXPECT_LE(s.durationMs(), static_cast<int64_t>(cfg.maxSegmentMs + 600));
        MM_EXPECT_GT(s.energyDb, -100.0f);
    }
}

MM_TEST(audio_segmenter, 过短段被剔除) {
    AudioBuffer clip = mmtest::makeSilence(1000, 16000);
    audio::SegmenterConfig cfg;
    cfg.minSegmentMs = 300;
    audio::SpeechSegmenter sg(cfg, clip.sampleRate);

    std::vector<SpeechSegment> raw;
    SpeechSegment s;
    s.startMs = 100;
    s.endMs = 200;  // 仅 100 ms
    raw.push_back(s);
    MM_EXPECT_EQ(sg.refine(raw, clip).size(), static_cast<size_t>(0));
}

// ============================ 转写单元分组 ============================

MM_TEST(audio_group, 相邻短段合并为单个单元) {
    std::vector<SpeechSegment> segs;
    for (int i = 0; i < 5; ++i) {
        SpeechSegment s;
        s.startMs = i * 2000;
        s.endMs = i * 2000 + 1200;
        segs.push_back(s);
    }
    audio::GroupConfig cfg;
    cfg.maxUnitMs = 25000;
    cfg.mergeGapMs = 1500;
    const auto units = audio::groupSegments(segs, cfg);
    MM_EXPECT_EQ(units.size(), static_cast<size_t>(1));
    MM_EXPECT_EQ(units.front().size(), static_cast<size_t>(5));
    MM_EXPECT_EQ(units.front().startMs, static_cast<int64_t>(0));
    MM_EXPECT_EQ(units.front().endMs, static_cast<int64_t>(4 * 2000 + 1200));
    MM_EXPECT_EQ(units.front().durationMs(), units.front().endMs - units.front().startMs);
}

MM_TEST(audio_group, 大间隔切分为多个单元) {
    std::vector<SpeechSegment> segs;
    for (int i = 0; i < 4; ++i) {
        SpeechSegment s;
        s.startMs = i * 10000;
        s.endMs = i * 10000 + 1000;
        segs.push_back(s);   // 间隔 9 秒
    }
    audio::GroupConfig cfg;
    cfg.mergeGapMs = 1500;
    cfg.maxUnitMs = 25000;
    const auto units = audio::groupSegments(segs, cfg);
    MM_EXPECT_EQ(units.size(), static_cast<size_t>(4));
    for (const auto& u : units) MM_EXPECT_EQ(u.size(), static_cast<size_t>(1));
}

MM_TEST(audio_group, 受单元时长上限约束) {
    std::vector<SpeechSegment> segs;
    for (int i = 0; i < 20; ++i) {
        SpeechSegment s;
        s.startMs = i * 1500;
        s.endMs = i * 1500 + 1200;   // 段间隔 300 ms，总长 30 秒
        segs.push_back(s);
    }
    audio::GroupConfig cfg;
    cfg.mergeGapMs = 1000;
    cfg.maxUnitMs = 10000;
    const auto units = audio::groupSegments(segs, cfg);
    MM_EXPECT_GT(units.size(), static_cast<size_t>(1));
    size_t members = 0;
    for (const auto& u : units) {
        MM_EXPECT_LE(u.durationMs(), static_cast<int64_t>(cfg.maxUnitMs + 2000));
        MM_EXPECT_GT(u.size(), static_cast<size_t>(0));
        members += u.size();
    }
    MM_EXPECT_EQ(members, segs.size());   // 不丢段、不重复
}

MM_TEST(audio_group, 边界输入安全) {
    MM_EXPECT_EQ(audio::groupSegments({}, audio::GroupConfig{}).size(), static_cast<size_t>(0));

    std::vector<SpeechSegment> one(1);
    one[0].startMs = 500;
    one[0].endMs = 900;
    const auto units = audio::groupSegments(one, audio::GroupConfig{});
    MM_REQUIRE_TRUE(units.size() == 1);
    MM_EXPECT_EQ(units.front().members.front(), static_cast<size_t>(0));
}

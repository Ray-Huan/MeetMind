// MeetMind — 实时（流式）转写
// 数据流：音频块 → 在线 VAD → 闭合语音段 → 推理工作线程 → 回调输出
//
// 关键设计（决定它能不能真正「实时」）：
//   1) push() 只做缓冲与 VAD，绝不在采集线程里跑推理——否则麦克风回调被阻塞会丢帧；
//   2) 推理串行化在单个工作线程，天然保证「出稿顺序 = 音频顺序」；
//   3) 段长超过 maxSegmentMs 时强制切段，给时延一个上界：即使连续讲话，也能按
//      固定节奏持续出稿，而不是等一句话说完；
//   4) 保留一段滚动音频窗口，用于给闭合段切片补白（避免切掉字头字尾）。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mm/asr/iasr_engine.h"
#include "mm/live/online_vad.h"
#include "mm/nlp/punctuation_restorer.h"
#include "mm/nlp/t2s.h"
#include "mm/nlp/text_normalizer.h"

namespace mm::live {

struct RealtimeConfig {
    OnlineVadConfig vad;
    /// 单段时长上限；超过则强制切段（控制时延上界，连续讲话时也能持续出稿）
    int64_t maxSegmentMs = 12000;
    /// 切片时前后补白（毫秒）
    int64_t padMs = 200;

    // ---- 文本后处理 ----
    // 必须与离线流水线保持同一顺序：繁转简 → ITN → 标点恢复。
    // 顺序不可调换：ITN 的数词规则基于简体字表。
    // 若不做这一步，实时输出会是繁体且无标点，与事后导出的纪要文字不一致。
    bool enableT2s = true;
    bool enableItn = true;
    bool enablePunctuation = true;
    /// 实时场景下每个闭合段都按「句末」处理，用它作为「到下一段的间隔」估算值
    int64_t punctuationGapMs = 1000;
};

/// 一次实时产出。
struct RealtimeEvent {
    asr::AsrSegment segment;
    /// 从「该段音频收齐」到「出稿」的耗时（含排队与推理）
    int64_t latencyMs = 0;
    /// 出稿时已消费的音频时长
    int64_t audioMs = 0;
};

struct RealtimeStats {
    int64_t audioMs = 0;         ///< 已消费音频
    int64_t transcribeMs = 0;    ///< 累计推理耗时
    int64_t segments = 0;        ///< 已产出段数
    int64_t chars = 0;           ///< 已产出字符数（按 UTF-8 字节计）
    double avgLatencyMs = 0.0;   ///< 平均时延
    double maxLatencyMs = 0.0;   ///< 峰值时延
    double realTimeFactor = 0.0; ///< audioMs / transcribeMs，>1 表示能跟上实时
};

class RealtimeTranscriber {
public:
    using Callback = std::function<void(const RealtimeEvent&)>;

    RealtimeTranscriber(asr::IAsrEngine* engine, RealtimeConfig cfg = RealtimeConfig{});
    ~RealtimeTranscriber();

    RealtimeTranscriber(const RealtimeTranscriber&) = delete;
    RealtimeTranscriber& operator=(const RealtimeTranscriber&) = delete;

    void setCallback(Callback cb);

    /// 启动工作线程（须在 push 之前调用）。
    Result<void> start();
    /// 喂入 16 kHz 单声道浮点音频（线程安全，可在采集线程调用）。
    void push(const float* samples, size_t count);
    /// 冲刷：强制闭合尾部语音段并等待队列推理完成。
    void flush();
    /// 停止工作线程（可重复调用）。
    void stop();

    RealtimeStats stats() const;
    /// 是否正处在语音段中（供界面显示「正在说话」）。
    bool inSpeech() const;

private:
    struct Job {
        AudioBuffer audio;       ///< 已补白、可直接送 ASR 的音频
        int64_t startMs = 0;     ///< 该切片在流中的起始时间
        int64_t submittedWall = 0;  ///< 入队时刻（steady_clock 毫秒）
    };

    void workerLoop();
    /// 处理单个任务（workerLoop 与 drainInline 共用）。
    void processJob(Job& job);
    /// 就地消费剩余队列：供「从工作线程内调用 flush()」时使用，
    /// 否则工作线程会等待自己而永久阻塞。
    void drainInline();
    void submit(const SpeechSegment& seg);
    /// 与离线流水线同序的文本后处理：繁转简 → ITN → 标点。
    std::string postProcess(const std::string& text) const;

    asr::IAsrEngine* engine_ = nullptr;
    RealtimeConfig cfg_;
    Callback cb_;
    nlp::TextNormalizer itn_;              ///< 无状态，可跨线程共用
    nlp::PunctuationRestorer punct_;       ///< 无状态，可跨线程共用

    // --- 采集侧状态（在 push 的调用线程内）---
    std::vector<float> streamBuf_;   ///< 滚动音频窗口
    size_t streamBase_ = 0;          ///< streamBuf_ 起点对应的流内样本序号
    OnlineVad vad_;

    // --- 跨线程队列 ---
    mutable std::mutex mu_;
    std::condition_variable cv_;       ///< 队列非空 / 停止信号
    std::condition_variable doneCv_;   ///< 队列与在途任务均已处理完
    std::deque<Job> queue_;
    int pending_ = 0;                  ///< 队列中 + 正在推理的任务数
    bool started_ = false;
    bool stop_ = false;

    // --- 统计 ---
    std::atomic<int64_t> transcribeMs_{0};
    std::atomic<int64_t> segments_{0};
    std::atomic<int64_t> chars_{0};
    std::atomic<int64_t> latencySum_{0};
    std::atomic<int64_t> latencyMax_{0};
    std::atomic<int64_t> audioMs_{0};
    std::atomic<bool> inSpeech_{false};

    std::thread worker_;
};

}  // namespace mm::live

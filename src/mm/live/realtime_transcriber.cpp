#include "mm/live/realtime_transcriber.h"

#include <algorithm>
#include <chrono>

#include "mm/audio/audio_types.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"

namespace mm::live {
namespace {

constexpr int kSampleRate = 16000;

/// whisper 对纯噪声/静音会输出占位标记（如 "[BLANK_AUDIO]"、" [ Silence ]"）。
/// 实时链路按句出稿，这类标记对用户是纯噪声，直接丢弃。
bool isBlankMarker(const std::string& text) {
    if (text.size() < 3 || text.size() > 32) return false;
    std::string letters;
    letters.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) letters.push_back(static_cast<char>(std::tolower(u)));
    }
    return letters == "blankaudio" || letters == "silence" || letters == "inaudible" ||
           letters == "nospeech";
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

RealtimeTranscriber::RealtimeTranscriber(asr::IAsrEngine* engine, RealtimeConfig cfg)
    : engine_(engine), cfg_(cfg), vad_(cfg.vad) {
    if (cfg_.maxSegmentMs <= 0) cfg_.maxSegmentMs = 12000;
    if (cfg_.padMs < 0) cfg_.padMs = 0;
    streamBuf_.reserve(static_cast<size_t>(cfg_.maxSegmentMs + 6000) * kSampleRate / 1000);
}

RealtimeTranscriber::~RealtimeTranscriber() { stop(); }

void RealtimeTranscriber::setCallback(Callback cb) { cb_ = std::move(cb); }

Result<void> RealtimeTranscriber::start() {
    if (engine_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "未提供 ASR 引擎");
    }
    if (!engine_->ready()) {
        return fail(ErrorCode::ModelNotLoaded, "ASR 引擎未就绪，请先初始化模型");
    }
    if (started_) {
        return fail(ErrorCode::InvalidArgument, "实时转写器已启动");
    }
    stop_ = false;
    started_ = true;
    worker_ = std::thread(&RealtimeTranscriber::workerLoop, this);
    return okStatus();
}

void RealtimeTranscriber::push(const float* samples, size_t count) {
    if (samples == nullptr || count == 0) return;

    streamBuf_.insert(streamBuf_.end(), samples, samples + count);

    // 1) 在线 VAD 增量判决，闭合的段立即入队
    for (const SpeechSegment& seg : vad_.feed(samples, count)) {
        submit(seg);
    }

    // 2) 超长段强制切分：给时延一个上界（连续讲话也能按固定节奏出稿）
    if (vad_.inSpeech() && vad_.currentSpeechMs() > cfg_.maxSegmentMs) {
        SpeechSegment forced;
        if (vad_.forceClose(&forced)) submit(forced);
    }

    inSpeech_.store(vad_.inSpeech(), std::memory_order_relaxed);
    audioMs_.store(vad_.elapsedMs(), std::memory_order_relaxed);

    // 3) 滚动窗口压缩：只保留足够覆盖「最长段 + 补白」的音频
    const size_t keep =
        static_cast<size_t>(cfg_.maxSegmentMs + 3000) * kSampleRate / 1000;
    const size_t slack = static_cast<size_t>(kSampleRate);   // 1 秒
    if (streamBuf_.size() > keep + slack) {
        const size_t drop = streamBuf_.size() - keep;
        streamBuf_.erase(streamBuf_.begin(),
                         streamBuf_.begin() + static_cast<std::ptrdiff_t>(drop));
        streamBase_ += drop;
    }
}

void RealtimeTranscriber::submit(const SpeechSegment& seg) {
    if (seg.durationMs() <= 0) return;

    int64_t startMs = seg.startMs - cfg_.padMs;
    int64_t endMs = seg.endMs + cfg_.padMs;
    if (startMs < 0) startMs = 0;

    size_t startSample = static_cast<size_t>(startMs) * kSampleRate / 1000;
    size_t endSample = static_cast<size_t>(endMs) * kSampleRate / 1000;

    if (startSample < streamBase_) startSample = streamBase_;
    const size_t avail = streamBase_ + streamBuf_.size();
    if (endSample > avail) endSample = avail;
    if (endSample <= startSample) return;

    Job job;
    job.startMs = static_cast<int64_t>(startSample) * 1000 / kSampleRate;
    job.audio.sampleRate = kSampleRate;
    job.audio.samples.assign(
        streamBuf_.begin() + static_cast<std::ptrdiff_t>(startSample - streamBase_),
        streamBuf_.begin() + static_cast<std::ptrdiff_t>(endSample - streamBase_));
    job.submittedWall = nowMs();

    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(job));
        ++pending_;
    }
    cv_.notify_one();
}

void RealtimeTranscriber::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        processJob(job);

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pending_ > 0) --pending_;
        }
        doneCv_.notify_all();
    }
}

void RealtimeTranscriber::processJob(Job& job) {
    const int64_t t0 = nowMs();
    Result<asr::AsrSegment> result = engine_->transcribe(job.audio, job.startMs, nullptr);
    const int64_t inferMs = nowMs() - t0;
    transcribeMs_.fetch_add(inferMs, std::memory_order_relaxed);

    if (!result.ok()) {
        MM_LOG_WARN("live.asr") << "实时转写失败: " << result.message();
        return;
    }
    const asr::AsrSegment& seg = result.value();
    std::string text = str::trim(seg.text);
    if (isBlankMarker(text)) text.clear();
    // 与离线流水线同序的规范化（繁转简 → ITN → 标点），保证实时文字与事后导出一致
    if (!text.empty()) text = postProcess(text);

    const int64_t latency = nowMs() - job.submittedWall;
    latencySum_.fetch_add(latency, std::memory_order_relaxed);
    int64_t prev = latencyMax_.load(std::memory_order_relaxed);
    while (latency > prev &&
           !latencyMax_.compare_exchange_weak(prev, latency, std::memory_order_relaxed)) {
    }

    if (text.empty()) {
        MM_LOG_DEBUG("live.asr") << "段 " << job.startMs << "ms 无文本（噪声或低置信）";
        return;
    }

    segments_.fetch_add(1, std::memory_order_relaxed);
    chars_.fetch_add(static_cast<int64_t>(text.size()), std::memory_order_relaxed);

    if (cb_) {
        RealtimeEvent ev;
        ev.segment = seg;
        ev.segment.text = text;
        ev.latencyMs = latency;
        ev.audioMs = audioMs_.load(std::memory_order_relaxed);
        cb_(ev);
    }
}

void RealtimeTranscriber::drainInline() {
    for (;;) {
        Job job;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        processJob(job);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pending_ > 0) --pending_;
        }
        doneCv_.notify_all();
    }
}

void RealtimeTranscriber::flush() {
    SpeechSegment tail;
    if (vad_.forceClose(&tail)) submit(tail);
    inSpeech_.store(false, std::memory_order_relaxed);

    // 若当前就在工作线程内（例如转写器由工作线程持有并调用 flush）：
    // 工作线程正被这条语句占用，没有任何人会去消费队列，等条件变量必然死锁。
    // 此时改为就地处理剩余任务。
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
        drainInline();
        return;
    }
    std::unique_lock<std::mutex> lk(mu_);
    doneCv_.wait(lk, [this] { return pending_ == 0; });
}

void RealtimeTranscriber::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            // 从工作线程自身调用：join 自己会抛 system_error，且析构时若仍
            // joinable 会直接 terminate —— 只能 detach，让线程自然退出。
            worker_.detach();
        } else {
            worker_.join();
        }
    }
    started_ = false;
}

std::string RealtimeTranscriber::postProcess(const std::string& text) const {
    std::string out = text;
    // 顺序与离线流水线一致：繁转简 → ITN → 标点恢复
    if (cfg_.enableT2s) {
        const nlp::TraditionalConverter& t2s = nlp::sharedTraditionalConverter();
        if (t2s.needsConversion(out)) out = t2s.convert(out);
    }
    if (cfg_.enableItn) {
        out = itn_.normalize(out);
    }
    if (cfg_.enablePunctuation) {
        nlp::PunctuationContext ctx;
        ctx.gapToNextMs = cfg_.punctuationGapMs;
        ctx.isLast = false;
        out = punct_.restore(out, ctx);
    }
    return out;
}

RealtimeStats RealtimeTranscriber::stats() const {
    RealtimeStats s;
    s.audioMs = audioMs_.load(std::memory_order_relaxed);
    s.transcribeMs = transcribeMs_.load(std::memory_order_relaxed);
    s.segments = segments_.load(std::memory_order_relaxed);
    s.chars = chars_.load(std::memory_order_relaxed);
    const int64_t done = latencySum_.load(std::memory_order_relaxed);
    if (s.segments > 0) {
        s.avgLatencyMs = static_cast<double>(done) / static_cast<double>(s.segments);
    }
    s.maxLatencyMs = static_cast<double>(latencyMax_.load(std::memory_order_relaxed));
    if (s.transcribeMs > 0) {
        s.realTimeFactor = static_cast<double>(s.audioMs) / static_cast<double>(s.transcribeMs);
    }
    return s;
}

bool RealtimeTranscriber::inSpeech() const {
    return inSpeech_.load(std::memory_order_relaxed);
}

}  // namespace mm::live

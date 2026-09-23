// MeetMind — 流水线编排
// 固定阶段序列：解码 → 重采样 → VAD → 转写 → 说话人分离 → 规范化 → 纪要 → 导出
// 设计要点：
//   * 任一段失败不会丢弃其余结果（保留占位段，保证「段-结果」索引对齐）
//   * 每个阶段边界检查取消令牌
//   * 每阶段计时写入报告，用于性能验收
#pragma once

#include <string>
#include <vector>

#include "mm/asr/asr_factory.h"
#include "mm/asr/iasr_engine.h"
#include "mm/common/config.h"
#include "mm/common/result.h"
#include "mm/nlp/tokenizer.h"
#include "mm/pipeline/exporter.h"
#include "mm/pipeline/pipeline_types.h"

namespace mm::pipeline {

class Pipeline {
public:
    explicit Pipeline(Config config);
    ~Pipeline();

    /// 执行完整流水线。
    /// @param cancel      可空；非空时可在任意阶段边界取消
    /// @param onProgress  可空；进度回调（同一线程内同步调用）
    Result<PipelineResult> run(CancelToken* cancel = nullptr,
                               const ProgressCallback& onProgress = nullptr);

    /// 导入人工审核后的结果 JSON，重新生成句子/纪要并导出。
    /// reviewedJson 由审核网页（tools/gen_review_page.py）导出，格式与原 result.json
    /// 一致，仅 asr.segments 的 text/speakerId 与 speakerLabels 可能被修改。
    /// 跳过解码/重采样/VAD/转写/说话人分离，直接走「句子切分 → 纪要 → 导出」。
    Result<PipelineResult> importReviewed(const std::string& reviewedJsonPath,
                                          CancelToken* cancel = nullptr,
                                          const ProgressCallback& onProgress = nullptr);

    /// 阶段总数（用于进度显示）。
    static int stageCount();

    /// 当前进程常驻内存（KB）；不支持时返回 0。
    static int64_t currentRssKb();

    const Config& config() const { return config_; }
    void setConfig(const Config& c) { config_ = c; }

private:
    void notify(const ProgressCallback& cb, Stage stage, int index, double fraction,
                const std::string& message) const;

    /// 惰性加载分词器（含停用词）。转写阶段（切点吸附词边界）与纪要阶段都要用，
    /// 因此必须在使用之前就绪，不能等到纪要阶段才加载。
    const nlp::Tokenizer& ensureTokenizer();

    Config config_;
    nlp::Tokenizer tokenizer_;
    bool tokenizerReady_ = false;
};

}  // namespace mm::pipeline

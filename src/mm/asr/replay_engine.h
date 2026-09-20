// MeetMind — 回放式识别引擎
// 用途：1) 无模型环境下的确定性回归测试；2) 演示模式；3) 已知转写文本的纪要复算。
// 按调用顺序依次吐出脚本中的文本行，时间戳取自实际音频块，因此下游链路行为与真实后端一致。
#pragma once

#include <string>
#include <vector>

#include "mm/asr/iasr_engine.h"

namespace mm::asr {

class ReplayAsrEngine final : public IAsrEngine {
public:
    ReplayAsrEngine() = default;

    /// 直接设置脚本文本（每元素为一段文本；空串表示该段无语音）。
    void setScript(std::vector<std::string> lines);
    /// 从文件加载脚本（每行一段，'#' 开头为注释，空行表示无语音段）。
    Result<void> loadScript(const std::string& path);

    /// 已消费的脚本行数 / 总行数。
    size_t consumed() const { return cursor_; }
    size_t scriptSize() const { return script_.size(); }

    // ---- IAsrEngine ----
    std::string id() const override { return "replay"; }
    std::string displayName() const override { return "回放引擎（脚本驱动）"; }
    AsrCapabilities capabilities() const override;
    Result<void> initialize(const AsrModelConfig& config) override;
    bool ready() const override { return initialized_; }
    Result<AsrSegment> transcribe(const AudioBuffer& chunk, int64_t chunkStartMs,
                                  const CancelToken* cancel) override;

    /// 脚本耗尽时的行为：true = 从头循环，false = 持续返回空段
    void setLoop(bool loop) { loop_ = loop; }

private:
    std::vector<std::string> script_;
    size_t cursor_ = 0;
    bool initialized_ = false;
    bool loop_ = false;
};

}  // namespace mm::asr

// MeetMind — 结果导出（Markdown / JSON / SRT / TXT / HTML）
// 全部导出采用「临时文件 + 原子替换」，避免中断产生半成品。
#pragma once

#include <string>
#include <vector>

#include "mm/common/config.h"
#include "mm/common/result.h"
#include "mm/pipeline/pipeline_types.h"

namespace mm::pipeline {

enum class ExportFormat { Markdown, Json, Srt, Text, Html };

const char* toString(ExportFormat f) noexcept;
const char* fileExtension(ExportFormat f) noexcept;

struct ExportOptions {
    std::string outputDir;   ///< 空 = 与输入文件同目录
    std::string baseName;    ///< 空 = 由标题与日期推导
    std::vector<ExportFormat> formats;  ///< 空 = 由 Config 决定
};

class Exporter {
public:
    /// 渲染为字符串（便于测试与预览）。
    /// @param plannedFiles 非空时取代 result.exportedFiles 出现在产物中：
    ///                     导出过程必须先确定目标路径再渲染，否则 JSON 里的
    ///                     exportedFiles 永远为空（自引用问题）。
    static Result<std::string> render(const PipelineResult& result, ExportFormat format,
                                      const std::vector<std::string>* plannedFiles = nullptr);

    /// 渲染「人工审核网页」（单文件 HTML，内嵌转写数据 + 引用音频文件名）。
    /// 用于处理完成后一键打开浏览器复核/修改，见 tools/gen_review_page.py（同源模板）。
    static Result<std::string> renderReviewHtml(const PipelineResult& result,
                                                const std::string& audioFileName);

    /// 计算将要写出的文件路径（不落盘）。
    static std::vector<std::string> planPaths(const PipelineResult& result, const Config& config,
                                              const ExportOptions& options);

    /// 导出全部选定格式，返回已写出的文件绝对路径。
    static Result<std::vector<std::string>> exportAll(const PipelineResult& result,
                                                      const Config& config,
                                                      const ExportOptions& options = ExportOptions{});

    /// 由配置解析出需要导出的格式列表。
    static std::vector<ExportFormat> formatsFromConfig(const Config& config);

    /// 推导导出文件主干名（已做文件名净化）。
    static std::string deriveBaseName(const PipelineResult& result, const Config& config);

private:
    static Result<void> writeFileAtomic(const std::string& path, const std::string& content);
};

}  // namespace mm::pipeline

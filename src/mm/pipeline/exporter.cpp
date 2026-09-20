#include "mm/pipeline/exporter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "mm/common/path_utils.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm::pipeline {
namespace fs = std::filesystem;

const char* toString(ExportFormat f) noexcept {
    switch (f) {
        case ExportFormat::Markdown: return "markdown";
        case ExportFormat::Json: return "json";
        case ExportFormat::Srt: return "srt";
        case ExportFormat::Text: return "text";
        case ExportFormat::Html: return "html";
    }
    return "unknown";
}

const char* fileExtension(ExportFormat f) noexcept {
    switch (f) {
        case ExportFormat::Markdown: return ".md";
        case ExportFormat::Json: return ".json";
        case ExportFormat::Srt: return ".srt";
        case ExportFormat::Text: return ".txt";
        case ExportFormat::Html: return ".html";
    }
    return ".dat";
}

namespace {

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

/// Markdown 表格单元格转义（避免 | 破坏表格）。
std::string mdCell(const std::string& s) {
    return str::replaceAll(str::replaceAll(s, "|", "\\|"), "\n", " ");
}

/// 说话人配色（用于 HTML 与终端的视觉区分；不依赖主题）。
const char* speakerColor(int id) {
    static const char* kColors[] = {"#2563eb", "#16a34a", "#d97706", "#dc2626",
                                    "#7c3aed", "#0891b2", "#be185d", "#65a30d"};
    if (id < 0) return "#64748b";
    return kColors[id % 8];
}

std::string renderMarkdown(const PipelineResult& r,
                           const std::vector<std::string>& files) {
    std::string out;
    out += "# " + r.title + "\n\n";

    out += "| 项目 | 内容 |\n| --- | --- |\n";
    out += "| 会议日期 | " + r.meetingDate + " |\n";
    out += "| 音频时长 | " + timeutil::formatDuration(r.quality.durationMs) + " |\n";
    out += "| 有效语音 | " + timeutil::formatDuration(r.minutes.stats.speechDurationMs) + "（" +
           str::numberToString(r.minutes.stats.speechRatio * 100.0, 1) + "%） |\n";
    out += "| 转写字数 | " + std::to_string(r.minutes.stats.cjkCharacters) + " 字 |\n";
    out += "| 说话人数 | " + std::to_string(r.minutes.stats.speakerCount) + " |\n";
    out += "| 识别后端 | " + r.report.asrBackend + " |\n";
    out += "| 源文件 | `" + mdCell(r.inputPath) + "` |\n";
    out += "| 生成时间 | " + timeutil::nowDateTime() + " |\n\n";

    if (!r.minutes.overview.empty()) {
        out += "## 一、会议概览\n\n" + r.minutes.overview + "\n\n";
    }

    if (!r.minutes.topics.empty()) {
        out += "## 二、议题脉络\n\n";
        int idx = 1;
        for (const nlp::Topic& t : r.minutes.topics) {
            out += std::to_string(idx++) + ". **" + t.title + "**";
            if (!t.keywords.empty()) {
                out += "（关键词：" + str::join(t.keywords, "、") + "）";
            }
            if (t.startMs >= 0) {
                out += " — 起始于 " + timeutil::formatDuration(t.startMs);
            }
            out += "\n";
        }
        out += "\n";
    }

    if (!r.minutes.keyPoints.empty()) {
        out += "## 三、关键结论\n\n";
        for (const nlp::SummarySentence& s : r.minutes.keyPoints) {
            out += "- ";
            if (s.startMs >= 0) out += "`" + timeutil::formatDuration(s.startMs) + "` ";
            out += s.text + "\n";
        }
        out += "\n";
    }

    if (!r.minutes.decisions.empty()) {
        out += "## 四、会议决议\n\n";
        for (const nlp::Decision& d : r.minutes.decisions) {
            out += "- [" + d.kind + "] " + d.text;
            if (d.startMs >= 0) out += "（`" + timeutil::formatDuration(d.startMs) + "`）";
            out += "\n";
        }
        out += "\n";
    }

    if (!r.minutes.actionItems.empty()) {
        out += "## 五、待办事项\n\n";
        out += "| # | 任务 | 责任人 | 截止时间 | 优先级 |\n| --- | --- | --- | --- | --- |\n";
        int i = 1;
        for (const nlp::ActionItem& a : r.minutes.actionItems) {
            out += "| " + std::to_string(i++) + " | " + mdCell(a.task) + " | " +
                   (a.owner.empty() ? "待指定" : mdCell(a.owner)) + " | " +
                   (a.dueDate.empty() ? (a.dueText.empty() ? "待定" : mdCell(a.dueText))
                                      : a.dueDate) +
                   " | " + a.priority + " |\n";
        }
        out += "\n";
    }

    if (!r.minutes.keywords.empty()) {
        out += "## 六、关键词\n\n";
        std::vector<std::string> kws;
        for (const nlp::Keyword& k : r.minutes.keywords) {
            kws.push_back(k.word + "(" + std::to_string(k.frequency) + ")");
        }
        out += str::join(kws, "、") + "\n\n";
    }

    if (!r.minutes.risks.empty()) {
        out += "## 七、风险与待确认\n\n";
        for (const std::string& s : r.minutes.risks) out += "- " + s + "\n";
        out += "\n";
    }

    out += "## 八、全文转写\n\n";
    int lastSpeaker = -2;
    for (const asr::AsrSegment& s : r.asr.segments) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        if (s.speakerId != lastSpeaker) {
            out += "\n**" + r.speakerLabel(s.speakerId) + "**\n\n";
            lastSpeaker = s.speakerId;
        }
        out += "`" + timeutil::formatDuration(s.startMs) + "` " + text + "\n\n";
    }

    if (!files.empty()) {
        out += "## 九、本次导出的文件\n\n";
        for (const std::string& f : files) out += "- `" + mdCell(f) + "`\n";
        out += "\n";
    }

    out += "\n---\n\n<sub>由 MeetMind 端侧语音转写与智能会议纪要工具生成 · 全流程离线运行</sub>\n";
    return out;
}

std::string renderSrt(const PipelineResult& r) {
    std::string out;
    int index = 1;
    for (const asr::AsrSegment& s : r.asr.segments) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        out += std::to_string(index++) + "\n";
        out += timeutil::formatSrt(s.startMs) + " --> " + timeutil::formatSrt(s.endMs) + "\n";
        out += "[" + r.speakerLabel(s.speakerId) + "] " + text + "\n\n";
    }
    return out;
}

std::string renderText(const PipelineResult& r) {
    std::string out;
    out += r.title + "\n";
    out += "会议日期: " + r.meetingDate + "    时长: " +
           timeutil::formatDuration(r.quality.durationMs) + "\n";
    out += std::string(60, '=') + "\n\n";
    for (const asr::AsrSegment& s : r.asr.segments) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        out += "[" + timeutil::formatDuration(s.startMs) + "] " + r.speakerLabel(s.speakerId) +
               ": " + text + "\n";
    }
    return out;
}

std::string renderHtml(const PipelineResult& r,
                       const std::vector<std::string>& files) {
    std::string out;
    out += "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>" + htmlEscape(r.title) + "</title>\n<style>\n";
    out += ":root{--fg:#1e293b;--muted:#64748b;--line:#e2e8f0;--bg:#ffffff;--accent:#2563eb;}\n";
    out += "body{margin:0;padding:32px;background:var(--bg);color:var(--fg);"
           "font-family:-apple-system,'Segoe UI','Microsoft YaHei',sans-serif;line-height:1.7;"
           "max-width:960px;margin:0 auto;}\n";
    out += "h1{font-size:26px;margin:0 0 4px;}h2{font-size:19px;margin:32px 0 12px;"
           "border-left:4px solid var(--accent);padding-left:10px;}\n";
    out += "table{border-collapse:collapse;width:100%;font-size:14px;margin:12px 0;}\n";
    out += "th,td{border:1px solid var(--line);padding:8px 10px;text-align:left;}\n";
    out += "th{background:#f8fafc;font-weight:600;}\n";
    out += ".meta{color:var(--muted);font-size:13px;margin-bottom:20px;}\n";
    out += ".seg{display:flex;gap:10px;padding:5px 0;align-items:baseline;}\n";
    out += ".time{color:var(--muted);font-size:12px;font-variant-numeric:tabular-nums;"
           "flex:0 0 62px;}\n";
    out += ".spk{font-weight:600;flex:0 0 76px;font-size:13px;}\n";
    out += "li{margin:5px 0;}ul{padding-left:22px;}\n";
    out += ".risk{background:#fff7ed;border-left:3px solid #f59e0b;padding:8px 12px;"
           "margin:6px 0;border-radius:4px;}\n";
    out += "footer{margin-top:40px;color:var(--muted);font-size:12px;"
           "border-top:1px solid var(--line);padding-top:12px;}\n";
    out += "</style>\n</head>\n<body>\n";

    out += "<h1>" + htmlEscape(r.title) + "</h1>\n";
    out += "<div class=\"meta\">会议日期 " + htmlEscape(r.meetingDate) + " · 时长 " +
           timeutil::formatDuration(r.quality.durationMs) + " · 说话人 " +
           std::to_string(r.minutes.stats.speakerCount) + " 人 · 转写 " +
           std::to_string(r.minutes.stats.cjkCharacters) + " 字</div>\n";

    if (!r.minutes.overview.empty()) {
        out += "<h2>会议概览</h2>\n<p>" + htmlEscape(r.minutes.overview) + "</p>\n";
    }
    if (!r.minutes.topics.empty()) {
        out += "<h2>议题脉络</h2>\n<ol>\n";
        for (const nlp::Topic& t : r.minutes.topics) {
            out += "<li><strong>" + htmlEscape(t.title) + "</strong>";
            if (t.startMs >= 0) {
                out += " <span class=\"meta\">" + timeutil::formatDuration(t.startMs) + "</span>";
            }
            out += "</li>\n";
        }
        out += "</ol>\n";
    }
    if (!r.minutes.keyPoints.empty()) {
        out += "<h2>关键结论</h2>\n<ul>\n";
        for (const nlp::SummarySentence& s : r.minutes.keyPoints) {
            out += "<li>";
            if (s.startMs >= 0) {
                out += "<span class=\"time\">" + timeutil::formatDuration(s.startMs) + "</span> ";
            }
            out += htmlEscape(s.text) + "</li>\n";
        }
        out += "</ul>\n";
    }
    if (!r.minutes.decisions.empty()) {
        out += "<h2>会议决议</h2>\n<ul>\n";
        for (const nlp::Decision& d : r.minutes.decisions) {
            out += "<li><strong>[" + htmlEscape(d.kind) + "]</strong> " + htmlEscape(d.text) +
                   "</li>\n";
        }
        out += "</ul>\n";
    }
    if (!r.minutes.actionItems.empty()) {
        out += "<h2>待办事项</h2>\n<table>\n<tr><th>#</th><th>任务</th><th>责任人</th>"
               "<th>截止</th><th>优先级</th></tr>\n";
        int i = 1;
        for (const nlp::ActionItem& a : r.minutes.actionItems) {
            out += "<tr><td>" + std::to_string(i++) + "</td><td>" + htmlEscape(a.task) +
                   "</td><td>" + (a.owner.empty() ? "待指定" : htmlEscape(a.owner)) + "</td><td>" +
                   (a.dueDate.empty() ? (a.dueText.empty() ? "待定" : htmlEscape(a.dueText))
                                      : a.dueDate) +
                   "</td><td>" + htmlEscape(a.priority) + "</td></tr>\n";
        }
        out += "</table>\n";
    }
    if (!r.minutes.keywords.empty()) {
        out += "<h2>关键词</h2>\n<p>";
        for (size_t i = 0; i < r.minutes.keywords.size(); ++i) {
            if (i) out += " · ";
            out += htmlEscape(r.minutes.keywords[i].word) + " <span class=\"meta\">" +
                   std::to_string(r.minutes.keywords[i].frequency) + "</span>";
        }
        out += "</p>\n";
    }
    if (!r.minutes.risks.empty()) {
        out += "<h2>风险与待确认</h2>\n";
        for (const std::string& s : r.minutes.risks) {
            out += "<div class=\"risk\">" + htmlEscape(s) + "</div>\n";
        }
    }
    out += "<h2>全文转写</h2>\n";
    for (const asr::AsrSegment& s : r.asr.segments) {
        const std::string text = str::trim(s.text);
        if (text.empty()) continue;
        out += "<div class=\"seg\"><span class=\"time\">" + timeutil::formatDuration(s.startMs) +
               "</span><span class=\"spk\" style=\"color:" + speakerColor(s.speakerId) + "\">" +
               htmlEscape(r.speakerLabel(s.speakerId)) + "</span><span>" + htmlEscape(text) +
               "</span></div>\n";
    }
    out += "<footer>由 MeetMind 端侧语音转写与智能会议纪要工具生成 · 全流程离线运行 · " +
           timeutil::nowDateTime() + "</footer>\n</body>\n</html>\n";
    return out;
}

}  // namespace

Result<std::string> Exporter::render(const PipelineResult& result, ExportFormat format,
                                     const std::vector<std::string>* plannedFiles) {
    // 渲染时可见的文件列表：优先使用「本次将写出的路径」，避免自引用导致列表为空
    const std::vector<std::string>& files =
        (plannedFiles != nullptr) ? *plannedFiles : result.exportedFiles;

    switch (format) {
        case ExportFormat::Markdown: return renderMarkdown(result, files);
        case ExportFormat::Json: {
            Json j = result.toJson();
            j.set("exportedFiles", Json::of(files));
            return j.dump(2);
        }
        case ExportFormat::Srt: return renderSrt(result);
        case ExportFormat::Text: return renderText(result);
        case ExportFormat::Html: return renderHtml(result, files);
    }
    return fail(ErrorCode::InvalidArgument, "未知导出格式");
}

std::vector<ExportFormat> Exporter::formatsFromConfig(const Config& config) {
    std::vector<ExportFormat> formats;
    if (config.exportMarkdown) formats.push_back(ExportFormat::Markdown);
    if (config.exportJson) formats.push_back(ExportFormat::Json);
    if (config.exportSrt) formats.push_back(ExportFormat::Srt);
    if (config.exportText) formats.push_back(ExportFormat::Text);
    if (config.exportHtml) formats.push_back(ExportFormat::Html);
    if (formats.empty()) formats.push_back(ExportFormat::Markdown);
    return formats;
}

std::string Exporter::deriveBaseName(const PipelineResult& result, const Config& config) {
    if (!config.baseFileName.empty()) {
        return str::sanitizeFileName(config.baseFileName, "meeting");
    }
    std::string stem;
    if (!result.inputPath.empty()) {
        stem = pathutil::stem(result.inputPath);
    }
    std::string base = stem.empty() ? result.title : stem;
    if (base.empty()) base = "meeting";
    base = str::sanitizeFileName(base, "meeting");
    // 纯数字/ASCII 组合时附加日期，避免同名覆盖
    if (!result.meetingDate.empty()) base += "_" + result.meetingDate;
    return str::sanitizeFileName(base, "meeting");
}

Result<void> Exporter::writeFileAtomic(const std::string& path, const std::string& content) {
    const std::string parent = pathutil::parentPath(path);
    if (!parent.empty() && !pathutil::createDirectories(parent)) {
        return fail(ErrorCode::IoError, "无法创建目录: " + parent);
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out = pathutil::openOutput(tmp);
        if (!out) return fail(ErrorCode::IoError, "无法写入临时文件: " + tmp);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return fail(ErrorCode::IoError, "写入失败: " + tmp);
    }
    if (!pathutil::rename(tmp, path)) {
        return fail(ErrorCode::IoError, "无法替换目标文件: " + path);
    }
    return okStatus();
}

std::vector<std::string> Exporter::planPaths(const PipelineResult& result, const Config& config,
                                             const ExportOptions& options) {
    std::vector<ExportFormat> formats = options.formats;
    if (formats.empty()) formats = formatsFromConfig(config);

    std::string dir = options.outputDir.empty() ? config.outputDir : options.outputDir;
    if (dir.empty()) {
        // 路径必须经 pathutil 处理，否则中文路径会因编码转换失败而抛异常
        dir = result.inputPath.empty() ? std::string(".") : pathutil::parentPath(result.inputPath);
        if (dir.empty()) dir = ".";
    }

    const std::string base = options.baseName.empty() ? deriveBaseName(result, config)
                                                     : str::sanitizeFileName(options.baseName);
    std::vector<std::string> paths;
    paths.reserve(formats.size());
    for (ExportFormat f : formats) {
        paths.push_back(pathutil::join(dir, base + fileExtension(f)));
    }
    return paths;
}

Result<std::vector<std::string>> Exporter::exportAll(const PipelineResult& result,
                                                     const Config& config,
                                                     const ExportOptions& options) {
    std::vector<ExportFormat> formats = options.formats;
    if (formats.empty()) formats = formatsFromConfig(config);

    // 目标路径统一由 planPaths 决定，避免「导出目录推导」在两处重复实现而走偏
    const std::vector<std::string> planned = planPaths(result, config, options);

    std::vector<std::string> written;
    for (size_t i = 0; i < formats.size(); ++i) {
        // 先确定全部目标路径，再逐个渲染，使产物内可准确引用「本次将写出的文件」
        Result<std::string> content = render(result, formats[i], &planned);
        if (!content.ok()) return fail(content.code(), content.message());
        const std::string path = planned[i];
        Result<void> w = writeFileAtomic(path, content.value());
        if (!w.ok()) return fail(w.code(), w.message());
        written.push_back(path);
        MM_LOG_INFO("export") << "已导出 " << toString(formats[i]) << ": " << path;
    }
    return written;
}

}  // namespace mm::pipeline

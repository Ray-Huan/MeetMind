// MeetMind — 会话历史持久化（基于 JSON，无数据库依赖）
// 目录结构：
//   <root>/index.json          会话索引（轻量，供列表展示）
//   <root>/<id>.json           完整流水线结果快照
#pragma once

#include <string>
#include <vector>

#include "mm/common/json.h"
#include "mm/common/result.h"
#include "mm/pipeline/pipeline_types.h"

namespace mm::storage {

/// 会话索引条目。
struct SessionSummary {
    std::string id;
    std::string title;
    std::string meetingDate;
    std::string createdAt;
    std::string inputPath;
    std::string asrBackend;
    int64_t durationMs = 0;
    int speakerCount = 0;
    int actionItemCount = 0;
    int lowConfidenceSegments = 0;
    std::vector<std::string> keywords;

    Json toJson() const;
    static SessionSummary fromJson(const Json& j);
};

class SessionStore {
public:
    /// @param rootDir 空则使用默认目录（%APPDATA%/MeetMind/sessions）
    explicit SessionStore(std::string rootDir = {});

    static std::string defaultRootDir();

    /// 保存一次完整结果；返回会话 id。
    Result<std::string> save(const pipeline::PipelineResult& result,
                             const std::string& id = {});

    /// 载入完整结果快照。
    Result<Json> load(const std::string& id) const;

    /// 列出全部会话（按创建时间倒序）。
    std::vector<SessionSummary> list() const;

    /// 删除会话。
    Result<void> remove(const std::string& id);

    /// 清空全部会话。
    Result<void> clear();

    size_t count() const { return list().size(); }

    const std::string& rootDir() const { return root_; }

    /// 生成会话 id（时间戳 + 序号，保证可排序且唯一）。
    static std::string makeId();

private:
    Result<void> writeIndex(const std::vector<SessionSummary>& sessions) const;
    /// 仅读取 index.json；不做快照重建（写路径必须使用，避免刚写入的快照被重复计入）。
    std::vector<SessionSummary> readIndexOnly() const;
    /// 由快照文件名重建索引（仅用于索引缺失/损坏时的恢复）。
    std::vector<SessionSummary> rebuildFromSnapshots() const;

    std::string root_;
};

}  // namespace mm::storage

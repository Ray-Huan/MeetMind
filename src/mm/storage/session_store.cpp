#include "mm/storage/session_store.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "mm/common/path_utils.h"
#include "mm/common/config.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm::storage {
namespace fs = std::filesystem;

Json SessionSummary::toJson() const {
    Json j = Json::object();
    j.set("id", id);
    j.set("title", title);
    j.set("meetingDate", meetingDate);
    j.set("createdAt", createdAt);
    j.set("inputPath", inputPath);
    j.set("asrBackend", asrBackend);
    j.set("durationMs", durationMs);
    j.set("speakerCount", speakerCount);
    j.set("actionItemCount", actionItemCount);
    j.set("lowConfidenceSegments", lowConfidenceSegments);
    j.set("keywords", Json::of(keywords));
    return j;
}

SessionSummary SessionSummary::fromJson(const Json& j) {
    SessionSummary s;
    s.id = j.getString("id");
    s.title = j.getString("title");
    s.meetingDate = j.getString("meetingDate");
    s.createdAt = j.getString("createdAt");
    s.inputPath = j.getString("inputPath");
    s.asrBackend = j.getString("asrBackend");
    s.durationMs = j.getInt64("durationMs");
    s.speakerCount = j.getInt("speakerCount");
    s.actionItemCount = j.getInt("actionItemCount");
    s.lowConfidenceSegments = j.getInt("lowConfidenceSegments");
    const Json& kw = j.get("keywords");
    for (size_t i = 0; i < kw.size(); ++i) s.keywords.push_back(kw.at(i).asString());
    return s;
}

std::string SessionStore::defaultRootDir() {
#if defined(_WIN32)
    const char* appData = std::getenv("APPDATA");
    if (appData && *appData) {
        return pathutil::joinPath(appData, "MeetMind", "sessions");
    }
#endif
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return pathutil::joinPath(home, ".meetmind", "sessions");
    }
    return "meetmind_sessions";
}

SessionStore::SessionStore(std::string rootDir)
    : root_(rootDir.empty() ? defaultRootDir() : std::move(rootDir)) {}

std::string SessionStore::makeId() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

Result<std::string> SessionStore::save(const pipeline::PipelineResult& result,
                                       const std::string& id) {
    if (!pathutil::createDirectories(root_)) {
        return fail(ErrorCode::IoError, "无法创建会话目录: " + root_);
    }

    std::string sessionId = str::sanitizeFileName(id, makeId());
    if (sessionId.empty()) sessionId = makeId();

    // 避免覆盖：若已存在则追加序号
    int suffix = 1;
    while (pathutil::exists(pathutil::join(root_, sessionId + ".json")) && suffix < 1000) {
        sessionId = str::sanitizeFileName(id, makeId()) + "-" + std::to_string(suffix++);
    }
    const std::string target = pathutil::join(root_, sessionId + ".json");

    Json snapshotJson = result.toJson();
    snapshotJson.set("sessionId", sessionId);
    snapshotJson.set("createdAt", timeutil::nowDateTime());
    Result<void> wrote = snapshotJson.writeFile(target, 2);
    if (!wrote.ok()) return fail(wrote.code(), wrote.message());

    // 更新索引（只读索引本身，避免刚写入的快照被重复计入）
    std::vector<SessionSummary> sessions = readIndexOnly();
    SessionSummary s;
    s.id = sessionId;
    s.title = result.title;
    s.meetingDate = result.meetingDate;
    s.createdAt = timeutil::nowDateTime();
    s.inputPath = result.inputPath;
    s.asrBackend = result.report.asrBackend;
    s.durationMs = result.quality.durationMs;
    s.speakerCount = result.minutes.stats.speakerCount;
    s.actionItemCount = static_cast<int>(result.minutes.actionItems.size());
    s.lowConfidenceSegments = result.minutes.stats.lowConfidenceSegments;
    for (size_t i = 0; i < result.minutes.keywords.size() && i < 5; ++i) {
        s.keywords.push_back(result.minutes.keywords[i].word);
    }
    sessions.insert(sessions.begin(), std::move(s));
    Result<void> idx = writeIndex(sessions);
    if (!idx.ok()) return fail(idx.code(), idx.message());

    MM_LOG_INFO("storage") << "会话已保存: " << target;
    return sessionId;
}

Result<void> SessionStore::writeIndex(const std::vector<SessionSummary>& sessions) const {
    Json arr = Json::array();
    std::vector<std::string> seen;
    for (const SessionSummary& s : sessions) {
        if (s.id.empty()) continue;
        if (std::find(seen.begin(), seen.end(), s.id) != seen.end()) continue;  // 按 id 去重
        seen.push_back(s.id);
        arr.push(s.toJson());
    }
    Json root = Json::object();
    root.set("schemaVersion", "1.0");
    root.set("updatedAt", timeutil::nowDateTime());
    root.set("sessions", arr);
    return root.writeFile(pathutil::join(root_, "index.json"), 2);
}

Result<Json> SessionStore::load(const std::string& id) const {
    const std::string safe = str::sanitizeFileName(id, "");
    if (safe.empty()) return fail(ErrorCode::InvalidArgument, "非法会话 id");
    return Json::parseFile(pathutil::join(root_, safe + ".json"));
}

std::vector<SessionSummary> SessionStore::readIndexOnly() const {
    std::vector<SessionSummary> out;
    const std::string index = pathutil::join(root_, "index.json");
    if (!pathutil::exists(index)) return out;

    Result<Json> parsed = Json::parseFile(index);
    if (!parsed.ok()) {
        MM_LOG_WARN("storage") << "索引损坏: " << parsed.message();
        return out;
    }
    const Json& arr = parsed.value().get("sessions");
    const std::string updatedAt = parsed.value().getString("updatedAt");
    for (size_t i = 0; i < arr.size(); ++i) {
        SessionSummary s = SessionSummary::fromJson(arr.at(i));
        if (s.createdAt.empty()) s.createdAt = updatedAt;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<SessionSummary> SessionStore::rebuildFromSnapshots() const {
    std::vector<SessionSummary> out;
    for (const std::string& file : pathutil::listFiles(root_, false, ".json")) {
        const std::string name = pathutil::toUtf8(pathutil::fromUtf8(file).filename());
        if (name == "index.json") continue;
        SessionSummary s;
        s.id = pathutil::stem(file);
        s.title = s.id;
        s.createdAt = name;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SessionSummary& a, const SessionSummary& b) {
        return a.id > b.id;
    });
    return out;
}

std::vector<SessionSummary> SessionStore::list() const {
    std::vector<SessionSummary> out = readIndexOnly();
    if (!out.empty()) {
        std::sort(out.begin(), out.end(), [](const SessionSummary& a, const SessionSummary& b) {
            return a.createdAt > b.createdAt;
        });
        return out;
    }
    // 索引缺失或损坏 → 由快照重建
    return rebuildFromSnapshots();
}

Result<void> SessionStore::remove(const std::string& id) {
    const std::string safe = str::sanitizeFileName(id, "");
    if (safe.empty()) return fail(ErrorCode::InvalidArgument, "非法会话 id");
    const std::string p = pathutil::join(root_, safe + ".json");
    if (!pathutil::exists(p)) return fail(ErrorCode::FileNotFound, "会话不存在: " + safe);
    if (!pathutil::removeFile(p)) return fail(ErrorCode::IoError, "删除失败: " + safe);

    std::vector<SessionSummary> sessions = readIndexOnly();
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [&](const SessionSummary& s) { return s.id == safe; }),
                   sessions.end());
    return writeIndex(sessions);
}

Result<void> SessionStore::clear() {
    for (const std::string& file : pathutil::listFiles(root_, false, ".json")) {
        pathutil::removeFile(file);
    }
    return okStatus();
}

}  // namespace mm::storage

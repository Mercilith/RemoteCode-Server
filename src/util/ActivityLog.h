#pragma once

#include <mutex>
#include <string>

#include "../third_party/json.hpp"

// Structured, human-inspectable record of what each agent does — separate
// from the messages table (which only holds what agents *say*), so tool
// calls, blocked mentions, and turn outcomes are all visible for later
// behavior/functionality tuning without replaying Discord history.
//
// Appends one JSON object per line to a daily rolling file under a log
// directory: `activity-YYYY-MM-DD.<source>.jsonl`. `source` is baked into
// the filename (not just a field) so the main service process
// ("orchestrator") and each per-turn MCP subprocess ("mcp-<agentId>") never
// share a file handle across process boundaries — only same-process,
// multi-thread writes need the mutex below.
class ActivityLog {
public:
    // Creates `logDir` if it doesn't already exist.
    ActivityLog(std::wstring logDir, std::string source);

    // Appends {"ts","source","chatId","agentId","eventType","detail"}.
    // `detail` may be any JSON value; pass nlohmann::json{} (null) to omit
    // the field entirely.
    void Log(
        const std::string& chatId, const std::string& agentId, const std::string& eventType,
        const nlohmann::json& detail = nlohmann::json::object());

private:
    std::wstring logDir_;
    std::string source_;
    std::mutex mutex_;
};

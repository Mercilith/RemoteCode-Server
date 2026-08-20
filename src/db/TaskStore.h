#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

// Fixed status enum for the `tasks` table — validated by TaskStatus::IsValid
// rather than left as free text, since create_task/update_task_status (see
// mcp/Tools.cpp) both need a single source of truth for "is this a real
// status" and the exact wording of the reject message.
namespace TaskStatus {
constexpr const char* kNotStarted = "not_started";
constexpr const char* kInProgress = "in_progress";
constexpr const char* kBlocked = "blocked";
constexpr const char* kInReview = "in_review";
constexpr const char* kDone = "done";
constexpr const char* kCancelled = "cancelled";

bool IsValid(const std::string& status);
// Comma-separated list of every valid status, for error messages.
std::string ValidList();
} // namespace TaskStatus

struct Task {
    std::string id;
    std::string workspaceId;      // may be empty — not every task is scoped to a workspace
    std::string chatId;           // may be empty
    std::string title;
    std::string description;      // may be empty
    std::string status;           // one of TaskStatus's constants
    std::string assigneeAgentId;  // may be empty — unassigned
    std::string createdBy;        // the agent (or "user") that created the task
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

class TaskStore {
public:
    explicit TaskStore(Database& db) : db_(db) {}

    bool Create(const Task& task);
    bool Get(const std::string& id, Task& outTask);

    // Updates status and, if setAssignee is true, also reassigns
    // assignee_agent_id (to assigneeAgentId, which may be passed empty to
    // unassign). setAssignee is false when the caller (update_task_status)
    // didn't pass an assignee_agent_id at all, so the existing assignee is
    // left untouched rather than being wiped.
    bool UpdateStatus(
        const std::string& id, const std::string& status, bool setAssignee, const std::string& assigneeAgentId,
        int64_t updatedAt);

    // Every filter is optional (empty string = "don't filter on this") —
    // used by the list_tasks MCP tool, which lets an agent narrow by any
    // combination of status/assignee/workspace or omit all three to search
    // everything. Most recently created first.
    std::vector<Task> List(
        const std::string& status, const std::string& assigneeAgentId, const std::string& workspaceId);

private:
    Database& db_;
};

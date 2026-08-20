#include "TaskStore.h"

namespace TaskStatus {

bool IsValid(const std::string& status) {
    return status == kNotStarted || status == kInProgress || status == kBlocked || status == kInReview ||
           status == kDone || status == kCancelled;
}

std::string ValidList() {
    return std::string("'") + kNotStarted + "', '" + kInProgress + "', '" + kBlocked + "', '" + kInReview +
           "', '" + kDone + "', '" + kCancelled + "'";
}

} // namespace TaskStatus

namespace {

std::string OrEmpty(const Statement& stmt, int index) {
    return stmt.ColumnIsNull(index) ? "" : stmt.ColumnText(index);
}

constexpr const char* kTaskColumns =
    "id, workspace_id, chat_id, title, description, status, assignee_agent_id, created_by, created_at, "
    "updated_at";

void ReadTaskRow(const Statement& stmt, Task& out) {
    out.id = stmt.ColumnText(0);
    out.workspaceId = OrEmpty(stmt, 1);
    out.chatId = OrEmpty(stmt, 2);
    out.title = stmt.ColumnText(3);
    out.description = OrEmpty(stmt, 4);
    out.status = stmt.ColumnText(5);
    out.assigneeAgentId = OrEmpty(stmt, 6);
    out.createdBy = stmt.ColumnText(7);
    out.createdAt = stmt.ColumnInt64(8);
    out.updatedAt = stmt.ColumnInt64(9);
}

} // namespace

bool TaskStore::Create(const Task& task) {
    Statement stmt(
        db_,
        "INSERT INTO tasks (id, workspace_id, chat_id, title, description, status, assignee_agent_id, "
        "created_by, created_at, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, task.id);
    if (task.workspaceId.empty()) {
        stmt.BindNull(2);
    } else {
        stmt.BindText(2, task.workspaceId);
    }
    if (task.chatId.empty()) {
        stmt.BindNull(3);
    } else {
        stmt.BindText(3, task.chatId);
    }
    stmt.BindText(4, task.title);
    if (task.description.empty()) {
        stmt.BindNull(5);
    } else {
        stmt.BindText(5, task.description);
    }
    stmt.BindText(6, task.status);
    if (task.assigneeAgentId.empty()) {
        stmt.BindNull(7);
    } else {
        stmt.BindText(7, task.assigneeAgentId);
    }
    stmt.BindText(8, task.createdBy);
    stmt.BindInt64(9, task.createdAt);
    stmt.BindInt64(10, task.updatedAt);
    stmt.Step();
    return stmt.Ok();
}

bool TaskStore::Get(const std::string& id, Task& outTask) {
    Statement stmt(db_, std::string("SELECT ") + kTaskColumns + " FROM tasks WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadTaskRow(stmt, outTask);
    return true;
}

bool TaskStore::UpdateStatus(
    const std::string& id, const std::string& status, bool setAssignee, const std::string& assigneeAgentId,
    int64_t updatedAt) {
    const std::string sql = setAssignee
                                 ? "UPDATE tasks SET status = ?1, assignee_agent_id = ?2, updated_at = ?3 "
                                   "WHERE id = ?4;"
                                 : "UPDATE tasks SET status = ?1, updated_at = ?2 WHERE id = ?3;";
    Statement stmt(db_, sql);
    if (!stmt.Valid()) {
        return false;
    }
    if (setAssignee) {
        stmt.BindText(1, status);
        if (assigneeAgentId.empty()) {
            stmt.BindNull(2);
        } else {
            stmt.BindText(2, assigneeAgentId);
        }
        stmt.BindInt64(3, updatedAt);
        stmt.BindText(4, id);
    } else {
        stmt.BindText(1, status);
        stmt.BindInt64(2, updatedAt);
        stmt.BindText(3, id);
    }
    stmt.Step();
    return stmt.Ok();
}

std::vector<Task> TaskStore::List(
    const std::string& status, const std::string& assigneeAgentId, const std::string& workspaceId) {
    std::vector<Task> tasks;

    std::string sql = std::string("SELECT ") + kTaskColumns + " FROM tasks WHERE 1=1";
    if (!status.empty()) {
        sql += " AND status = ?1";
    }
    if (!assigneeAgentId.empty()) {
        sql += " AND assignee_agent_id = ?2";
    }
    if (!workspaceId.empty()) {
        sql += " AND workspace_id = ?3";
    }
    sql += " ORDER BY created_at DESC;";

    Statement stmt(db_, sql);
    if (!stmt.Valid()) {
        return tasks;
    }
    if (!status.empty()) {
        stmt.BindText(1, status);
    }
    if (!assigneeAgentId.empty()) {
        stmt.BindText(2, assigneeAgentId);
    }
    if (!workspaceId.empty()) {
        stmt.BindText(3, workspaceId);
    }
    while (stmt.Step()) {
        Task task;
        ReadTaskRow(stmt, task);
        tasks.push_back(std::move(task));
    }
    return tasks;
}

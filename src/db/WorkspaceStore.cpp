#include "WorkspaceStore.h"

namespace {

std::string OrEmpty(const Statement& stmt, int index) {
    return stmt.ColumnIsNull(index) ? "" : stmt.ColumnText(index);
}

constexpr const char* kWorkspaceColumns =
    "id, title, repo_ids, discord_category_id, chat_id, created_by, status, last_error, "
    "created_at, updated_at";

void ReadWorkspaceRow(const Statement& stmt, Workspace& out) {
    out.id = stmt.ColumnText(0);
    out.title = OrEmpty(stmt, 1);
    out.repoIdsJson = stmt.ColumnText(2);
    out.discordCategoryId = OrEmpty(stmt, 3);
    out.chatId = stmt.ColumnText(4);
    out.createdBy = stmt.ColumnText(5);
    out.status = stmt.ColumnText(6);
    out.lastError = OrEmpty(stmt, 7);
    out.createdAt = stmt.ColumnInt64(8);
    out.updatedAt = stmt.ColumnInt64(9);
}

} // namespace

bool WorkspaceStore::Create(const Workspace& workspace) {
    Statement stmt(
        db_,
        "INSERT INTO workspaces (id, title, repo_ids, discord_category_id, chat_id, created_by, "
        "status, last_error, created_at, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, workspace.id);
    if (workspace.title.empty()) {
        stmt.BindNull(2);
    } else {
        stmt.BindText(2, workspace.title);
    }
    stmt.BindText(3, workspace.repoIdsJson);
    if (workspace.discordCategoryId.empty()) {
        stmt.BindNull(4);
    } else {
        stmt.BindText(4, workspace.discordCategoryId);
    }
    stmt.BindText(5, workspace.chatId);
    stmt.BindText(6, workspace.createdBy);
    stmt.BindText(7, workspace.status);
    if (workspace.lastError.empty()) {
        stmt.BindNull(8);
    } else {
        stmt.BindText(8, workspace.lastError);
    }
    stmt.BindInt64(9, workspace.createdAt);
    stmt.BindInt64(10, workspace.updatedAt);
    stmt.Step();
    return stmt.Ok();
}

bool WorkspaceStore::Get(const std::string& id, Workspace& outWorkspace) {
    Statement stmt(db_, std::string("SELECT ") + kWorkspaceColumns + " FROM workspaces WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadWorkspaceRow(stmt, outWorkspace);
    return true;
}

std::vector<Workspace> WorkspaceStore::ListAll() {
    std::vector<Workspace> workspaces;
    Statement stmt(
        db_, std::string("SELECT ") + kWorkspaceColumns + " FROM workspaces ORDER BY created_at DESC;");
    if (!stmt.Valid()) {
        return workspaces;
    }
    while (stmt.Step()) {
        Workspace workspace;
        ReadWorkspaceRow(stmt, workspace);
        workspaces.push_back(std::move(workspace));
    }
    return workspaces;
}

std::vector<Workspace> WorkspaceStore::ListPendingDiscordSetup() {
    std::vector<Workspace> workspaces;
    Statement stmt(
        db_, std::string("SELECT ") + kWorkspaceColumns +
                 " FROM workspaces WHERE status = 'ready' ORDER BY created_at ASC;");
    if (!stmt.Valid()) {
        return workspaces;
    }
    while (stmt.Step()) {
        Workspace workspace;
        ReadWorkspaceRow(stmt, workspace);
        workspaces.push_back(std::move(workspace));
    }
    return workspaces;
}

bool WorkspaceStore::SetDiscordCategoryId(
    const std::string& id, const std::string& discordCategoryId, int64_t updatedAt) {
    Statement stmt(db_, "UPDATE workspaces SET discord_category_id = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, discordCategoryId);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

bool WorkspaceStore::SetStatus(const std::string& id, const std::string& status, int64_t updatedAt) {
    Statement stmt(db_, "UPDATE workspaces SET status = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, status);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

bool WorkspaceStore::SetError(const std::string& id, const std::string& lastError, int64_t updatedAt) {
    Statement stmt(
        db_, "UPDATE workspaces SET status = 'failed', last_error = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, lastError);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

#include "RepoStore.h"

namespace {

std::string OrEmpty(const Statement& stmt, int index) {
    return stmt.ColumnIsNull(index) ? "" : stmt.ColumnText(index);
}

constexpr const char* kRepoColumns =
    "id, github_url, local_path, agent_id, status, notes, last_error, created_at, updated_at";

void ReadRepoRow(const Statement& stmt, Repo& out) {
    out.id = stmt.ColumnText(0);
    out.githubUrl = stmt.ColumnText(1);
    out.localPath = stmt.ColumnText(2);
    out.agentId = OrEmpty(stmt, 3);
    out.status = stmt.ColumnText(4);
    out.notes = OrEmpty(stmt, 5);
    out.lastError = OrEmpty(stmt, 6);
    out.createdAt = stmt.ColumnInt64(7);
    out.updatedAt = stmt.ColumnInt64(8);
}

} // namespace

bool RepoStore::Create(const Repo& repo) {
    Statement stmt(
        db_,
        "INSERT INTO repos (id, github_url, local_path, agent_id, status, notes, last_error, "
        "created_at, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, repo.id);
    stmt.BindText(2, repo.githubUrl);
    stmt.BindText(3, repo.localPath);
    if (repo.agentId.empty()) {
        stmt.BindNull(4);
    } else {
        stmt.BindText(4, repo.agentId);
    }
    stmt.BindText(5, repo.status);
    if (repo.notes.empty()) {
        stmt.BindNull(6);
    } else {
        stmt.BindText(6, repo.notes);
    }
    if (repo.lastError.empty()) {
        stmt.BindNull(7);
    } else {
        stmt.BindText(7, repo.lastError);
    }
    stmt.BindInt64(8, repo.createdAt);
    stmt.BindInt64(9, repo.updatedAt);
    stmt.Step();
    return stmt.Ok();
}

bool RepoStore::Get(const std::string& id, Repo& outRepo) {
    Statement stmt(db_, std::string("SELECT ") + kRepoColumns + " FROM repos WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadRepoRow(stmt, outRepo);
    return true;
}

std::vector<Repo> RepoStore::ListAll() {
    std::vector<Repo> repos;
    Statement stmt(db_, std::string("SELECT ") + kRepoColumns + " FROM repos ORDER BY created_at DESC;");
    if (!stmt.Valid()) {
        return repos;
    }
    while (stmt.Step()) {
        Repo repo;
        ReadRepoRow(stmt, repo);
        repos.push_back(std::move(repo));
    }
    return repos;
}

bool RepoStore::SetStatus(const std::string& id, const std::string& status, int64_t updatedAt) {
    Statement stmt(db_, "UPDATE repos SET status = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, status);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

bool RepoStore::SetAgentId(const std::string& id, const std::string& agentId, int64_t updatedAt) {
    Statement stmt(db_, "UPDATE repos SET agent_id = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

bool RepoStore::SetError(const std::string& id, const std::string& lastError, int64_t updatedAt) {
    Statement stmt(
        db_, "UPDATE repos SET status = 'failed', last_error = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, lastError);
    stmt.BindInt64(2, updatedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

bool RepoStore::ClearError(const std::string& id, int64_t updatedAt) {
    Statement stmt(
        db_, "UPDATE repos SET status = 'cloning', last_error = '', updated_at = ?1 WHERE id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindInt64(1, updatedAt);
    stmt.BindText(2, id);
    stmt.Step();
    return stmt.Ok();
}

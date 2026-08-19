#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

// A workspace = a git worktree of each of 1+ already-imported repos, backed
// by a dedicated Discord category (channel group) containing an initial
// conversation chat. See orchestrator/WorkspaceCreator.h for how a row here
// actually gets created, and Orchestrator::CreateWorkspace/
// Orchestrator::EnsurePendingWorkspaceChannels for how the Discord side gets
// attached (possibly lazily — see those comments for why).
struct Workspace {
    std::string id;
    std::string title;
    std::string repoIdsJson;        // JSON array of repo ids included (explicit request plus any
                                     // heuristic-detected dependencies — see WorkspaceCreator).
    std::string discordCategoryId;  // empty until the Discord category has actually been created
    std::string chatId;             // the workspace's initial conversation chat
    std::string createdBy;          // "user" (Cardon via /create-workspace) or an agent id
    std::string status;             // "creating" | "ready" | "active" | "failed"
    std::string lastError;          // set alongside status == "failed"
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

class WorkspaceStore {
public:
    explicit WorkspaceStore(Database& db) : db_(db) {}

    bool Create(const Workspace& workspace);
    bool Get(const std::string& id, Workspace& outWorkspace);
    // Every workspace, most recently created first.
    std::vector<Workspace> ListAll();
    // Workspaces whose DB/filesystem setup is done (status == "ready") but
    // whose Discord category/channel isn't yet — polled by
    // Orchestrator::EnsurePendingWorkspaceChannels right after every
    // HandleIncomingMessage dispatch (same "housekeeping" slot as
    // PostPendingApprovals) to lazily create them for a workspace the
    // create_workspace MCP tool created — that tool runs in the separate MCP
    // subprocess, which has no live Discord connection, so it can only do
    // the DB/filesystem half of the work. A workspace reaches "active" only
    // once both the category AND its initial channel exist.
    std::vector<Workspace> ListPendingDiscordSetup();

    bool SetDiscordCategoryId(const std::string& id, const std::string& discordCategoryId, int64_t updatedAt);
    bool SetStatus(const std::string& id, const std::string& status, int64_t updatedAt);
    // Also sets status to "failed" — a workspace with a recorded error is by
    // definition in the failed state (mirrors RepoStore::SetError).
    bool SetError(const std::string& id, const std::string& lastError, int64_t updatedAt);

private:
    Database& db_;
};

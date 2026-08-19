#pragma once

#include <string>
#include <vector>

#include "../db/ChatStore.h"
#include "../db/RepoStore.h"
#include "../db/WorkspaceStore.h"

// Shared core of `/create-workspace` and the `create_workspace` MCP tool —
// see Orchestrator::CreateWorkspace and mcp/Tools.cpp's CreateWorkspaceTool
// for the two entry points. Deliberately touches ONLY the database and the
// local filesystem/git — never Discord. That split exists because the MCP
// tool runs inside a separate subprocess (McpServer, spawned per agent turn
// — see main.cpp's --mcp-server mode) with no live Discord connection at
// all, only a Database handle onto the same SQLite file the main service
// uses. The Discord category/initial-channel creation is therefore always
// done afterward, in-process, by whichever caller actually has a DiscordBot:
// Orchestrator::CreateWorkspace does it synchronously right after calling
// this (the /create-workspace slash-command path), and
// Orchestrator::EnsurePendingWorkspaceChannels does it lazily on a poll (the
// create_workspace MCP-tool path) — see WorkspaceStore::ListMissingDiscordCategory.
namespace WorkspaceCreator {

struct Result {
    bool ok = false;
    std::string workspaceId;
    std::string chatId;
    std::string title;
    // Final resolved repo id set — the explicitly requested repos plus any
    // heuristic-detected dependencies (see the comment above ResolveRepoSet
    // in WorkspaceCreator.cpp for exactly what "heuristic" means here).
    std::vector<std::string> repoIds;
    std::vector<std::string> worktreePaths; // parallel to repoIds
    std::string error;
};

// Resolves `repoIdsOrNames` (each token matched against RepoStore::ListAll()
// by id, or by the org/repo or bare repo name parsed out of githubUrl, all
// case-insensitively) against already-imported repos, expands the set with
// the best-effort dependency heuristic, creates a git worktree per resolved
// repo (via ProcessRunner — never an agent turn, matching this codebase's
// hard rule that repo/worktree filesystem operations are always programmatic),
// and writes the `workspaces` row, the initial conversation `chats` row, each
// involved repo's dedicated agent as a `chat_participants` row in listening
// mode, and a system message describing the workspace. `requestedByAgentId`
// is empty for the /create-workspace slash command (Cardon) or an agent id
// for the create_workspace MCP tool.
Result Create(
    RepoStore& repoStore, WorkspaceStore& workspaceStore, ChatStore& chatStore,
    const std::vector<std::string>& repoIdsOrNames, const std::string& title,
    const std::string& requestedByAgentId);

// The exact "%ProgramData%\RemoteCode\Workspaces\<workspaceId>\<repoId>"
// worktree path and "workspace/<workspaceId>/<repoId>" branch name Create()
// uses internally when it runs `git worktree add` — exported so a later,
// separate operation on that same worktree (see orchestrator/WorkspacePr.h's
// PR-creation flow) can recompute both deterministically rather than needing
// them persisted anywhere. Returns an empty wstring if %ProgramData% can't be
// resolved; also ensures the parent "...\Workspaces\<workspaceId>\" directory
// exists, same as the internal caller inside Create() relies on.
std::wstring RepoWorktreePath(const std::string& workspaceId, const std::string& repoId);
std::string RepoWorktreeBranch(const std::string& workspaceId, const std::string& repoId);

} // namespace WorkspaceCreator

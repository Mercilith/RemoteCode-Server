#pragma once

#include <string>

// Pushes a workspace repo's worktree branch and opens a pull request for it
// via the `gh` CLI — see mcp/Tools.cpp's CreatePullRequestTool, the
// `create_pull_request` MCP tool this backs. Deliberately the same shape as
// WorkspaceCreator: a plain programmatic subprocess call (git push, then
// `gh pr create`), never an agent turn — this codebase's hard rule for any
// git/GitHub operation (see Orchestrator::RunRepoOnboarding's gh repo clone
// call and WorkspaceCreator::Create's git worktree add call for the same
// pattern). The agent only ever supplies the title/body/base branch text;
// the actual git/gh invocation is this module's job, not the agent's.
namespace WorkspacePr {

struct Result {
    bool ok = false;
    std::string prUrl;
    std::string error;
};

// `workspaceId`/`repoId` locate the worktree via
// WorkspaceCreator::RepoWorktreePath/RepoWorktreeBranch (the same
// deterministic path/branch WorkspaceCreator::Create used to create it —
// nothing about the worktree's location needs to be separately persisted).
// Runs `git push -u origin <branch>` followed by
// `gh pr create --title ... --body-file ... --base <baseBranch> --head <branch>`
// with the worktree as the working directory. `baseBranch` defaults to
// "main" if empty.
Result Create(
    const std::string& workspaceId, const std::string& repoId, const std::string& title, const std::string& body,
    const std::string& baseBranch);

} // namespace WorkspacePr

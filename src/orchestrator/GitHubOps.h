#pragma once

#include <string>

#include "../third_party/json.hpp"

// Repo-level (no local worktree needed) GitHub read/write operations backing
// the list_pull_requests / get_pr_status / set_pr_status / merge_pull_request
// / list_issues / create_issue / set_issue_status MCP tools -- see
// mcp/Tools.cpp's *Tool wrapper functions for the args-validation/dispatch
// side. Same rule as WorkspacePr.h: plain programmatic `gh` subprocess calls,
// never an agent turn -- the agent only ever supplies the structured
// arguments (repo_id, pr/issue number, status, text), this module does the
// actual `gh` invocation, targeting `owner/repo` directly via `gh`'s --repo
// flag rather than a local clone (unlike WorkspacePr::Create, which operates
// on a workspace's git worktree).
namespace GitHubOps {

struct Result {
    bool ok = false;
    // Structured result for the list/status-reading operations (an array or
    // object straight from `gh ... --json ...`'s stdout).
    nlohmann::json data;
    // Plain-text result for operations that don't return JSON (e.g. the new
    // issue's URL from `gh issue create`).
    std::string text;
    std::string error;
};

// ---------------------------------------------------------------------------
// Pure validation/mapping helpers -- no process spawning, safe to unit test
// directly. Each returns false and fills outError with a caller-facing
// message on an invalid input; on success outState/outMethod carries gh's
// expected value.
// ---------------------------------------------------------------------------

// Maps list_pull_requests'/list_issues' optional `status` filter to gh's
// --state value. `forPr` selects the allowed set: PRs accept
// open/closed/merged/all, issues only open/closed/all (gh issue list has no
// "merged" state). An empty filter defaults to "open" for both.
bool ResolveListState(const std::string& filter, bool forPr, std::string& outState, std::string& outError);

// Validates set_pr_status's `status` arg against the four non-merge states
// this tool supports (open/closed/approved/changes-requested). Explicitly
// rejects "merged" with a message pointing at merge_pull_request instead --
// callers must not be able to merge a PR through this tool.
bool ValidateSetPrStatus(const std::string& status, std::string& outError);

// Validates set_issue_status's `status` arg (open/closed only).
bool ValidateSetIssueStatus(const std::string& status, std::string& outError);

// Validates merge_pull_request's optional `merge_method` arg
// (merge/squash/rebase, default "merge" when empty). outMethod carries the
// resolved value gh's --<method> flag is built from.
bool ValidateMergeMethod(const std::string& method, std::string& outMethod, std::string& outError);

// ---------------------------------------------------------------------------
// Actual `gh`-shelling operations. `ownerRepo` is "org/repo" (see
// util/GitHubRepo.h::ParseGitHubUrl, applied to Repo::githubUrl by the
// mcp/Tools.cpp callers before reaching here).
// ---------------------------------------------------------------------------

// `gh pr list --repo <ownerRepo> --state <state> --json number,title,url,state,headRefName`
Result ListPullRequests(const std::string& ownerRepo, const std::string& state);

// `gh pr view <prNumber> --repo <ownerRepo> --json number,title,url,state,mergeable,reviewDecision`
Result GetPrStatus(const std::string& ownerRepo, int prNumber);

// status must already be one ValidateSetPrStatus accepted. `comment` is used
// as the review body for approved/changes-requested (gh pr review requires a
// body for --request-changes; see GitHubOps.cpp's placeholder-text comment
// for what happens when the caller doesn't supply one).
Result SetPrStatus(
    const std::string& ownerRepo, int prNumber, const std::string& status, const std::string& comment);

// `mergeMethod` must already be one ValidateMergeMethod resolved.
Result MergePullRequest(const std::string& ownerRepo, int prNumber, const std::string& mergeMethod);

// `gh issue list --repo <ownerRepo> --state <state> --json number,title,url,state`
Result ListIssues(const std::string& ownerRepo, const std::string& state);

// `gh issue create --repo <ownerRepo> --title <title> --body-file <tempfile>`
Result CreateIssue(const std::string& ownerRepo, const std::string& title, const std::string& body);

// status must already be one ValidateSetIssueStatus accepted ("open"/"closed").
Result SetIssueStatus(const std::string& ownerRepo, int issueNumber, const std::string& status);

} // namespace GitHubOps

#pragma once

#include <string>

#include "../db/RepoStore.h"

// Backs the import_repo / create_repo MCP tools (see mcp/Tools.cpp) — a
// synchronous, in-tool-call variant of Orchestrator::AddRepo's clone step.
//
// Deliberately NOT a call into Orchestrator::AddRepo itself: that method
// lives in the live Orchestrator process and, after cloning, kicks off the
// full repo-onboarding pipeline (RunRepoOnboarding — posting the
// repo_onboarding_alex prompt and running Alex's turn to propose a new
// repo-expert agent). None of that is reachable from here: this runs inside
// the per-turn MCP subprocess (see main.cpp's --mcp-server mode), which has
// only a Database handle onto the shared SQLite file, no live Discord
// connection, and isn't set up to run another agent's turn recursively from
// inside this one's. So this module intentionally stops short of onboarding:
// it clones the repo and writes the `repos` row, then leaves status at
// "ready" for a human (or Alex, asked normally) to onboard afterward,
// exactly like a repo that finished cloning but hasn't been onboarded yet
// through any other path.
//
// Same rule as WorkspacePr.h / WorkspaceCreator.h: plain programmatic `gh`
// subprocess calls, never an agent turn.
namespace RepoImport {

struct Result {
    bool ok = false;
    std::string repoId;
    std::string githubUrl;
    std::string error;
};

// Parses `githubUrl` (see util/GitHubRepo.h::ParseGitHubUrl for accepted
// forms), and if the resulting repo id isn't already known, clones it via
// `gh repo clone` into the same deterministic %ProgramData%\RemoteCode\Repos\<id>
// path Orchestrator::AddRepo uses, and writes the `repos` row (status
// "cloning" while the clone runs, then "ready" on success or "failed" with
// last_error set on failure). Idempotent: an already-known repo id is
// returned as-is with ok=true (no re-clone), matching Orchestrator::AddRepo.
Result Import(RepoStore& repoStore, const std::string& githubUrl, const std::string& notes);

// Creates a brand-new, empty, private GitHub repo via
// `gh repo create <name> --private`, then Imports it the same way `name`
// may be "org/repo" or a bare "repo" name (created under the authenticated
// gh user/org, whichever `gh repo create` resolves it to) — the actual
// resulting URL is parsed back out of `gh`'s own output rather than assumed,
// since a bare name's resolved owner isn't known ahead of time.
Result CreateAndImport(RepoStore& repoStore, const std::string& name, const std::string& notes);

} // namespace RepoImport

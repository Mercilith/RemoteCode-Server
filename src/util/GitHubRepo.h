#pragma once

#include <string>

// Pure parsing helpers for the repo-onboarding pipeline (see
// Orchestrator::AddRepo) — no I/O, no process spawning (see
// util/ProcessRunner.h for that).
namespace GitHubRepo {

// Accepts:
//   https://github.com/org/repo
//   https://github.com/org/repo.git
//   git@github.com:org/repo.git
//   org/repo   (bare form)
// A trailing ".git" is stripped. Rejects anything that doesn't resolve to
// exactly an org and a repo name (e.g. missing repo, extra path segments,
// empty org/repo, a non-GitHub host on the https:// form).
bool ParseGitHubUrl(const std::string& input, std::string& outOrg, std::string& outRepo);

// Slugify(org) + "__" + Slugify(repo), lowercase — the deterministic id used
// as repos.id, so adding the same repo twice is idempotent (see
// Orchestrator::AddRepo).
std::string RepoId(const std::string& org, const std::string& repo);

} // namespace GitHubRepo

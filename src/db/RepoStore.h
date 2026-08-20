#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

struct Repo {
    std::string id;         // Slugify(org) + "__" + Slugify(repo) — see util/GitHubRepo.h
    std::string githubUrl;
    std::string localPath;  // %ProgramData%\RemoteCode\Repos\<id>
    std::string agentId;    // empty until the repo-onboarding create_agent approval resolves
    std::string status;     // "cloning" | "ready" | "onboarding" | "active" | "failed"
    std::string notes;      // Cardon's notes when the repo was added, may be empty
    std::string lastError;  // set alongside status == "failed"
    bool onboardingTriggered = false; // see RepoStore::TryClaimOnboarding
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

class RepoStore {
public:
    explicit RepoStore(Database& db) : db_(db) {}

    bool Create(const Repo& repo);
    bool Get(const std::string& id, Repo& outRepo);
    // Every repo, most recently created first — used by the admin API's
    // GET /repos.
    std::vector<Repo> ListAll();

    bool SetStatus(const std::string& id, const std::string& status, int64_t updatedAt);
    bool SetAgentId(const std::string& id, const std::string& agentId, int64_t updatedAt);
    // Also sets status to "failed" — a repo with a recorded error is by
    // definition in the failed state.
    bool SetError(const std::string& id, const std::string& lastError, int64_t updatedAt);
    // Resets a failed repo back to "cloning" and clears last_error — used by
    // the retry-repo admin endpoint before re-running onboarding, so the UI
    // doesn't keep showing a stale error message while the retry is in
    // flight.
    bool ClearError(const std::string& id, int64_t updatedAt);

    // Atomically claims onboarding for a repo that hasn't had it triggered
    // yet: flips onboarding_triggered 0->1 and returns true only if THIS
    // call is the one that made the change (false if it was already 1, or
    // the repo doesn't exist) — the WHERE-guarded UPDATE plus checking
    // Database::Changes() is what makes this safe to call from a periodic
    // sweep without racing a concurrent caller into onboarding the same
    // repo twice. Callers that already know they're the sole trigger point
    // for a repo (AddRepo, RetryRepo) still call this to keep the flag
    // consistent, even though they don't need the return value.
    bool TryClaimOnboarding(const std::string& id);
    // Every "ready" repo whose onboarding hasn't been triggered yet — what
    // Orchestrator::EnsurePendingRepoOnboarding polls.
    std::vector<Repo> ListPendingOnboarding();

private:
    Database& db_;
};

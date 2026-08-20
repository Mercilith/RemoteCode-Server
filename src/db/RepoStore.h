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

private:
    Database& db_;
};

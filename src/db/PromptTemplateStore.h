#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

struct PromptTemplate {
    std::string name;
    std::string content;
    int64_t updatedAt = 0;
};

// Exact names of the two built-in, server-authored prompt templates used by
// the repo-onboarding pipeline (see Orchestrator::RunRepoOnboarding /
// HandleReaction). Referenced by both the MCP tools (mcp/Tools.cpp) and the
// admin API (http/AdminServer.cpp) so the set of valid names can't drift
// between the two surfaces.
namespace PromptTemplateNames {
constexpr const char* kRepoOnboardingAlex = "repo_onboarding_alex";
constexpr const char* kRepoOnboardingAgent = "repo_onboarding_agent";
} // namespace PromptTemplateNames

class PromptTemplateStore {
public:
    explicit PromptTemplateStore(Database& db) : db_(db) {}

    bool Get(const std::string& name, std::string& outContent);
    // Upsert.
    bool Set(const std::string& name, const std::string& content, int64_t updatedAt);
    // Every stored template (name, content, updated_at) — used by the admin
    // API's GET /prompt-templates and informationally by the MCP tools.
    std::vector<PromptTemplate> ListAll();

    // Inserts the two built-in templates (see PromptTemplateNames) with
    // their default content if the table is currently empty. Idempotent —
    // safe to call on every startup, mirrors AgentStore::SeedAlexIfEmpty.
    bool SeedDefaultsIfEmpty();

private:
    Database& db_;
};

// Renders `templateContent`, substituting the four repo-onboarding
// placeholders ({{repo_name}}, {{repo_url}}, {{local_path}}, {{notes}}).
// `notes` renders as an empty string if empty, or
// "\nCardon's notes: " + notes + "\n" if not. Pure string substitution, no
// I/O — factored out so it's independently unit-testable.
std::string RenderPromptTemplate(
    const std::string& templateContent, const std::string& repoName, const std::string& repoUrl,
    const std::string& localPath, const std::string& notes);

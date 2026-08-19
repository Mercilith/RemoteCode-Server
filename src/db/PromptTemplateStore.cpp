#include "PromptTemplateStore.h"

#include <ctime>

namespace {

int64_t NowUnix() {
    return static_cast<int64_t>(time(nullptr));
}

// Sent to Alex once a repo has finished cloning (see
// Orchestrator::RunRepoOnboarding) — asks Alex to design a new agent that's
// a dedicated expert on this repo and submit it for Cardon's approval the
// normal way.
constexpr const char* kDefaultRepoOnboardingAlex =
    "A GitHub repo was just cloned and is ready for a dedicated expert agent:\n\n"
    "  Repo: {{repo_name}}\n"
    "  URL: {{repo_url}}\n"
    "  Cloned at: {{local_path}}\n"
    "{{notes}}\n"
    "Design a new agent whose sole focus is deeply understanding and working on this "
    "specific repository — both understanding its architecture/conventions and actually "
    "doing hands-on work on it (fixes, features, investigation) when asked. Pick a name "
    "that isn't already in use by an existing agent (check with list_agents first).\n\n"
    "The system prompt you write for the new agent MUST instruct it to explore the "
    "codebase at {{local_path}} thoroughly (README, directory structure, key files, "
    "existing conventions) before doing anything else, and to treat that directory as its "
    "home base for all file/shell work on this project.\n\n"
    "Once you've settled on a name and system prompt, submit it for approval as usual via "
    "submit_agent_for_approval — this repo-expert agent goes through Cardon's normal "
    "approval workflow like any other agent.";

// Sent to the new agent once Cardon approves it (see
// Orchestrator::HandleReaction) — asks it to explore the repo and build its
// own persistent memory of it before doing anything else.
constexpr const char* kDefaultRepoOnboardingAgent =
    "Welcome — you're the dedicated agent for {{repo_name}} ({{repo_url}}), checked out "
    "locally at {{local_path}}.\n"
    "{{notes}}\n"
    "Before doing anything else, thoroughly explore the codebase at {{local_path}} using "
    "your Read/Glob/Grep tools: read the README and any top-level docs, map out the "
    "directory structure, identify the key files/modules, and note the existing coding "
    "conventions (style, patterns, testing approach, build/run instructions).\n\n"
    "Once you've built a real understanding of the project, use the remember tool to save "
    "the key facts, architecture, and conventions you'll want recalled in every future "
    "session — so you don't have to rediscover this codebase from scratch each time you're "
    "asked to work on it. Then let Cardon know you're set up and ready to work on this "
    "project.";

} // namespace

bool PromptTemplateStore::Get(const std::string& name, std::string& outContent) {
    Statement stmt(db_, "SELECT content FROM prompt_templates WHERE name = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, name);
    if (!stmt.Step()) {
        return false;
    }
    outContent = stmt.ColumnText(0);
    return true;
}

bool PromptTemplateStore::Set(const std::string& name, const std::string& content, int64_t updatedAt) {
    Statement stmt(
        db_,
        "INSERT INTO prompt_templates (name, content, updated_at) VALUES (?1,?2,?3) "
        "ON CONFLICT(name) DO UPDATE SET content=excluded.content, updated_at=excluded.updated_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, name);
    stmt.BindText(2, content);
    stmt.BindInt64(3, updatedAt);
    stmt.Step();
    return stmt.Ok();
}

std::vector<PromptTemplate> PromptTemplateStore::ListAll() {
    std::vector<PromptTemplate> templates;
    Statement stmt(db_, "SELECT name, content, updated_at FROM prompt_templates ORDER BY name;");
    if (!stmt.Valid()) {
        return templates;
    }
    while (stmt.Step()) {
        PromptTemplate t;
        t.name = stmt.ColumnText(0);
        t.content = stmt.ColumnText(1);
        t.updatedAt = stmt.ColumnInt64(2);
        templates.push_back(std::move(t));
    }
    return templates;
}

bool PromptTemplateStore::SeedDefaultsIfEmpty() {
    Statement countStmt(db_, "SELECT COUNT(*) FROM prompt_templates;");
    if (!countStmt.Valid() || !countStmt.Step()) {
        return false;
    }
    if (countStmt.ColumnInt64(0) != 0) {
        return true; // already seeded (or edited) — leave it alone
    }

    const int64_t now = NowUnix();
    return Set(PromptTemplateNames::kRepoOnboardingAlex, kDefaultRepoOnboardingAlex, now) &&
           Set(PromptTemplateNames::kRepoOnboardingAgent, kDefaultRepoOnboardingAgent, now);
}

namespace {

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
    return s;
}

} // namespace

std::string RenderPromptTemplate(
    const std::string& templateContent, const std::string& repoName, const std::string& repoUrl,
    const std::string& localPath, const std::string& notes) {
    const std::string notesRendered = notes.empty() ? "" : ("\nCardon's notes: " + notes + "\n");
    std::string out = templateContent;
    out = ReplaceAll(out, "{{repo_name}}", repoName);
    out = ReplaceAll(out, "{{repo_url}}", repoUrl);
    out = ReplaceAll(out, "{{local_path}}", localPath);
    out = ReplaceAll(out, "{{notes}}", notesRendered);
    return out;
}

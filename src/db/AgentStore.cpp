#include "AgentStore.h"

#include <ctime>

#include "../config/Secrets.h"

namespace {

int64_t NowUnix() {
    return static_cast<int64_t>(time(nullptr));
}

constexpr const char* kAlexId = "alex";
constexpr const char* kAlexName = "Alex";
constexpr const char* kAlexDescription =
    "Helps set up and configure new agents — interviews you about what an agent should do, "
    "drafts its config, and submits it for your approval.";
constexpr const char* kAlexSystemPrompt =
    "You are Alex. Your job is to help Cardon design and configure new agents for his "
    "multi-agent system. You do not build or run agents yourself — you have a conversation "
    "with Cardon to figure out what a new agent should be, then produce a structured draft "
    "(name, one-line description, full system prompt, which tools it needs, which other "
    "agents it should be allowed to message, and any starting facts it should know) and "
    "submit that draft for approval. Ask clarifying questions until you have enough to write "
    "a system prompt that would make the new agent's behavior unambiguous — vague specs "
    "produce vague agents. Prefer specific, narrow agents with clear boundaries over broad "
    "do-everything agents. Once Cardon approves a draft, it becomes a real agent; you can "
    "also propose edits to existing agents the same way. Never mark an agent as active "
    "yourself — every new agent goes through the approval workflow.";
// Enforced by Tools::Call (fail-closed) — every tool listed here must
// actually be implemented in Tools.cpp. start_chat/draft_agent are
// intentionally left out: they're not implemented yet (start_chat is being
// built in a parallel branch), and listing an unimplemented tool here would
// silently permit nothing since Tools::Call would still reject the name.
constexpr const char* kAlexToolPermissionsJson =
    R"(["post_message","read_chat","message_user","submit_agent_for_approval","update_agent","remember","list_agents","list_my_chats","start_chat"])";
constexpr const char* kAlexCanMessageJson = R"(["*"])";

} // namespace

bool AgentStore::IsEmpty() {
    Statement stmt(db_, "SELECT COUNT(*) FROM agents;");
    if (!stmt.Valid()) {
        return false;
    }
    if (!stmt.Step()) {
        return false;
    }
    return stmt.ColumnInt64(0) == 0;
}

bool AgentStore::SeedAlexIfEmpty() {
    if (!IsEmpty()) {
        return true;
    }
    Agent alex;
    alex.id = kAlexId;
    alex.name = kAlexName;
    alex.description = kAlexDescription;
    alex.systemPrompt = kAlexSystemPrompt;
    alex.status = "active";
    alex.toolPermissionsJson = kAlexToolPermissionsJson;
    alex.canMessageJson = kAlexCanMessageJson;
    alex.createdBy = "user";
    alex.createdAt = NowUnix();
    alex.updatedAt = alex.createdAt;
    return Upsert(alex);
}

namespace {

constexpr const char* kAgentColumns =
    "id, name, description, system_prompt, status, tool_permissions, can_message, created_by, "
    "created_at, updated_at, discord_bot_token_encrypted, discord_bot_user_id, discord_bot_username";

void ReadAgentRow(const Statement& stmt, Agent& out) {
    out.id = stmt.ColumnText(0);
    out.name = stmt.ColumnText(1);
    out.description = stmt.ColumnText(2);
    out.systemPrompt = stmt.ColumnText(3);
    out.status = stmt.ColumnText(4);
    out.toolPermissionsJson = stmt.ColumnText(5);
    out.canMessageJson = stmt.ColumnText(6);
    out.createdBy = stmt.ColumnText(7);
    out.createdAt = stmt.ColumnInt64(8);
    out.updatedAt = stmt.ColumnInt64(9);
    out.discordBotTokenEncrypted = stmt.ColumnIsNull(10) ? "" : stmt.ColumnText(10);
    out.discordBotUserId = stmt.ColumnIsNull(11) ? "" : stmt.ColumnText(11);
    out.discordBotUsername = stmt.ColumnIsNull(12) ? "" : stmt.ColumnText(12);
}

} // namespace

bool AgentStore::Get(const std::string& id, Agent& outAgent) {
    Statement stmt(db_, std::string("SELECT ") + kAgentColumns + " FROM agents WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadAgentRow(stmt, outAgent);
    return true;
}

std::vector<Agent> AgentStore::ListAll() {
    std::vector<Agent> agents;
    Statement stmt(db_, std::string("SELECT ") + kAgentColumns + " FROM agents ORDER BY name;");
    if (!stmt.Valid()) {
        return agents;
    }
    while (stmt.Step()) {
        Agent agent;
        ReadAgentRow(stmt, agent);
        agents.push_back(std::move(agent));
    }
    return agents;
}

bool AgentStore::Upsert(const Agent& agent) {
    Statement stmt(
        db_,
        "INSERT INTO agents (id, name, description, system_prompt, status, tool_permissions, "
        "can_message, created_by, created_at, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, description=excluded.description, "
        "system_prompt=excluded.system_prompt, status=excluded.status, "
        "tool_permissions=excluded.tool_permissions, can_message=excluded.can_message, "
        "updated_at=excluded.updated_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agent.id);
    stmt.BindText(2, agent.name);
    stmt.BindText(3, agent.description);
    stmt.BindText(4, agent.systemPrompt);
    stmt.BindText(5, agent.status);
    stmt.BindText(6, agent.toolPermissionsJson);
    stmt.BindText(7, agent.canMessageJson);
    stmt.BindText(8, agent.createdBy);
    stmt.BindInt64(9, agent.createdAt);
    stmt.BindInt64(10, agent.updatedAt);
    stmt.Step();
    return stmt.Ok();
}

bool AgentStore::SetDiscordBotToken(
    const std::string& agentId, const std::string& plaintextToken, const std::string& botUserId,
    const std::string& botUsername) {
    const std::string protectedToken = Secrets::Protect(plaintextToken);
    if (protectedToken.empty()) {
        return false;
    }
    Statement stmt(
        db_,
        "UPDATE agents SET discord_bot_token_encrypted = ?1, discord_bot_user_id = ?2, "
        "discord_bot_username = ?3, updated_at = ?4 WHERE id = ?5;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, protectedToken);
    stmt.BindText(2, botUserId);
    stmt.BindText(3, botUsername);
    stmt.BindInt64(4, NowUnix());
    stmt.BindText(5, agentId);
    stmt.Step();
    return stmt.Ok();
}

bool AgentStore::ClearDiscordBotToken(const std::string& agentId) {
    Statement stmt(
        db_,
        "UPDATE agents SET discord_bot_token_encrypted = NULL, discord_bot_user_id = NULL, "
        "discord_bot_username = NULL, updated_at = ?1 WHERE id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindInt64(1, NowUnix());
    stmt.BindText(2, agentId);
    stmt.Step();
    return stmt.Ok();
}

bool AgentStore::SetFact(const std::string& agentId, const std::string& key, const std::string& value) {
    Statement stmt(
        db_,
        "INSERT INTO agent_facts (agent_id, key, value, updated_at) VALUES (?1,?2,?3,?4) "
        "ON CONFLICT(agent_id, key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, key);
    stmt.BindText(3, value);
    stmt.BindInt64(4, NowUnix());
    stmt.Step();
    return stmt.Ok();
}

bool AgentStore::GetFact(const std::string& agentId, const std::string& key, std::string& outValue) {
    Statement stmt(db_, "SELECT value FROM agent_facts WHERE agent_id = ?1 AND key = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, key);
    if (!stmt.Step()) {
        return false;
    }
    outValue = stmt.ColumnText(0);
    return true;
}

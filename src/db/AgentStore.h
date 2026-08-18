#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

struct Agent {
    std::string id;
    std::string name;
    std::string description;
    std::string systemPrompt;
    std::string status; // "active" | "disabled" | "pending_approval"
    std::string toolPermissionsJson; // JSON array, e.g. ["post_message","read_chat"]
    std::string canMessageJson;      // JSON array of agent ids, or ["*"]
    std::string createdBy;           // "user" or an agent id
    int64_t createdAt = 0;
    int64_t updatedAt = 0;

    // Set only if this agent has its own Discord bot (assigned via the
    // admin API) instead of posting through the shared bot's webhooks.
    // discordBotTokenEncrypted is a DPAPI-protected (Secrets::Protect)
    // blob, never plaintext — decrypt via Secrets::Unprotect at the point
    // of use (see discord/AgentBotClient). All three are empty together
    // when no own bot is configured.
    std::string discordBotTokenEncrypted;
    std::string discordBotUserId;
    std::string discordBotUsername;
};

class AgentStore {
public:
    explicit AgentStore(Database& db) : db_(db) {}

    bool IsEmpty();

    // Inserts the Alex registry row (design doc Section 10) if the table
    // is empty. Idempotent — safe to call on every startup.
    bool SeedAlexIfEmpty();

    bool Get(const std::string& id, Agent& outAgent);
    // Every agent, ordered by name — used by the admin API's agent list.
    std::vector<Agent> ListAll();
    // Does not touch discordBotToken*/discordBotUserId/discordBotUsername —
    // those are managed exclusively via SetDiscordBotToken/
    // ClearDiscordBotToken below, so callers editing unrelated fields (e.g.
    // update_agent) can never accidentally wipe an assigned bot.
    bool Upsert(const Agent& agent);

    // Encrypts `plaintextToken` via Secrets::Protect and stores it plus the
    // bot's own identity (already resolved by the caller, e.g. via
    // AgentBotClient::FetchSelf — this store doesn't make network calls).
    bool SetDiscordBotToken(
        const std::string& agentId, const std::string& plaintextToken, const std::string& botUserId,
        const std::string& botUsername);
    // Reverts to the shared-webhook posting path.
    bool ClearDiscordBotToken(const std::string& agentId);

    // Durable per-agent facts (design doc Section 7) — small key/value
    // pairs injected into every turn for that agent.
    bool SetFact(const std::string& agentId, const std::string& key, const std::string& value);
    bool GetFact(const std::string& agentId, const std::string& key, std::string& outValue);

private:
    Database& db_;
};

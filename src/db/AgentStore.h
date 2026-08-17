#pragma once

#include <cstdint>
#include <string>

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
};

class AgentStore {
public:
    explicit AgentStore(Database& db) : db_(db) {}

    bool IsEmpty();

    // Inserts the Alex registry row (design doc Section 10) if the table
    // is empty. Idempotent — safe to call on every startup.
    bool SeedAlexIfEmpty();

    bool Get(const std::string& id, Agent& outAgent);
    bool Upsert(const Agent& agent);

    // Durable per-agent facts (design doc Section 7) — small key/value
    // pairs injected into every turn for that agent.
    bool SetFact(const std::string& agentId, const std::string& key, const std::string& value);
    bool GetFact(const std::string& agentId, const std::string& key, std::string& outValue);

private:
    Database& db_;
};

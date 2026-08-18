#pragma once

#include <cstdint>
#include <string>

#include "Database.h"

// CRUD for `agent_chat_sessions` — the (agent, chat) -> Claude Agent SDK
// session id mapping that lets AgentTurn resume a conversation (via the
// SDK's own `resume` option) instead of replaying the full recent-message
// history into the prompt on every turn. Table existed since the original
// schema pass but was unused until session resumption was implemented.
class AgentSessionStore {
public:
    explicit AgentSessionStore(Database& db) : db_(db) {}

    bool Get(const std::string& agentId, const std::string& chatId, std::string& outSdkSessionId);
    // Upserts — safe to call whether or not a row already exists for this
    // (agentId, chatId) pair.
    bool Set(const std::string& agentId, const std::string& chatId, const std::string& sdkSessionId);
    // Used when a resume attempt fails (e.g. the session file is gone/
    // corrupted) so the next turn falls back to a fresh session with full
    // history instead of repeatedly trying to resume a dead one.
    bool Clear(const std::string& agentId, const std::string& chatId);

private:
    Database& db_;
};

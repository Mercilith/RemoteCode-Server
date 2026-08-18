#pragma once

#include <cstdint>
#include <string>

#include "Database.h"

// CRUD for `agent_chat_sessions` — the (agent, chat) -> Claude Agent SDK
// session id mapping that lets AgentTurn resume a conversation (via the
// SDK's own `resume` option) instead of replaying the full recent-message
// history into the prompt on every turn. Table existed since the original
// schema pass but was unused until session resumption was implemented.

// Design-spec Section 12, Decision #4: a session unused for over an hour is
// stale and should not be resumed. Shared default so callers of
// GetIfFresh() outside Orchestrator.cpp (which keeps its own scoped
// constant) don't need to duplicate the literal.
constexpr int64_t kDefaultSessionIdleTimeoutSeconds = 3600;

class AgentSessionStore {
public:
    explicit AgentSessionStore(Database& db) : db_(db) {}

    bool Get(const std::string& agentId, const std::string& chatId, std::string& outSdkSessionId);
    // Like Get(), but only returns true (and fills outSdkSessionId) if the
    // stored session was last used within maxAgeSeconds of now. A session
    // older than that is treated as if it doesn't exist — not deleted here
    // (Set() will naturally overwrite it once a fresh session gets
    // established from the caller's next successful turn).
    bool GetIfFresh(
        const std::string& agentId, const std::string& chatId, int64_t maxAgeSeconds,
        std::string& outSdkSessionId);
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

#pragma once

#include <cstdint>
#include <string>

#include "Database.h"

// CRUD for `chat_summaries` — a rolling per-chat summary used to bootstrap a
// fresh Agent SDK session with bounded context instead of replaying every
// raw message ever posted (see Orchestrator::HandleIncomingMessage).
//
// The table's schema (see Schema.h) has a PK of (agent_id, chat_id), left
// over from an earlier per-agent design. Deliberately unused here: every
// agent in a chat sees the same messages, and broadcast dispatch already
// multiplies Claude calls by the number of active agents in a chat, so a
// second per-agent multiplier just for summarization would be wasteful.
// Instead this store always reads/writes under a fixed sentinel agent id
// (kSharedAgentId) so one summary is shared by the whole chat — the
// agent-scoping in the table is purely an internal storage detail, not part
// of this store's public API.
class ChatSummaryStore {
public:
    explicit ChatSummaryStore(Database& db) : db_(db) {}

    bool Get(const std::string& chatId, std::string& outSummary, int64_t& outThroughMessageId);
    // Upserts — safe to call whether or not a row already exists for this
    // chat.
    bool Set(const std::string& chatId, const std::string& summary, int64_t throughMessageId);

private:
    Database& db_;
};

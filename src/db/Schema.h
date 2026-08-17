#pragma once

#include "Database.h"

// Creates every table from the orchestration design doc's schema (Section
// 4) if not already present. All tables are created now even though this
// pass only reads/writes a subset (agents, agent_facts, chats, messages,
// chat_participants, chat_agent_webhooks) — so later passes (approvals,
// session resumption, chat summaries) aren't blocked on schema work.
class Schema {
public:
    static bool EnsureCreated(Database& db);
};

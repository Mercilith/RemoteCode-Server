#pragma once

#include <string>

#include "Database.h"

// Creates every table from the orchestration design doc's schema (Section
// 4) if not already present. All tables are created now even though this
// pass only reads/writes a subset (agents, agent_facts, chats, messages,
// chat_participants, chat_agent_webhooks) — so later passes (approvals,
// session resumption, chat summaries) aren't blocked on schema work.
class Schema {
public:
    static bool EnsureCreated(Database& db);

    // Adds `column` to `table` (with SQL type `type`, e.g. "TEXT") if it
    // doesn't already exist. SQLite has no "ADD COLUMN IF NOT EXISTS", so
    // this checks PRAGMA table_info first. `table`/`column`/`type` are
    // always internal constants, never user input, so plain string
    // concatenation into the SQL is fine here (PRAGMA/DDL identifiers can't
    // be bound as parameters anyway).
    static bool EnsureColumn(
        Database& db, const std::string& table, const std::string& column, const std::string& type);
};

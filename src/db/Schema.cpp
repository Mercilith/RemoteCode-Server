#include "Schema.h"

namespace {

constexpr const char* kCreateStatements[] = {
    R"sql(
CREATE TABLE IF NOT EXISTS agents (
    id              TEXT PRIMARY KEY,
    name            TEXT NOT NULL,
    description     TEXT NOT NULL,
    system_prompt   TEXT NOT NULL,
    status          TEXT NOT NULL,
    tool_permissions TEXT NOT NULL,
    can_message      TEXT NOT NULL,
    created_by       TEXT NOT NULL,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS agent_facts (
    agent_id    TEXT NOT NULL,
    key         TEXT NOT NULL,
    value       TEXT NOT NULL,
    updated_at  INTEGER NOT NULL,
    PRIMARY KEY (agent_id, key)
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS chat_summaries (
    agent_id    TEXT NOT NULL,
    chat_id     TEXT NOT NULL,
    summary     TEXT NOT NULL,
    through_message_id INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    PRIMARY KEY (agent_id, chat_id)
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS chats (
    id          TEXT PRIMARY KEY,
    title       TEXT,
    created_by  TEXT NOT NULL,
    status      TEXT NOT NULL,
    discord_channel_id     TEXT,
    created_at  INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS chat_agent_webhooks (
    chat_id                TEXT NOT NULL,
    agent_id                TEXT NOT NULL,
    discord_webhook_id      TEXT NOT NULL,
    discord_webhook_token   TEXT NOT NULL,
    created_at               INTEGER NOT NULL,
    PRIMARY KEY (chat_id, agent_id)
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS agent_chat_sessions (
    agent_id        TEXT NOT NULL,
    chat_id         TEXT NOT NULL,
    sdk_session_id  TEXT NOT NULL,
    last_used_at    INTEGER NOT NULL,
    PRIMARY KEY (agent_id, chat_id)
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS chat_participants (
    chat_id         TEXT NOT NULL,
    participant_type TEXT NOT NULL,
    participant_id  TEXT NOT NULL,
    joined_at       INTEGER NOT NULL,
    muted           INTEGER NOT NULL DEFAULT 0,
    mode            TEXT NOT NULL DEFAULT 'auto_respond',
    PRIMARY KEY (chat_id, participant_type, participant_id)
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    chat_id     TEXT NOT NULL,
    sender_type TEXT NOT NULL,
    sender_id   TEXT NOT NULL,
    type        TEXT NOT NULL,
    content     TEXT NOT NULL,
    metadata    TEXT,
    discord_message_id TEXT,
    created_at  INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS approvals (
    id              TEXT PRIMARY KEY,
    chat_id         TEXT NOT NULL,
    message_id      INTEGER NOT NULL,
    requested_by    TEXT NOT NULL,
    kind            TEXT NOT NULL,
    payload         TEXT NOT NULL,
    status          TEXT NOT NULL,
    created_at      INTEGER NOT NULL,
    resolved_at     INTEGER
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS repos (
    id             TEXT PRIMARY KEY,
    github_url     TEXT NOT NULL,
    local_path     TEXT NOT NULL,
    agent_id       TEXT,
    status         TEXT NOT NULL,
    notes          TEXT,
    last_error     TEXT,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS prompt_templates (
    name        TEXT PRIMARY KEY,
    content     TEXT NOT NULL,
    updated_at  INTEGER NOT NULL
);
)sql",
};

} // namespace

bool Schema::EnsureCreated(Database& db) {
    for (const char* statement : kCreateStatements) {
        if (!db.Exec(statement)) {
            return false;
        }
    }

    // Migrations for columns added after the table already existed in the
    // field — new installs get these via kCreateStatements just fine, but
    // an already-running database needs them added in place.
    if (!EnsureColumn(db, "agents", "discord_bot_token_encrypted", "TEXT")) {
        return false;
    }
    if (!EnsureColumn(db, "agents", "discord_bot_user_id", "TEXT")) {
        return false;
    }
    if (!EnsureColumn(db, "agents", "discord_bot_username", "TEXT")) {
        return false;
    }
    if (!EnsureColumn(db, "agents", "repo_local_path", "TEXT")) {
        return false;
    }
    // "listening" participant mode (chat lifecycle feature): a participant
    // whose mode is 'listening' still gets every message appended to the
    // chat's stored history (so it has full context whenever it DOES take a
    // turn), but Orchestrator::HandleIncomingMessage only actually queues a
    // turn for it when explicitly @-tagged, not on every message the way
    // 'auto_respond' (the default) participants are. See ChatStore::
    // SetParticipantMode/ListParticipantAgents.
    if (!EnsureColumn(db, "chat_participants", "mode", "TEXT NOT NULL DEFAULT 'auto_respond'")) {
        return false;
    }

    return true;
}

bool Schema::EnsureColumn(
    Database& db, const std::string& table, const std::string& column, const std::string& type) {
    Statement check(db, "PRAGMA table_info(" + table + ");");
    if (!check.Valid()) {
        return false;
    }
    while (check.Step()) {
        // table_info's result columns are (cid, name, type, notnull, dflt_value, pk).
        if (check.ColumnText(1) == column) {
            return true; // already present
        }
    }
    return db.Exec("ALTER TABLE " + table + " ADD COLUMN " + column + " " + type + ";");
}

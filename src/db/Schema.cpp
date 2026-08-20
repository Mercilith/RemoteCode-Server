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
    onboarding_triggered INTEGER NOT NULL DEFAULT 0,
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
    R"sql(
CREATE TABLE IF NOT EXISTS workspaces (
    id                   TEXT PRIMARY KEY,
    title                TEXT,
    repo_ids             TEXT NOT NULL,
    discord_category_id  TEXT,
    chat_id              TEXT NOT NULL,
    created_by           TEXT NOT NULL,
    status               TEXT NOT NULL,
    last_error           TEXT,
    created_at           INTEGER NOT NULL,
    updated_at           INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS temp_tool_grants (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    agent_id     TEXT NOT NULL,
    tool_name    TEXT NOT NULL,
    granted_at   INTEGER NOT NULL,
    consumed_at  INTEGER
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS workspace_agent_grants (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    workspace_id  TEXT NOT NULL,
    agent_id      TEXT NOT NULL,
    created_at    INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS tasks (
    id                  TEXT PRIMARY KEY,
    workspace_id        TEXT,
    chat_id             TEXT,
    title               TEXT NOT NULL,
    description         TEXT,
    status              TEXT NOT NULL,
    assignee_agent_id   TEXT,
    created_by          TEXT NOT NULL,
    created_at          INTEGER NOT NULL,
    updated_at          INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS reminders (
    id           TEXT PRIMARY KEY,
    chat_id      TEXT NOT NULL,
    message      TEXT NOT NULL,
    fire_at      INTEGER NOT NULL,
    created_by   TEXT NOT NULL,
    status       TEXT NOT NULL,
    created_at   INTEGER NOT NULL
);
)sql",
    R"sql(
CREATE TABLE IF NOT EXISTS schema_meta (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL
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
    // One-shot claim flag for repo onboarding (see Orchestrator::
    // EnsurePendingRepoOnboarding) — a repo imported via the import_repo/
    // create_repo MCP tools lands at status='ready' with no way for that
    // isolated subprocess to kick off the onboarding-chat-with-Alex flow
    // itself, so the live Orchestrator process periodically sweeps for
    // status='ready' repos that haven't been claimed yet and runs
    // RunRepoOnboarding for them. Separate from `status` because
    // RunRepoOnboarding itself unconditionally re-sets status back to
    // 'ready' partway through (right before creating the onboarding chat),
    // so status alone can't distinguish "never onboarded" from "onboarding
    // in progress" — reusing it as the gate would let the sweep re-trigger
    // (and re-prompt Alex, re-running her onboarding turn) on every poll.
    if (!EnsureColumn(db, "repos", "onboarding_triggered", "INTEGER NOT NULL DEFAULT 0")) {
        return false;
    }

    // One-time repair for a real bug (now fixed in DiscordBot::
    // HandleMessageCreate): that handler used to add every active agent
    // (plus a 'user' row) as a chat_participants row for whatever chat a
    // human's message landed in, with no check for chats that already had
    // a deliberately curated participant list — so simply typing into an
    // agent's private DM channel silently added every OTHER active agent
    // to that DM too. Orchestrator::HandleIncomingMessage's normal dispatch
    // then correctly (per its own logic) queued and ran a real turn for
    // every one of them on every message in that DM, each one showing a
    // typing indicator and burning real API usage before coming back
    // silent.
    //
    // This MUST run at most once, ever, per database — NOT on every
    // startup as the original version of this comment claimed. EnsureCreated
    // runs on every single MCP subprocess launch (see main.cpp's
    // TryRunMcpServer, invoked once per agent turn), not just when the main
    // Orchestrator process boots. And a "dm-" chat is no longer always
    // exactly one agent: Orchestrator::HandleReaction's "add_agent_to_chat"
    // approval kind deliberately grows a DM into a multi-agent chat by
    // adding a second agent as an ordinary chat_participants row (see its
    // comment). Re-running this DELETE unconditionally on every turn was
    // wiping that second agent back out again almost immediately — often
    // before they ever got a chance to act on the conversation — which is
    // indistinguishable from the agent simply never showing up. The
    // schema_meta marker makes sure this only ever fires once, against
    // whatever pre-existing corruption is on disk from the old bug; any
    // agent legitimately added to a DM after that always survives.
    {
        Statement alreadyRepaired(db, "SELECT 1 FROM schema_meta WHERE key = 'dm_participant_cleanup_v1';");
        if (!alreadyRepaired.Valid()) {
            return false;
        }
        if (!alreadyRepaired.Step()) {
            if (!db.Exec(R"sql(
DELETE FROM chat_participants
WHERE chat_id LIKE 'dm-%'
  AND NOT (
    participant_type = 'agent'
    AND (chat_id = 'dm-' || participant_id OR chat_id LIKE 'dm-' || participant_id || '-%')
  );
)sql")) {
                return false;
            }
            if (!db.Exec(
                    "INSERT INTO schema_meta (key, value) VALUES ('dm_participant_cleanup_v1', '1');")) {
                return false;
            }
        }
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

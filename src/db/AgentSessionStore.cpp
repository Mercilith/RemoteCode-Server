#include "AgentSessionStore.h"

#include <ctime>

bool AgentSessionStore::Get(
    const std::string& agentId, const std::string& chatId, std::string& outSdkSessionId) {
    Statement stmt(
        db_, "SELECT sdk_session_id FROM agent_chat_sessions WHERE agent_id = ?1 AND chat_id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, chatId);
    if (!stmt.Step()) {
        return false;
    }
    outSdkSessionId = stmt.ColumnText(0);
    return true;
}

bool AgentSessionStore::GetIfFresh(
    const std::string& agentId, const std::string& chatId, int64_t maxAgeSeconds,
    std::string& outSdkSessionId) {
    Statement stmt(
        db_,
        "SELECT sdk_session_id, last_used_at FROM agent_chat_sessions "
        "WHERE agent_id = ?1 AND chat_id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, chatId);
    if (!stmt.Step()) {
        return false;
    }
    const int64_t lastUsedAt = stmt.ColumnInt64(1);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (lastUsedAt < now - maxAgeSeconds) {
        return false;
    }
    outSdkSessionId = stmt.ColumnText(0);
    return true;
}

bool AgentSessionStore::Set(
    const std::string& agentId, const std::string& chatId, const std::string& sdkSessionId) {
    Statement stmt(
        db_,
        "INSERT INTO agent_chat_sessions (agent_id, chat_id, sdk_session_id, last_used_at) "
        "VALUES (?1,?2,?3,?4) ON CONFLICT(agent_id, chat_id) DO UPDATE SET "
        "sdk_session_id=excluded.sdk_session_id, last_used_at=excluded.last_used_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, chatId);
    stmt.BindText(3, sdkSessionId);
    stmt.BindInt64(4, static_cast<int64_t>(time(nullptr)));
    stmt.Step();
    return stmt.Ok();
}

bool AgentSessionStore::Clear(const std::string& agentId, const std::string& chatId) {
    Statement stmt(db_, "DELETE FROM agent_chat_sessions WHERE agent_id = ?1 AND chat_id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, chatId);
    stmt.Step();
    return stmt.Ok();
}

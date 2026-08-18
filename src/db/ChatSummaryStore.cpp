#include "ChatSummaryStore.h"

#include <ctime>

namespace {

// See the comment on ChatSummaryStore in the header — this is not a real
// agent id and must never collide with one. It's fine for it to be
// unresolvable via AgentStore::Get; nothing looks it up that way.
constexpr const char* kSharedAgentId = "__shared__";

} // namespace

bool ChatSummaryStore::Get(
    const std::string& chatId, std::string& outSummary, int64_t& outThroughMessageId) {
    Statement stmt(
        db_,
        "SELECT summary, through_message_id FROM chat_summaries WHERE agent_id = ?1 AND chat_id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, kSharedAgentId);
    stmt.BindText(2, chatId);
    if (!stmt.Step()) {
        return false;
    }
    outSummary = stmt.ColumnText(0);
    outThroughMessageId = stmt.ColumnInt64(1);
    return true;
}

bool ChatSummaryStore::Set(const std::string& chatId, const std::string& summary, int64_t throughMessageId) {
    Statement stmt(
        db_,
        "INSERT INTO chat_summaries (agent_id, chat_id, summary, through_message_id, updated_at) "
        "VALUES (?1,?2,?3,?4,?5) ON CONFLICT(agent_id, chat_id) DO UPDATE SET "
        "summary=excluded.summary, through_message_id=excluded.through_message_id, "
        "updated_at=excluded.updated_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, kSharedAgentId);
    stmt.BindText(2, chatId);
    stmt.BindText(3, summary);
    stmt.BindInt64(4, throughMessageId);
    stmt.BindInt64(5, static_cast<int64_t>(time(nullptr)));
    stmt.Step();
    return stmt.Ok();
}

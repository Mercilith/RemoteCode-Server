#include "ChatStore.h"

#include <algorithm>
#include <ctime>

namespace {

std::string OrEmpty(const Statement& stmt, int index) {
    return stmt.ColumnIsNull(index) ? "" : stmt.ColumnText(index);
}

} // namespace

bool ChatStore::CreateChat(const Chat& chat) {
    Statement stmt(
        db_,
        "INSERT INTO chats (id, title, created_by, status, discord_channel_id, created_at) "
        "VALUES (?1,?2,?3,?4,?5,?6);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chat.id);
    if (chat.title.empty()) {
        stmt.BindNull(2);
    } else {
        stmt.BindText(2, chat.title);
    }
    stmt.BindText(3, chat.createdBy);
    stmt.BindText(4, chat.status);
    if (chat.discordChannelId.empty()) {
        stmt.BindNull(5);
    } else {
        stmt.BindText(5, chat.discordChannelId);
    }
    stmt.BindInt64(6, chat.createdAt);
    stmt.Step();
    return stmt.Ok();
}

bool ChatStore::GetChatByDiscordChannel(const std::string& discordChannelId, Chat& outChat) {
    Statement stmt(
        db_,
        "SELECT id, title, created_by, status, discord_channel_id, created_at FROM chats "
        "WHERE discord_channel_id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, discordChannelId);
    if (!stmt.Step()) {
        return false;
    }
    outChat.id = stmt.ColumnText(0);
    outChat.title = OrEmpty(stmt, 1);
    outChat.createdBy = stmt.ColumnText(2);
    outChat.status = stmt.ColumnText(3);
    outChat.discordChannelId = OrEmpty(stmt, 4);
    outChat.createdAt = stmt.ColumnInt64(5);
    return true;
}

bool ChatStore::GetChat(const std::string& chatId, Chat& outChat) {
    Statement stmt(
        db_,
        "SELECT id, title, created_by, status, discord_channel_id, created_at FROM chats "
        "WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chatId);
    if (!stmt.Step()) {
        return false;
    }
    outChat.id = stmt.ColumnText(0);
    outChat.title = OrEmpty(stmt, 1);
    outChat.createdBy = stmt.ColumnText(2);
    outChat.status = stmt.ColumnText(3);
    outChat.discordChannelId = OrEmpty(stmt, 4);
    outChat.createdAt = stmt.ColumnInt64(5);
    return true;
}

std::vector<Chat> ChatStore::ListChats() {
    std::vector<Chat> chats;
    Statement stmt(
        db_,
        "SELECT id, title, created_by, status, discord_channel_id, created_at FROM chats "
        "ORDER BY created_at DESC;");
    if (!stmt.Valid()) {
        return chats;
    }
    while (stmt.Step()) {
        Chat c;
        c.id = stmt.ColumnText(0);
        c.title = OrEmpty(stmt, 1);
        c.createdBy = stmt.ColumnText(2);
        c.status = stmt.ColumnText(3);
        c.discordChannelId = OrEmpty(stmt, 4);
        c.createdAt = stmt.ColumnInt64(5);
        chats.push_back(std::move(c));
    }
    return chats;
}

bool ChatStore::SetChatDiscordChannel(const std::string& chatId, const std::string& discordChannelId) {
    Statement stmt(db_, "UPDATE chats SET discord_channel_id = ?1 WHERE id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, discordChannelId);
    stmt.BindText(2, chatId);
    stmt.Step();
    return stmt.Ok();
}

bool ChatStore::AddParticipant(
    const std::string& chatId, const std::string& participantType, const std::string& participantId) {
    Statement stmt(
        db_,
        "INSERT OR IGNORE INTO chat_participants (chat_id, participant_type, participant_id, "
        "joined_at, muted) VALUES (?1,?2,?3,?4,0);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chatId);
    stmt.BindText(2, participantType);
    stmt.BindText(3, participantId);
    stmt.BindInt64(4, static_cast<int64_t>(time(nullptr)));
    stmt.Step();
    return stmt.Ok();
}

bool ChatStore::IsParticipant(
    const std::string& chatId, const std::string& participantType, const std::string& participantId) {
    Statement stmt(
        db_,
        "SELECT 1 FROM chat_participants WHERE chat_id = ?1 AND participant_type = ?2 "
        "AND participant_id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chatId);
    stmt.BindText(2, participantType);
    stmt.BindText(3, participantId);
    return stmt.Step();
}

std::vector<std::string> ChatStore::ListParticipantAgentIds(const std::string& chatId) {
    std::vector<std::string> ids;
    Statement stmt(
        db_,
        "SELECT participant_id FROM chat_participants WHERE chat_id = ?1 AND participant_type = 'agent';");
    if (!stmt.Valid()) {
        return ids;
    }
    stmt.BindText(1, chatId);
    while (stmt.Step()) {
        ids.push_back(stmt.ColumnText(0));
    }
    return ids;
}

int64_t ChatStore::InsertMessage(const Message& message) {
    Statement stmt(
        db_,
        "INSERT INTO messages (chat_id, sender_type, sender_id, type, content, metadata, "
        "discord_message_id, created_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8);");
    if (!stmt.Valid()) {
        return -1;
    }
    stmt.BindText(1, message.chatId);
    stmt.BindText(2, message.senderType);
    stmt.BindText(3, message.senderId);
    stmt.BindText(4, message.type);
    stmt.BindText(5, message.content);
    if (message.metadataJson.empty()) {
        stmt.BindNull(6);
    } else {
        stmt.BindText(6, message.metadataJson);
    }
    if (message.discordMessageId.empty()) {
        stmt.BindNull(7);
    } else {
        stmt.BindText(7, message.discordMessageId);
    }
    stmt.BindInt64(8, message.createdAt);
    stmt.Step();
    if (!stmt.Ok()) {
        return -1;
    }
    return stmt.LastInsertRowId();
}

std::vector<Message> ChatStore::RecentMessages(const std::string& chatId, int limit) {
    std::vector<Message> messages;
    Statement stmt(
        db_,
        "SELECT id, chat_id, sender_type, sender_id, type, content, metadata, discord_message_id, "
        "created_at FROM messages WHERE chat_id = ?1 ORDER BY id DESC LIMIT ?2;");
    if (!stmt.Valid()) {
        return messages;
    }
    stmt.BindText(1, chatId);
    stmt.BindInt64(2, limit);
    while (stmt.Step()) {
        Message m;
        m.id = stmt.ColumnInt64(0);
        m.chatId = stmt.ColumnText(1);
        m.senderType = stmt.ColumnText(2);
        m.senderId = stmt.ColumnText(3);
        m.type = stmt.ColumnText(4);
        m.content = stmt.ColumnText(5);
        m.metadataJson = OrEmpty(stmt, 6);
        m.discordMessageId = OrEmpty(stmt, 7);
        m.createdAt = stmt.ColumnInt64(8);
        messages.push_back(std::move(m));
    }
    // Query is newest-first (for an efficient LIMIT); reverse so callers
    // get chronological order, ready to feed into a turn's context.
    std::reverse(messages.begin(), messages.end());
    return messages;
}

namespace {

void ReadMessageRow(const Statement& stmt, Message& out) {
    out.id = stmt.ColumnInt64(0);
    out.chatId = stmt.ColumnText(1);
    out.senderType = stmt.ColumnText(2);
    out.senderId = stmt.ColumnText(3);
    out.type = stmt.ColumnText(4);
    out.content = stmt.ColumnText(5);
    out.metadataJson = OrEmpty(stmt, 6);
    out.discordMessageId = OrEmpty(stmt, 7);
    out.createdAt = stmt.ColumnInt64(8);
}

} // namespace

bool ChatStore::GetMessageById(int64_t id, Message& outMessage) {
    Statement stmt(
        db_,
        "SELECT id, chat_id, sender_type, sender_id, type, content, metadata, discord_message_id, "
        "created_at FROM messages WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindInt64(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadMessageRow(stmt, outMessage);
    return true;
}

bool ChatStore::GetMessageByDiscordId(const std::string& discordMessageId, Message& outMessage) {
    Statement stmt(
        db_,
        "SELECT id, chat_id, sender_type, sender_id, type, content, metadata, discord_message_id, "
        "created_at FROM messages WHERE discord_message_id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, discordMessageId);
    if (!stmt.Step()) {
        return false;
    }
    ReadMessageRow(stmt, outMessage);
    return true;
}

bool ChatStore::SetMessageDiscordId(int64_t id, const std::string& discordMessageId) {
    Statement stmt(db_, "UPDATE messages SET discord_message_id = ?1 WHERE id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, discordMessageId);
    stmt.BindInt64(2, id);
    stmt.Step();
    return stmt.Ok();
}

std::vector<Message> ChatStore::MessagesAfter(const std::string& chatId, int64_t afterId) {
    std::vector<Message> messages;
    Statement stmt(
        db_,
        "SELECT id, chat_id, sender_type, sender_id, type, content, metadata, discord_message_id, "
        "created_at FROM messages WHERE chat_id = ?1 AND id > ?2 ORDER BY id ASC;");
    if (!stmt.Valid()) {
        return messages;
    }
    stmt.BindText(1, chatId);
    stmt.BindInt64(2, afterId);
    while (stmt.Step()) {
        Message m;
        ReadMessageRow(stmt, m);
        messages.push_back(std::move(m));
    }
    return messages;
}

int64_t ChatStore::LatestMessageId(const std::string& chatId) {
    Statement stmt(db_, "SELECT COALESCE(MAX(id), 0) FROM messages WHERE chat_id = ?1;");
    if (!stmt.Valid()) {
        return 0;
    }
    stmt.BindText(1, chatId);
    if (!stmt.Step()) {
        return 0;
    }
    return stmt.ColumnInt64(0);
}

int64_t ChatStore::LatestMessageId() {
    Statement stmt(db_, "SELECT COALESCE(MAX(id), 0) FROM messages;");
    if (!stmt.Valid() || !stmt.Step()) {
        return 0;
    }
    return stmt.ColumnInt64(0);
}

std::vector<Message> ChatStore::MessagesBySenderAfter(const std::string& senderId, int64_t afterId) {
    std::vector<Message> messages;
    Statement stmt(
        db_,
        "SELECT id, chat_id, sender_type, sender_id, type, content, metadata, discord_message_id, "
        "created_at FROM messages WHERE sender_id = ?1 AND id > ?2 ORDER BY id ASC;");
    if (!stmt.Valid()) {
        return messages;
    }
    stmt.BindText(1, senderId);
    stmt.BindInt64(2, afterId);
    while (stmt.Step()) {
        Message m;
        ReadMessageRow(stmt, m);
        messages.push_back(std::move(m));
    }
    return messages;
}

bool ChatStore::GetWebhook(
    const std::string& chatId, const std::string& agentId, std::string& outWebhookId,
    std::string& outWebhookToken) {
    Statement stmt(
        db_,
        "SELECT discord_webhook_id, discord_webhook_token FROM chat_agent_webhooks "
        "WHERE chat_id = ?1 AND agent_id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chatId);
    stmt.BindText(2, agentId);
    if (!stmt.Step()) {
        return false;
    }
    outWebhookId = stmt.ColumnText(0);
    outWebhookToken = stmt.ColumnText(1);
    return true;
}

bool ChatStore::SetWebhook(
    const std::string& chatId, const std::string& agentId, const std::string& webhookId,
    const std::string& webhookToken) {
    Statement stmt(
        db_,
        "INSERT INTO chat_agent_webhooks (chat_id, agent_id, discord_webhook_id, "
        "discord_webhook_token, created_at) VALUES (?1,?2,?3,?4,?5) "
        "ON CONFLICT(chat_id, agent_id) DO UPDATE SET discord_webhook_id=excluded.discord_webhook_id, "
        "discord_webhook_token=excluded.discord_webhook_token;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, chatId);
    stmt.BindText(2, agentId);
    stmt.BindText(3, webhookId);
    stmt.BindText(4, webhookToken);
    stmt.BindInt64(5, static_cast<int64_t>(time(nullptr)));
    stmt.Step();
    return stmt.Ok();
}

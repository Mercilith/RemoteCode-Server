#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

struct Chat {
    std::string id;
    std::string title;
    std::string createdBy; // "user" or an agent id
    std::string status;    // "active" | "archived"
    std::string discordChannelId;
    int64_t createdAt = 0;
};

struct Message {
    int64_t id = 0;
    std::string chatId;
    std::string senderType; // "agent" | "user" | "system"
    std::string senderId;
    std::string type; // "text" | "tool_call" | "tool_result" | "approval_request" | "system_event"
    std::string content;
    std::string metadataJson;      // may be empty
    std::string discordMessageId;  // may be empty
    int64_t createdAt = 0;
};

class ChatStore {
public:
    explicit ChatStore(Database& db) : db_(db) {}

    bool CreateChat(const Chat& chat);
    bool GetChatByDiscordChannel(const std::string& discordChannelId, Chat& outChat);
    bool GetChat(const std::string& chatId, Chat& outChat);

    bool AddParticipant(
        const std::string& chatId, const std::string& participantType, const std::string& participantId);
    bool IsParticipant(
        const std::string& chatId, const std::string& participantType, const std::string& participantId);
    // Agent ids participating in a chat — this pass's addressing rule is
    // simply "every agent participant of the chat responds," so this is
    // the whole of Section 6.3's logic for now.
    std::vector<std::string> ListParticipantAgentIds(const std::string& chatId);

    // Returns the new message's id, or -1 on failure.
    int64_t InsertMessage(const Message& message);
    // Most recent `limit` messages, oldest first (ready to feed straight
    // into a turn's context).
    std::vector<Message> RecentMessages(const std::string& chatId, int limit);
    bool GetMessageById(int64_t id, Message& outMessage);
    bool GetMessageByDiscordId(const std::string& discordMessageId, Message& outMessage);
    bool SetMessageDiscordId(int64_t id, const std::string& discordMessageId);
    // Messages with id > afterId, chronological order — used to find every
    // message a turn produced (the primary reply plus any post_message
    // tool calls) without needing a live channel back from the MCP
    // subprocess that wrote them.
    std::vector<Message> MessagesAfter(const std::string& chatId, int64_t afterId);
    // Highest message id in the chat, or 0 if it has none yet — the
    // "before" watermark passed to MessagesAfter to find everything a turn
    // produced.
    int64_t LatestMessageId(const std::string& chatId);

    bool GetWebhook(
        const std::string& chatId, const std::string& agentId, std::string& outWebhookId,
        std::string& outWebhookToken);
    bool SetWebhook(
        const std::string& chatId, const std::string& agentId, const std::string& webhookId,
        const std::string& webhookToken);

private:
    Database& db_;
};

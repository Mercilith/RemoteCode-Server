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

// A chat's agent participant plus its dispatch mode — see
// ChatStore::ListParticipantAgents / SetParticipantMode.
struct ParticipantAgent {
    std::string agentId;
    std::string mode; // "auto_respond" (default) | "listening"
};

namespace ParticipantMode {
constexpr const char* kAutoRespond = "auto_respond";
constexpr const char* kListening = "listening";
} // namespace ParticipantMode

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
    // Every chat, most recently created first. Archived chats are excluded
    // unless includeArchived is true — used by the debug `/debug/chats`
    // admin endpoint (which passes true, so nothing simply disappears from
    // the debug view) so a caller can pick a chat_id without querying
    // SQLite directly.
    std::vector<Chat> ListChats(bool includeArchived = false);
    // Every chat `participantId` (an agent id) currently participates in —
    // joins chats against chat_participants where participant_type='agent'.
    // Archived chats are excluded unless includeArchived is true. Used by
    // the list_my_chats MCP tool (default: archived chats don't clutter an
    // agent's own chat list).
    std::vector<Chat> ListChatsForParticipant(const std::string& participantId, bool includeArchived = false);
    // Persists a Discord channel id onto a chat that was created without
    // one yet — used once a lazily-created DM channel (see
    // Orchestrator::EnsureChannelForChat) actually exists.
    bool SetChatDiscordChannel(const std::string& chatId, const std::string& discordChannelId);
    // Sets chats.status (e.g. "active" -> "archived" via /close-chat).
    // Archiving does not touch chat_participants or messages — history is
    // preserved, only the chat stops showing up in default ListChats*
    // results and (once its Discord channel is deleted alongside this by
    // the caller) stops being reachable from Discord.
    bool SetChatStatus(const std::string& chatId, const std::string& status);

    bool AddParticipant(
        const std::string& chatId, const std::string& participantType, const std::string& participantId);
    // Drops the chat_participants row only — never touches messages, so
    // chat history is preserved. Used by /remove-agent.
    bool RemoveParticipant(
        const std::string& chatId, const std::string& participantType, const std::string& participantId);
    bool IsParticipant(
        const std::string& chatId, const std::string& participantType, const std::string& participantId);
    // Agent ids participating in a chat — this pass's addressing rule is
    // simply "every agent participant of the chat responds," so this is
    // the whole of Section 6.3's logic for now. Does NOT report mode; see
    // ListParticipantAgents for the mode-aware variant Orchestrator's
    // dispatch loop actually needs.
    std::vector<std::string> ListParticipantAgentIds(const std::string& chatId);
    // Same membership as ListParticipantAgentIds, but paired with each
    // participant's dispatch mode — what Orchestrator::HandleIncomingMessage
    // uses to decide whether an untagged agent gets queued for a turn on
    // every message ("auto_respond") or only when explicitly @-tagged
    // ("listening").
    std::vector<ParticipantAgent> ListParticipantAgents(const std::string& chatId);
    // Sets a participant's dispatch mode — see ParticipantMode above.
    // No-op (returns true) if the participant row doesn't exist to update.
    bool SetParticipantMode(
        const std::string& chatId, const std::string& participantType, const std::string& participantId,
        const std::string& mode);

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
    // Messages authored by `senderId` (any chat) with id > afterId,
    // chronological order — like MessagesAfter but scoped by sender
    // instead of chat, so it also catches a turn's post_message calls
    // targeting an explicit different chat_id, and message_user calls
    // (which always write into the agent's own separate DM chat).
    std::vector<Message> MessagesBySenderAfter(const std::string& senderId, int64_t afterId);
    // Highest message id in the chat, or 0 if it has none yet — the
    // "before" watermark passed to MessagesAfter to find everything a turn
    // produced.
    int64_t LatestMessageId(const std::string& chatId);
    // Highest message id across every chat — the global watermark for
    // MessagesBySenderAfter, since a turn's tool calls aren't confined to
    // one chat.
    int64_t LatestMessageId();

    bool GetWebhook(
        const std::string& chatId, const std::string& agentId, std::string& outWebhookId,
        std::string& outWebhookToken);
    bool SetWebhook(
        const std::string& chatId, const std::string& agentId, const std::string& webhookId,
        const std::string& webhookToken);

private:
    Database& db_;
};

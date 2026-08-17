#include "Tools.h"

#include <ctime>

using nlohmann::json;

namespace {

json PostMessage(
    ChatStore& chatStore, const std::string& agentId, const json& arguments, std::string& outError) {
    if (!arguments.contains("chat_id") || !arguments.contains("content")) {
        outError = "post_message requires 'chat_id' and 'content'";
        return {};
    }

    Message message;
    message.chatId = arguments["chat_id"].get<std::string>();
    message.senderType = "agent";
    message.senderId = agentId;
    message.type = "text";
    message.content = arguments["content"].get<std::string>();
    message.createdAt = static_cast<int64_t>(time(nullptr));

    // DB-only: this pass does not push tool-initiated posts to Discord —
    // only the primary turn reply (written by Orchestrator after the
    // worker returns) does. A future pass can have Orchestrator relay
    // newly inserted agent-authored messages via DiscordBot::PostAsAgent.
    const int64_t id = chatStore.InsertMessage(message);
    if (id < 0) {
        outError = "failed to insert message";
        return {};
    }
    return json{{"message_id", id}, {"status", "posted"}};
}

json ReadChat(ChatStore& chatStore, const json& arguments, std::string& outError) {
    if (!arguments.contains("chat_id")) {
        outError = "read_chat requires 'chat_id'";
        return {};
    }
    const std::string chatId = arguments["chat_id"].get<std::string>();
    const int limit = arguments.value("limit", 50);

    const std::vector<Message> messages = chatStore.RecentMessages(chatId, limit);
    json out = json::array();
    for (const Message& m : messages) {
        out.push_back({
            {"sender_type", m.senderType},
            {"sender_id", m.senderId},
            {"type", m.type},
            {"content", m.content},
            {"created_at", m.createdAt},
        });
    }
    return json{{"messages", out}};
}

} // namespace

json Tools::Definitions() {
    return json::array({
        {
            {"name", "post_message"},
            {"description", "Post a text message into a chat as this agent."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"chat_id", {{"type", "string"}}},
                      {"content", {{"type", "string"}}},
                  }},
                 {"required", json::array({"chat_id", "content"})},
             }},
        },
        {
            {"name", "read_chat"},
            {"description", "Read the most recent messages in a chat."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"chat_id", {{"type", "string"}}},
                      {"limit", {{"type", "integer"}}},
                  }},
                 {"required", json::array({"chat_id"})},
             }},
        },
    });
}

json Tools::Call(
    ChatStore& chatStore, const std::string& agentId, const std::string& toolName, const json& arguments,
    std::string& outError) {
    if (toolName == "post_message") {
        return PostMessage(chatStore, agentId, arguments, outError);
    }
    if (toolName == "read_chat") {
        return ReadChat(chatStore, arguments, outError);
    }
    outError = "unknown tool: " + toolName;
    return {};
}

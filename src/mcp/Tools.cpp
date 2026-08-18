#include "Tools.h"

#include <ctime>
#include <sstream>

#include "../util/Text.h"

using nlohmann::json;

namespace {

// Falls back to ctx.chatId ("the chat this turn is happening in") when the
// caller doesn't pass an explicit chat_id — the model never sees our
// internal chat ids in its context, so requiring it to supply one for the
// common case ("talk in the chat I'm already in") would be unusable.
std::string ResolveChatId(const ToolContext& ctx, const json& arguments) {
    if (arguments.contains("chat_id") && arguments["chat_id"].is_string()) {
        return arguments["chat_id"].get<std::string>();
    }
    return ctx.chatId;
}

json PostMessage(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("content")) {
        outError = "post_message requires 'content'";
        return {};
    }

    Message message;
    message.chatId = ResolveChatId(ctx, arguments);
    message.senderType = "agent";
    message.senderId = ctx.agentId;
    message.type = "text";
    message.content = arguments["content"].get<std::string>();
    message.createdAt = static_cast<int64_t>(time(nullptr));

    // DB-only: this pass does not push tool-initiated posts to Discord —
    // only the primary turn reply (written by Orchestrator after the
    // worker returns) does. A future pass can have Orchestrator relay
    // newly inserted agent-authored messages via DiscordBot::PostAsAgent.
    const int64_t id = ctx.chatStore.InsertMessage(message);
    if (id < 0) {
        outError = "failed to insert message";
        return {};
    }
    return json{{"message_id", id}, {"status", "posted"}};
}

json ReadChat(ToolContext& ctx, const json& arguments, std::string& outError) {
    const std::string chatId = ResolveChatId(ctx, arguments);
    if (chatId.empty()) {
        outError = "read_chat requires 'chat_id' (no current chat to default to)";
        return {};
    }
    const int limit = arguments.value("limit", 50);

    const std::vector<Message> messages = ctx.chatStore.RecentMessages(chatId, limit);
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

json SubmitAgentForApproval(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("name") || !arguments.contains("description") ||
        !arguments.contains("system_prompt")) {
        outError = "submit_agent_for_approval requires 'name', 'description', 'system_prompt'";
        return {};
    }
    if (ctx.chatId.empty()) {
        outError = "submit_agent_for_approval has no current chat to post the approval request into";
        return {};
    }

    const std::string name = arguments["name"].get<std::string>();
    const std::string description = arguments["description"].get<std::string>();
    const std::string systemPrompt = arguments["system_prompt"].get<std::string>();
    const json toolPermissions = arguments.value("tool_permissions", json::array());
    const json canMessage = arguments.value("can_message", json::array({"*"}));

    const std::string agentId = Slugify(name);
    Agent existing;
    if (ctx.agentStore.Get(agentId, existing)) {
        outError = "an agent with id '" + agentId + "' already exists (status: " + existing.status + ")";
        return {};
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));

    Agent draft;
    draft.id = agentId;
    draft.name = name;
    draft.description = description;
    draft.systemPrompt = systemPrompt;
    draft.status = "pending_approval";
    draft.toolPermissionsJson = toolPermissions.dump();
    draft.canMessageJson = canMessage.dump();
    draft.createdBy = ctx.agentId;
    draft.createdAt = now;
    draft.updatedAt = now;
    if (!ctx.agentStore.Upsert(draft)) {
        outError = "failed to save agent draft";
        return {};
    }

    std::ostringstream summary;
    summary << "**New agent proposed: " << name << "**\n" << description << "\n\nReact with \xE2\x9C\x85 "
            << "to approve or \xE2\x9D\x8C to reject.";

    Message message;
    message.chatId = ctx.chatId;
    message.senderType = "agent";
    message.senderId = ctx.agentId;
    message.type = "approval_request";
    message.content = summary.str();
    message.createdAt = now;
    const int64_t messageId = ctx.chatStore.InsertMessage(message);
    if (messageId < 0) {
        outError = "failed to record the approval request message";
        return {};
    }

    Approval approval;
    approval.id = "approval-" + agentId;
    approval.chatId = ctx.chatId;
    approval.messageId = messageId;
    approval.requestedBy = ctx.agentId;
    approval.kind = "create_agent";
    approval.payloadJson = json{{"agent_id", agentId}}.dump();
    approval.status = "pending";
    approval.createdAt = now;
    if (!ctx.approvalStore.Create(approval)) {
        outError = "failed to record the approval";
        return {};
    }

    return json{{"agent_id", agentId}, {"status", "pending_approval"}};
}

json UpdateAgent(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("agent_id")) {
        outError = "update_agent requires 'agent_id'";
        return {};
    }
    const std::string agentId = arguments["agent_id"].get<std::string>();

    Agent agent;
    if (!ctx.agentStore.Get(agentId, agent)) {
        outError = "no agent with id '" + agentId + "'";
        return {};
    }

    bool changed = false;
    if (arguments.contains("name")) {
        agent.name = arguments["name"].get<std::string>();
        changed = true;
    }
    if (arguments.contains("description")) {
        agent.description = arguments["description"].get<std::string>();
        changed = true;
    }
    if (arguments.contains("system_prompt")) {
        agent.systemPrompt = arguments["system_prompt"].get<std::string>();
        changed = true;
    }
    if (arguments.contains("tool_permissions")) {
        agent.toolPermissionsJson = arguments["tool_permissions"].dump();
        changed = true;
    }
    if (arguments.contains("can_message")) {
        agent.canMessageJson = arguments["can_message"].dump();
        changed = true;
    }

    if (!changed) {
        outError = "update_agent requires at least one field to change";
        return {};
    }

    agent.updatedAt = static_cast<int64_t>(time(nullptr));
    // Upsert never touches discord_bot_token*/discord_bot_user_id/
    // discord_bot_username (see AgentStore.h) — safe to reuse here even
    // though `agent` was loaded with those fields populated.
    if (!ctx.agentStore.Upsert(agent)) {
        outError = "failed to save agent update";
        return {};
    }
    return json{{"agent_id", agent.id}, {"status", agent.status}};
}

} // namespace

json Tools::Definitions() {
    return json::array({
        {
            {"name", "post_message"},
            {"description",
             "Post a text message into the current chat as this agent. Pass chat_id only to post "
             "into a different chat. To bring another agent into the conversation, tag them by id "
             "(e.g. \"@alex\") anywhere in content — only agents you tag get a follow-up turn, and "
             "only if your can_message permits messaging them; posting with no tags does not "
             "trigger anyone else."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"chat_id", {{"type", "string"}}},
                      {"content", {{"type", "string"}}},
                  }},
                 {"required", json::array({"content"})},
             }},
        },
        {
            {"name", "read_chat"},
            {"description",
             "Read the most recent messages in the current chat. Pass chat_id only to read a "
             "different chat."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"chat_id", {{"type", "string"}}},
                      {"limit", {{"type", "integer"}}},
                  }},
             }},
        },
        {
            {"name", "submit_agent_for_approval"},
            {"description",
             "Propose a new agent for Cardon to approve. Posts the draft into the current chat with "
             "checkmark/cross reactions — the agent becomes active only if Cardon reacts with the "
             "checkmark. Only call this once you have enough detail to write a system prompt that "
             "makes the new agent's behavior unambiguous."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"name", {{"type", "string"}}},
                      {"description", {{"type", "string"}, {"description", "One-line summary."}}},
                      {"system_prompt", {{"type", "string"}}},
                      {"tool_permissions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"can_message", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                  }},
                 {"required", json::array({"name", "description", "system_prompt"})},
             }},
        },
        {
            {"name", "update_agent"},
            {"description",
             "Update an existing agent's name, description, system prompt, tool_permissions, or "
             "can_message. Only the fields you pass are changed. No approval needed — used both when "
             "Cardon asks you directly to revise an agent, and in ordinary conversation."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"agent_id", {{"type", "string"}}},
                      {"name", {{"type", "string"}}},
                      {"description", {{"type", "string"}}},
                      {"system_prompt", {{"type", "string"}}},
                      {"tool_permissions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"can_message", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                  }},
                 {"required", json::array({"agent_id"})},
             }},
        },
    });
}

json Tools::Call(ToolContext& ctx, const std::string& toolName, const json& arguments, std::string& outError) {
    ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_call", json{{"tool", toolName}, {"arguments", arguments}});

    json result;
    if (toolName == "post_message") {
        result = PostMessage(ctx, arguments, outError);
    } else if (toolName == "read_chat") {
        result = ReadChat(ctx, arguments, outError);
    } else if (toolName == "submit_agent_for_approval") {
        result = SubmitAgentForApproval(ctx, arguments, outError);
    } else if (toolName == "update_agent") {
        result = UpdateAgent(ctx, arguments, outError);
    } else {
        outError = "unknown tool: " + toolName;
    }

    if (!outError.empty()) {
        ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_error", json{{"tool", toolName}, {"error", outError}});
    } else {
        ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_result", json{{"tool", toolName}, {"result", result}});
    }
    return result;
}

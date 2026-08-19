#include "Tools.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include "../orchestrator/WorkspaceCreator.h"
#include "../util/Mentions.h"
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

json MessageUser(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("content")) {
        outError = "message_user requires 'content'";
        return {};
    }

    // Target whichever DM chat is currently active for this agent, not a
    // hardcoded "dm-<agentId>" — /create-dm can retire that chat and start a
    // fresh one under a different id (see Orchestrator::HandleSlashCommandCreateDm),
    // and message_user must follow along to whatever's current rather than
    // resurrecting an archived conversation. Falls back to creating the
    // original "dm-<agentId>" id if this agent has no DM yet at all. Every
    // id used here still starts with "dm-", which is the actual signal
    // Orchestrator uses to skip @mention dispatch for these messages (a DM
    // chat only ever has one agent).
    Chat dmChat;
    if (!ctx.chatStore.GetActiveDmChatForAgent(ctx.agentId, dmChat)) {
        dmChat.id = "dm-" + ctx.agentId;
        dmChat.title = ctx.agentId + " (DM)";
        dmChat.createdBy = ctx.agentId;
        dmChat.status = "active";
        // discordChannelId left empty on purpose — Orchestrator creates the
        // real private Discord channel lazily the first time this chat has
        // something to mirror (see Orchestrator::EnsureChannelForChat), the same
        // way every other Discord side-effect happens after a turn returns.
        dmChat.createdAt = static_cast<int64_t>(time(nullptr));
        if (!ctx.chatStore.CreateChat(dmChat)) {
            outError = "failed to create the DM chat";
            return {};
        }
        ctx.chatStore.AddParticipant(dmChat.id, "agent", ctx.agentId);
    }
    const std::string& dmChatId = dmChat.id;

    Message message;
    message.chatId = dmChatId;
    message.senderType = "agent";
    message.senderId = ctx.agentId;
    message.type = "text";
    message.content = arguments["content"].get<std::string>();
    message.createdAt = static_cast<int64_t>(time(nullptr));

    const int64_t id = ctx.chatStore.InsertMessage(message);
    if (id < 0) {
        outError = "failed to insert message";
        return {};
    }
    return json{{"message_id", id}, {"status", "queued"}};
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

json Remember(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("key") || !arguments.contains("value")) {
        outError = "remember requires 'key' and 'value'";
        return {};
    }
    const std::string key = arguments["key"].get<std::string>();
    const std::string value = arguments["value"].get<std::string>();
    if (!ctx.agentStore.SetFact(ctx.agentId, key, value)) {
        outError = "failed to remember fact";
        return {};
    }
    return json{{"status", "remembered"}};
}

json ListAgents(ToolContext& ctx, const json& /*arguments*/, std::string& /*outError*/) {
    // tool_permissions/can_message are only useful to a caller that could
    // actually act on them — i.e. one that itself holds update_agent (the
    // only tool that can change another agent's permissions). Everyone else
    // gets the same id/name/description this always returned. Without this,
    // an agent asked to grant another agent a permission has no way to see
    // what that agent already has, and update_agent replaces the whole
    // array rather than merging — a real gap that showed up live: Alex
    // correctly refused to guess rather than risk wiping out an agent's
    // existing permissions when asked to grant a new one.
    Agent caller;
    bool showPermissions = false;
    if (ctx.agentStore.Get(ctx.agentId, caller)) {
        try {
            const json callerPerms = json::parse(caller.toolPermissionsJson);
            showPermissions = callerPerms.is_array() &&
                               std::find(callerPerms.begin(), callerPerms.end(), "update_agent") != callerPerms.end();
        } catch (const json::parse_error&) {
        }
    }

    const std::vector<Agent> agents = ctx.agentStore.ListAll();
    json out = json::array();
    for (const Agent& agent : agents) {
        if (agent.status != "active") {
            continue;
        }
        json entry{
            {"id", agent.id},
            {"name", agent.name},
            {"description", agent.description},
        };
        if (showPermissions) {
            try {
                entry["tool_permissions"] = json::parse(agent.toolPermissionsJson);
            } catch (const json::parse_error&) {
                entry["tool_permissions"] = json::array();
            }
            try {
                entry["can_message"] = json::parse(agent.canMessageJson);
            } catch (const json::parse_error&) {
                entry["can_message"] = json::array();
            }
        }
        out.push_back(entry);
    }
    return out;
}

json ListMyChats(ToolContext& ctx, const json& /*arguments*/, std::string& /*outError*/) {
    json out = json::array();
    for (const Chat& c : ctx.chatStore.ListChatsForParticipant(ctx.agentId)) {
        out.push_back({{"id", c.id}, {"title", c.title}});
    }
    return json{{"chats", out}};
}

json StartChat(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("participant_ids") || !arguments["participant_ids"].is_array() ||
        arguments["participant_ids"].empty()) {
        outError = "start_chat requires a non-empty 'participant_ids' array";
        return {};
    }
    if (!arguments.contains("initial_message")) {
        outError = "start_chat requires 'initial_message'";
        return {};
    }

    Agent sender;
    if (!ctx.agentStore.Get(ctx.agentId, sender)) {
        outError = "internal error: calling agent '" + ctx.agentId + "' not found";
        return {};
    }

    // Validate every target up front — reject the whole call (rather than
    // silently dropping a bad id) if any target isn't a known, active agent
    // or the caller's can_message doesn't permit messaging them.
    std::vector<std::string> participantIds;
    for (const json& idVal : arguments["participant_ids"]) {
        if (!idVal.is_string()) {
            outError = "start_chat: 'participant_ids' entries must be strings";
            return {};
        }
        const std::string targetId = idVal.get<std::string>();
        Agent target;
        if (!ctx.agentStore.Get(targetId, target) || target.status != "active") {
            outError = "start_chat: '" + targetId + "' is not a known, active agent";
            return {};
        }
        if (!Mentions::IsAllowedToMessage(sender, targetId)) {
            outError = "start_chat: you are not allowed to message '" + targetId + "' (see can_message)";
            return {};
        }
        participantIds.push_back(targetId);
    }

    const std::string title = arguments.value("title", "");
    // "agentchat-" (never "dm-", which is reserved for the single-agent DM
    // mechanism and has special handling elsewhere) plus a timestamp for
    // uniqueness — exact format doesn't matter beyond that.
    const std::string chatId =
        "agentchat-" + Slugify(title.empty() ? "chat" : title) + "-" + std::to_string(time(nullptr));

    Chat chat;
    chat.id = chatId;
    chat.title = title;
    chat.createdBy = ctx.agentId;
    chat.status = "active";
    // discordChannelId left empty on purpose — Orchestrator creates the real
    // channel lazily the first time this chat has something to mirror (see
    // Orchestrator::EnsureChannelForChat), the same way DM chats work.
    chat.createdAt = static_cast<int64_t>(time(nullptr));
    if (!ctx.chatStore.CreateChat(chat)) {
        outError = "failed to create the chat";
        return {};
    }

    // The calling agent plus every validated target join as "agent"
    // participants. No explicit "user" participant row — like message_user's
    // DM chats, human access is granted via Discord channel permissions at
    // creation time (see Orchestrator::EnsureChannelForChat), not a
    // chat_participants row.
    ctx.chatStore.AddParticipant(chatId, "agent", ctx.agentId);
    for (const std::string& id : participantIds) {
        ctx.chatStore.AddParticipant(chatId, "agent", id);
    }

    Message message;
    message.chatId = chatId;
    message.senderType = "agent";
    message.senderId = ctx.agentId;
    message.type = "text";
    message.content = arguments["initial_message"].get<std::string>();
    message.createdAt = static_cast<int64_t>(time(nullptr));
    if (ctx.chatStore.InsertMessage(message) < 0) {
        outError = "failed to insert the initial message";
        return {};
    }

    return json{{"chat_id", chatId}, {"status", "created"}};
}

json RequestAddAgentToChat(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("target_agent_id") || !arguments.contains("reason")) {
        outError = "request_add_agent_to_chat requires 'target_agent_id' and 'reason'";
        return {};
    }
    if (ctx.chatId.empty()) {
        outError = "request_add_agent_to_chat has no current chat to request into";
        return {};
    }

    const std::string targetId = arguments["target_agent_id"].get<std::string>();
    Agent target;
    if (!ctx.agentStore.Get(targetId, target) || target.status != "active") {
        outError = "request_add_agent_to_chat: '" + targetId + "' is not a known, active agent";
        return {};
    }
    if (ctx.chatStore.IsParticipant(ctx.chatId, "agent", targetId)) {
        outError = "'" + targetId + "' is already a participant of this chat";
        return {};
    }

    Agent sender;
    if (!ctx.agentStore.Get(ctx.agentId, sender)) {
        outError = "internal error: calling agent '" + ctx.agentId + "' not found";
        return {};
    }
    if (!Mentions::IsAllowedToMessage(sender, targetId)) {
        outError = "request_add_agent_to_chat: you are not allowed to message '" + targetId + "' (see can_message)";
        return {};
    }

    const std::string reason = arguments["reason"].get<std::string>();
    const int64_t now = static_cast<int64_t>(time(nullptr));

    std::ostringstream summary;
    summary << "**" << sender.name << " wants to add " << target.name << " to this chat**\n" << reason
            << "\n\nReact with \xE2\x9C\x85 to approve or \xE2\x9D\x8C to reject.";

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

    // "add_agent_to_chat" is the second approval kind (see ApprovalStore.h);
    // it reuses the exact same table shape as "create_agent" — kind is a
    // free-text discriminator and payload a free-form JSON blob, so no
    // schema change was needed, only a new kind value and payload shape
    // (chat_id + target_agent_id) that Orchestrator::HandleReaction knows
    // how to interpret.
    Approval approval;
    approval.id = "approval-add-" + ctx.chatId + "-" + targetId + "-" + std::to_string(now);
    approval.chatId = ctx.chatId;
    approval.messageId = messageId;
    approval.requestedBy = ctx.agentId;
    approval.kind = "add_agent_to_chat";
    approval.payloadJson = json{{"chat_id", ctx.chatId}, {"target_agent_id", targetId}}.dump();
    approval.status = "pending";
    approval.createdAt = now;
    if (!ctx.approvalStore.Create(approval)) {
        outError = "failed to record the approval";
        return {};
    }

    return json{{"status", "pending_approval"}, {"target_agent_id", targetId}, {"chat_id", ctx.chatId}};
}

// The only two template names this tool surface allows viewing/editing —
// deliberately not open to arbitrary new names (see PromptTemplateNames in
// db/PromptTemplateStore.h, the single source of truth both this file and
// the admin API check against).
bool IsKnownTemplateName(const std::string& name) {
    return name == PromptTemplateNames::kRepoOnboardingAlex || name == PromptTemplateNames::kRepoOnboardingAgent;
}

json GetPromptTemplate(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("name")) {
        outError = "get_prompt_template requires 'name'";
        return {};
    }
    const std::string name = arguments["name"].get<std::string>();
    if (!IsKnownTemplateName(name)) {
        outError = "unknown prompt template '" + name + "' — valid names are '" +
                    std::string(PromptTemplateNames::kRepoOnboardingAlex) + "' and '" +
                    PromptTemplateNames::kRepoOnboardingAgent + "'";
        return {};
    }
    std::string content;
    if (!ctx.promptTemplateStore.Get(name, content)) {
        outError = "no stored content for prompt template '" + name + "'";
        return {};
    }
    return json{{"name", name}, {"content", content}};
}

json UpdatePromptTemplate(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("name") || !arguments.contains("content")) {
        outError = "update_prompt_template requires 'name' and 'content'";
        return {};
    }
    const std::string name = arguments["name"].get<std::string>();
    if (!IsKnownTemplateName(name)) {
        outError = "unknown prompt template '" + name + "' — valid names are '" +
                    std::string(PromptTemplateNames::kRepoOnboardingAlex) + "' and '" +
                    PromptTemplateNames::kRepoOnboardingAgent + "'";
        return {};
    }
    const std::string content = arguments["content"].get<std::string>();
    if (!ctx.promptTemplateStore.Set(name, content, static_cast<int64_t>(time(nullptr)))) {
        outError = "failed to save prompt template '" + name + "'";
        return {};
    }
    return json{{"name", name}, {"content", content}};
}

// Runs the same core as /create-workspace (see orchestrator/WorkspaceCreator.h
// and Orchestrator::CreateWorkspace) but WITHOUT the Discord category/channel
// creation — this tool runs inside the MCP subprocess (spawned per agent
// turn, see main.cpp's --mcp-server mode), which has only a Database handle
// onto the shared SQLite file and no live Discord connection at all. The
// workspace's chat is left with an empty discord_channel_id and the
// workspace row's status stays "ready"; Orchestrator::EnsurePendingWorkspaceChannels
// (polled after every HandleIncomingMessage, back on the main process where
// a real DiscordBot exists) picks it up and creates the category/channel
// shortly afterward.
json CreateWorkspaceTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_ids_or_names") || !arguments["repo_ids_or_names"].is_array() ||
        arguments["repo_ids_or_names"].empty()) {
        outError = "create_workspace requires a non-empty 'repo_ids_or_names' array";
        return {};
    }

    std::vector<std::string> repoTokens;
    for (const json& tokenVal : arguments["repo_ids_or_names"]) {
        if (!tokenVal.is_string()) {
            outError = "create_workspace: 'repo_ids_or_names' entries must be strings";
            return {};
        }
        repoTokens.push_back(tokenVal.get<std::string>());
    }
    const std::string title = arguments.value("title", "");

    const WorkspaceCreator::Result result =
        WorkspaceCreator::Create(ctx.repoStore, ctx.workspaceStore, ctx.chatStore, repoTokens, title, ctx.agentId);
    if (!result.ok) {
        outError = result.error;
        return {};
    }

    json repoIds = json::array();
    for (const std::string& id : result.repoIds) {
        repoIds.push_back(id);
    }
    return json{
        {"workspace_id", result.workspaceId},
        {"chat_id", result.chatId},
        {"repo_ids", repoIds},
        {"status", "ready"},
        {"note",
         "Worktrees and the chat are set up; the Discord category/channel are being created and will "
         "appear shortly."},
    };
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
            {"name", "message_user"},
            {"description",
             "Send Cardon something privately — a status update, an escalation, an answer meant just "
             "for him — instead of posting it in whatever group chat this turn is running in. Always "
             "goes to your own private DM channel with him, created automatically the first time you "
             "use this. Use post_message for anything meant for the group instead."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", {{"content", {{"type", "string"}}}}},
                 {"required", json::array({"content"})},
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
        {
            {"name", "remember"},
            {"description",
             "Store a durable fact about yourself as a key/value pair — persists across turns and "
             "gets injected into your future turns automatically. Use this for things worth "
             "remembering long-term (a preference Cardon stated, a decision that was made, context "
             "you'd otherwise have to re-derive), not for anything transient."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"key", {{"type", "string"}}},
                      {"value", {{"type", "string"}}},
                  }},
                 {"required", json::array({"key", "value"})},
             }},
        },
        {
            {"name", "list_agents"},
            {"description",
             "List every currently active agent (id, name, one-line description). Use this to check "
             "what already exists before proposing a new agent, so you don't create something that "
             "duplicates or overlaps an existing one. If you yourself hold update_agent, each entry "
             "also includes that agent's current tool_permissions and can_message — check this before "
             "calling update_agent to change another agent's permissions, since update_agent replaces "
             "the whole array rather than merging into it."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", json::object()},
             }},
        },
        {
            {"name", "list_my_chats"},
            {"description", "List every chat you currently participate in, with their ids and titles."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", json::object()},
             }},
        },
        {
            {"name", "start_chat"},
            {"description",
             "Start a new multi-agent chat with one or more other agents and post an initial message "
             "into it. Every id in participant_ids must be a known, active agent that your can_message "
             "permits messaging — the call is rejected outright if any target fails either check. "
             "Returns the new chat's id (pass it as chat_id to post_message/read_chat afterward)."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"participant_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"title", {{"type", "string"}}},
                      {"initial_message", {{"type", "string"}}},
                  }},
                 {"required", json::array({"participant_ids", "initial_message"})},
             }},
        },
        {
            {"name", "request_add_agent_to_chat"},
            {"description",
             "Ask Cardon to add another agent into the current chat (a group chat or your DM with him) — "
             "posts a request with checkmark/cross reactions; the target only actually joins once Cardon "
             "approves. Rejected outright if the target isn't a known active agent, is already in this "
             "chat, or your can_message doesn't permit messaging them."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"target_agent_id", {{"type", "string"}}},
                      {"reason", {{"type", "string"}, {"description", "Why this agent should join."}}},
                  }},
                 {"required", json::array({"target_agent_id", "reason"})},
             }},
        },
        {
            {"name", "get_prompt_template"},
            {"description",
             "Read one of the two built-in, server-authored repo-onboarding prompt templates by name "
             "('repo_onboarding_alex' or 'repo_onboarding_agent') — the prompts sent to design/onboard a "
             "new repo-expert agent. Useful to check current wording before proposing an edit."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", {{"name", {{"type", "string"}}}}},
                 {"required", json::array({"name"})},
             }},
        },
        {
            {"name", "update_prompt_template"},
            {"description",
             "Edit one of the two built-in repo-onboarding prompt templates ('repo_onboarding_alex' or "
             "'repo_onboarding_agent'). Rejected for any other name — this cannot create new templates, "
             "only edit the two that already exist. Keep the {{repo_name}}, {{repo_url}}, "
             "{{local_path}}, {{notes}} placeholders intact unless you mean to change how the pipeline "
             "fills them in."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"name", {{"type", "string"}}},
                      {"content", {{"type", "string"}}},
                  }},
                 {"required", json::array({"name", "content"})},
             }},
        },
        {
            {"name", "create_workspace"},
            {"description",
             "Create a workspace: a git worktree of each given already-imported repo, all living "
             "together, plus a new private Discord category with an initial conversation channel. Any "
             "other already-imported repo whose manifest (package.json/pubspec.yaml/CMakeLists.txt/"
             "*.csproj) mentions one of the requested repos by name is pulled in automatically as a "
             "best-effort dependency guess. Each involved repo's dedicated agent is added to the initial "
             "conversation in listening mode. Use this when asked to work across multiple related repos "
             "at once (e.g. fixing a bug that spans a project and its dependency)."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_ids_or_names",
                       {{"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Already-imported repo ids or names, e.g. [\"my-org__my-repo\"]."}}},
                      {"title", {{"type", "string"}}},
                  }},
                 {"required", json::array({"repo_ids_or_names"})},
             }},
        },
    });
}

namespace {

// Fails closed: inserts a system_event chat message (visible, per the design
// doc's "log a system_event" requirement) recording the denied call. The
// JSON activity log's tool_error entry is already written by the caller
// (ActivityLog wraps every Tools::Call), so this only needs to add the
// chat-visible record.
void LogPermissionDenied(ToolContext& ctx, const std::string& toolName) {
    if (ctx.chatId.empty()) {
        return;
    }
    Message message;
    message.chatId = ctx.chatId;
    message.senderType = "system";
    message.senderId = ctx.agentId;
    message.type = "system_event";
    message.content = "Blocked: agent '" + ctx.agentId + "' attempted to call '" + toolName +
                       "' without permission.";
    message.createdAt = static_cast<int64_t>(time(nullptr));
    ctx.chatStore.InsertMessage(message);
}

} // namespace

json Tools::Call(ToolContext& ctx, const std::string& toolName, const json& arguments, std::string& outError) {
    ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_call", json{{"tool", toolName}, {"arguments", arguments}});

    Agent agent;
    bool permitted = false;
    if (!ctx.agentStore.Get(ctx.agentId, agent)) {
        outError = "unknown agent: " + ctx.agentId;
    } else {
        json permissions;
        try {
            permissions = json::parse(agent.toolPermissionsJson);
        } catch (const json::parse_error&) {
            permissions = json::array();
        }
        permitted = permissions.is_array() &&
                    std::find(permissions.begin(), permissions.end(), toolName) != permissions.end();
        if (!permitted) {
            outError = "agent '" + ctx.agentId + "' is not permitted to call tool '" + toolName + "'";
        }
    }

    if (!outError.empty()) {
        LogPermissionDenied(ctx, toolName);
        ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_error", json{{"tool", toolName}, {"error", outError}});
        return {};
    }

    json result;
    if (toolName == "post_message") {
        result = PostMessage(ctx, arguments, outError);
    } else if (toolName == "read_chat") {
        result = ReadChat(ctx, arguments, outError);
    } else if (toolName == "message_user") {
        result = MessageUser(ctx, arguments, outError);
    } else if (toolName == "submit_agent_for_approval") {
        result = SubmitAgentForApproval(ctx, arguments, outError);
    } else if (toolName == "update_agent") {
        result = UpdateAgent(ctx, arguments, outError);
    } else if (toolName == "remember") {
        result = Remember(ctx, arguments, outError);
    } else if (toolName == "list_agents") {
        result = ListAgents(ctx, arguments, outError);
    } else if (toolName == "list_my_chats") {
        result = ListMyChats(ctx, arguments, outError);
    } else if (toolName == "start_chat") {
        result = StartChat(ctx, arguments, outError);
    } else if (toolName == "request_add_agent_to_chat") {
        result = RequestAddAgentToChat(ctx, arguments, outError);
    } else if (toolName == "get_prompt_template") {
        result = GetPromptTemplate(ctx, arguments, outError);
    } else if (toolName == "update_prompt_template") {
        result = UpdatePromptTemplate(ctx, arguments, outError);
    } else if (toolName == "create_workspace") {
        result = CreateWorkspaceTool(ctx, arguments, outError);
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

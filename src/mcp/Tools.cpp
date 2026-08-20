#include "Tools.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <sstream>

#include "../orchestrator/WorkspaceCreator.h"
#include "../orchestrator/WorkspacePr.h"
#include "../orchestrator/GitHubOps.h"
#include "../orchestrator/RepoImport.h"
#include "../util/GitHubRepo.h"
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

// post_message/read_chat are the only two chat_id-taking tools that don't
// already gate cross-chat targeting behind participation (start_chat,
// request_add_agent_to_chat, and the workspace-join tool all check
// IsParticipant/can_message before touching another chat) — an agent could
// otherwise pass any chat_id it can guess or discover (e.g. "dm-<agentId>"
// for any other agent, a well-established, easily-derived naming
// convention in this codebase) and read or write into a chat it was never
// added to, DM or not. Only gates an *explicit* chat_id argument — the
// default (no chat_id, resolving to ctx.chatId) is left unchecked since
// Orchestrator only ever dispatches a turn into a chat the agent is already
// a participant of, so it's safe by construction and checking it anyway
// would just be redundant (and require every existing test fixture that
// constructs a scoped McpServer to also seed a matching chat_participants
// row, for no real safety gain).
bool CallerIsParticipant(ToolContext& ctx, const std::string& chatId) {
    return ctx.chatStore.IsParticipant(chatId, "agent", ctx.agentId);
}

json PostMessage(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("content")) {
        outError = "post_message requires 'content'";
        return {};
    }
    const std::string chatId = ResolveChatId(ctx, arguments);
    if (arguments.contains("chat_id") && !CallerIsParticipant(ctx, chatId)) {
        outError = "post_message: you are not a participant of that chat";
        return {};
    }

    Message message;
    message.chatId = chatId;
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
    if (arguments.contains("chat_id") && !CallerIsParticipant(ctx, chatId)) {
        outError = "read_chat: you are not a participant of that chat";
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

// DB-only, same split as CreateWorkspaceTool — this tool runs in the MCP
// subprocess with no live Discord connection, so granting the joining
// agent's own bot access to the workspace's already-existing channel has to
// happen back on the main process; see WorkspaceStore::AddPendingAgentGrant
// and Orchestrator::SyncPendingWorkspaceAgentGrants.
json AddAgentToWorkspace(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("workspace_id") || !arguments.contains("agent_id")) {
        outError = "add_agent_to_workspace requires 'workspace_id' and 'agent_id'";
        return {};
    }
    const std::string workspaceId = arguments["workspace_id"].get<std::string>();
    const std::string targetAgentId = arguments["agent_id"].get<std::string>();

    Workspace workspace;
    if (!ctx.workspaceStore.Get(workspaceId, workspace)) {
        outError = "add_agent_to_workspace: no workspace with id '" + workspaceId + "'";
        return {};
    }
    Agent target;
    if (!ctx.agentStore.Get(targetAgentId, target) || target.status != "active") {
        outError = "add_agent_to_workspace: '" + targetAgentId + "' is not a known, active agent";
        return {};
    }
    if (ctx.chatStore.IsParticipant(workspace.chatId, "agent", targetAgentId)) {
        outError = "'" + targetAgentId + "' is already part of workspace '" + workspaceId + "'";
        return {};
    }

    Agent caller;
    if (!ctx.agentStore.Get(ctx.agentId, caller)) {
        outError = "internal error: calling agent '" + ctx.agentId + "' not found";
        return {};
    }
    if (!Mentions::IsAllowedToMessage(caller, targetAgentId)) {
        outError = "add_agent_to_workspace: you are not allowed to message '" + targetAgentId +
                    "' (see can_message)";
        return {};
    }

    ctx.chatStore.AddParticipant(workspace.chatId, "agent", targetAgentId);
    // New workspace participants default to listening-only — the same rule
    // a workspace's initial participants get (see WorkspaceCreator::Create):
    // they gather context but only take a turn once explicitly @-tagged,
    // until Cardon or an agent with permission flips them to auto_respond.
    ctx.chatStore.SetParticipantMode(workspace.chatId, "agent", targetAgentId, ParticipantMode::kListening);
    ctx.workspaceStore.AddPendingAgentGrant(workspaceId, targetAgentId, static_cast<int64_t>(time(nullptr)));

    return json{{"workspace_id", workspaceId}, {"agent_id", targetAgentId}, {"status", "added"}};
}

// Every real tool name this server knows — used to validate
// request_temporary_permission's tool_name argument. Kept as a flat list
// (rather than deriving it from Tools::Definitions()) since this function
// already has to be updated by hand whenever a new tool is added anyway
// (Definitions() and the dispatch chain in Tools::Call both do too).
// request_temporary_permission itself is deliberately excluded — it's
// already unconditionally available (see IsAlwaysAllowedTool below), so
// requesting temporary access to it would be meaningless.
bool IsKnownToolName(const std::string& name) {
    static const std::vector<std::string> kKnown = {
        "post_message",     "read_chat",         "message_user",        "submit_agent_for_approval",
        "update_agent",     "remember",          "list_agents",         "list_my_chats",
        "start_chat",       "request_add_agent_to_chat", "get_prompt_template", "update_prompt_template",
        "create_workspace", "add_agent_to_workspace",    "create_pull_request",
        "list_pull_requests", "get_pr_status",     "set_pr_status",       "merge_pull_request",
        "list_issues",       "create_issue",       "set_issue_status",
        "list_repos",         "import_repo",        "create_repo",
        "create_task",        "update_task_status",  "list_tasks",
        "schedule_reminder",  "list_reminders",      "cancel_reminder",
    };
    return std::find(kKnown.begin(), kKnown.end(), name) != kKnown.end();
}

json RequestTemporaryPermission(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("tool_name") || !arguments.contains("reason")) {
        outError = "request_temporary_permission requires 'tool_name' and 'reason'";
        return {};
    }
    const std::string toolName = arguments["tool_name"].get<std::string>();
    if (!IsKnownToolName(toolName)) {
        outError = "request_temporary_permission: '" + toolName + "' is not a real tool name";
        return {};
    }
    if (ctx.chatId.empty()) {
        outError = "request_temporary_permission has no current chat to request into";
        return {};
    }

    Agent caller;
    if (!ctx.agentStore.Get(ctx.agentId, caller)) {
        outError = "internal error: calling agent '" + ctx.agentId + "' not found";
        return {};
    }
    json permissions;
    try {
        permissions = json::parse(caller.toolPermissionsJson);
    } catch (const json::parse_error&) {
        permissions = json::array();
    }
    if (permissions.is_array() && std::find(permissions.begin(), permissions.end(), toolName) != permissions.end()) {
        return json{{"status", "already_permitted"}, {"tool_name", toolName}};
    }

    const std::string reason = arguments["reason"].get<std::string>();
    const int64_t now = static_cast<int64_t>(time(nullptr));

    std::ostringstream summary;
    summary << "**" << caller.name << " requests one-time use of '" << toolName << "'**\n" << reason
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

    // "temp_tool_permission" is a third approval kind — reuses the same
    // free-text kind/payload shape as add_agent_to_chat/create_agent (see
    // ApprovalStore.h); Orchestrator::HandleReaction knows how to interpret
    // this specific payload shape (agent_id + tool_name) on approval.
    Approval approval;
    approval.id = "approval-temp-" + ctx.agentId + "-" + toolName + "-" + std::to_string(now);
    approval.chatId = ctx.chatId;
    approval.messageId = messageId;
    approval.requestedBy = ctx.agentId;
    approval.kind = "temp_tool_permission";
    approval.payloadJson = json{{"agent_id", ctx.agentId}, {"tool_name", toolName}}.dump();
    approval.status = "pending";
    approval.createdAt = now;
    if (!ctx.approvalStore.Create(approval)) {
        outError = "failed to record the approval";
        return {};
    }

    return json{{"status", "pending_approval"}, {"tool_name", toolName}};
}

// Pushes the workspace repo's worktree branch and opens a PR via `gh` — see
// orchestrator/WorkspacePr.h. The actual git/gh subprocess work happens
// there (plain programmatic calls, never an agent turn); this tool is just
// argument validation plus a call into it. Unlike create_workspace/
// add_agent_to_workspace, this genuinely needs to run synchronously here in
// the MCP subprocess (not deferred to the main process) since it has no
// Discord side effect to defer — git push and gh pr create work the same
// regardless of which process calls them, so there's no reason to split it
// the way the Discord-touching tools have to be.
json CreatePullRequestTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("workspace_id") || !arguments.contains("repo_id") || !arguments.contains("title") ||
        !arguments.contains("body")) {
        outError = "create_pull_request requires 'workspace_id', 'repo_id', 'title', and 'body'";
        return {};
    }
    const std::string workspaceId = arguments["workspace_id"].get<std::string>();
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const std::string title = arguments["title"].get<std::string>();
    const std::string body = arguments["body"].get<std::string>();
    const std::string baseBranch = arguments.value("base_branch", "");

    Workspace workspace;
    if (!ctx.workspaceStore.Get(workspaceId, workspace)) {
        outError = "create_pull_request: no workspace with id '" + workspaceId + "'";
        return {};
    }
    json workspaceRepoIds;
    try {
        workspaceRepoIds = json::parse(workspace.repoIdsJson);
    } catch (const json::parse_error&) {
        workspaceRepoIds = json::array();
    }
    if (!workspaceRepoIds.is_array() ||
        std::find(workspaceRepoIds.begin(), workspaceRepoIds.end(), repoId) == workspaceRepoIds.end()) {
        outError = "create_pull_request: '" + repoId + "' is not part of workspace '" + workspaceId + "'";
        return {};
    }
    Repo repo;
    if (!ctx.repoStore.Get(repoId, repo)) {
        outError = "create_pull_request: unknown repo '" + repoId + "'";
        return {};
    }

    const WorkspacePr::Result result = WorkspacePr::Create(workspaceId, repoId, title, body, baseBranch);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"pr_url", result.prUrl}, {"workspace_id", workspaceId}, {"repo_id", repoId}};
}

// Resolves a repo_id (RepoStore's id, e.g. "someorg__somerepo") to the
// "org/repo" form gh's --repo flag wants, via Repo::githubUrl +
// GitHubRepo::ParseGitHubUrl (the same helper Orchestrator::AddRepo uses to
// validate a repo's URL on the way in — githubUrl is trusted to already
// parse cleanly by the time a repo row exists). Shared by every gh-backed
// PR/issue tool below, none of which need a local worktree the way
// create_pull_request does (see GitHubOps.h's module comment).
bool ResolveOwnerRepo(ToolContext& ctx, const std::string& repoId, std::string& outOwnerRepo, std::string& outError) {
    Repo repo;
    if (!ctx.repoStore.Get(repoId, repo)) {
        outError = "unknown repo '" + repoId + "'";
        return false;
    }
    std::string org, name;
    if (!GitHubRepo::ParseGitHubUrl(repo.githubUrl, org, name)) {
        outError = "repo '" + repoId + "' has an unparseable github_url ('" + repo.githubUrl + "')";
        return false;
    }
    outOwnerRepo = org + "/" + name;
    return true;
}

// list_pull_requests — `status` defaults to "open"; gh's own PR-list field
// name for the branch (headRefName) is remapped to `branch` in the tool's
// output to match the plainer name the spec/agents expect.
json ListPullRequestsTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id")) {
        outError = "list_pull_requests requires 'repo_id'";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const std::string statusArg = arguments.value("status", std::string());

    std::string state;
    if (!GitHubOps::ResolveListState(statusArg, /*forPr=*/true, state, outError)) {
        return {};
    }
    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::ListPullRequests(ownerRepo, state);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    json pullRequests = json::array();
    for (const json& pr : result.data) {
        pullRequests.push_back(json{
            {"number", pr.value("number", 0)},
            {"title", pr.value("title", "")},
            {"url", pr.value("url", "")},
            {"state", pr.value("state", "")},
            {"branch", pr.value("headRefName", "")},
        });
    }
    return json{{"repo_id", repoId}, {"pull_requests", pullRequests}};
}

// get_pr_status
json GetPrStatusTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id") || !arguments.contains("pr_number")) {
        outError = "get_pr_status requires 'repo_id' and 'pr_number'";
        return {};
    }
    if (!arguments["pr_number"].is_number_integer()) {
        outError = "get_pr_status: 'pr_number' must be an integer";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const int prNumber = arguments["pr_number"].get<int>();

    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::GetPrStatus(ownerRepo, prNumber);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{
        {"repo_id", repoId},
        {"pr_number", result.data.value("number", prNumber)},
        {"title", result.data.value("title", "")},
        {"url", result.data.value("url", "")},
        {"state", result.data.value("state", "")},
        {"mergeable", result.data.value("mergeable", "")},
        {"review_decision", result.data.value("reviewDecision", "")},
    };
}

// set_pr_status — deliberately does not accept "merged" (see
// GitHubOps::ValidateSetPrStatus's error message): merging is a materially
// bigger action than any of the four states this tool actually covers, so it
// gets its own explicit tool (merge_pull_request) an agent has to be
// separately permissioned for, rather than being reachable as just another
// status string here. `comment` is optional and doubles as the review body
// for 'approved'/'changes-requested' — gh's --request-changes requires a
// non-empty body, so GitHubOps::SetPrStatus substitutes a placeholder when
// the caller (agent) didn't provide one rather than failing the call outright.
json SetPrStatusTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id") || !arguments.contains("pr_number") || !arguments.contains("status")) {
        outError = "set_pr_status requires 'repo_id', 'pr_number', and 'status'";
        return {};
    }
    if (!arguments["pr_number"].is_number_integer()) {
        outError = "set_pr_status: 'pr_number' must be an integer";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const int prNumber = arguments["pr_number"].get<int>();
    const std::string status = arguments["status"].get<std::string>();
    const std::string comment = arguments.value("comment", std::string());

    if (!GitHubOps::ValidateSetPrStatus(status, outError)) {
        return {};
    }
    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::SetPrStatus(ownerRepo, prNumber, status, comment);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", repoId}, {"pr_number", prNumber}, {"status", status}};
}

// merge_pull_request
json MergePullRequestTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id") || !arguments.contains("pr_number")) {
        outError = "merge_pull_request requires 'repo_id' and 'pr_number'";
        return {};
    }
    if (!arguments["pr_number"].is_number_integer()) {
        outError = "merge_pull_request: 'pr_number' must be an integer";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const int prNumber = arguments["pr_number"].get<int>();
    const std::string methodArg = arguments.value("merge_method", std::string());

    std::string mergeMethod;
    if (!GitHubOps::ValidateMergeMethod(methodArg, mergeMethod, outError)) {
        return {};
    }
    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::MergePullRequest(ownerRepo, prNumber, mergeMethod);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", repoId}, {"pr_number", prNumber}, {"merge_method", mergeMethod}, {"result", result.text}};
}

// list_issues
json ListIssuesTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id")) {
        outError = "list_issues requires 'repo_id'";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const std::string statusArg = arguments.value("status", std::string());

    std::string state;
    if (!GitHubOps::ResolveListState(statusArg, /*forPr=*/false, state, outError)) {
        return {};
    }
    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::ListIssues(ownerRepo, state);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    json issues = json::array();
    for (const json& issue : result.data) {
        issues.push_back(json{
            {"number", issue.value("number", 0)},
            {"title", issue.value("title", "")},
            {"url", issue.value("url", "")},
            {"state", issue.value("state", "")},
        });
    }
    return json{{"repo_id", repoId}, {"issues", issues}};
}

// create_issue
json CreateIssueTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id") || !arguments.contains("title")) {
        outError = "create_issue requires 'repo_id' and 'title'";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const std::string title = arguments["title"].get<std::string>();
    const std::string body = arguments.value("body", std::string());

    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::CreateIssue(ownerRepo, title, body);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", repoId}, {"issue_url", result.text}};
}

// set_issue_status
json SetIssueStatusTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("repo_id") || !arguments.contains("issue_number") || !arguments.contains("status")) {
        outError = "set_issue_status requires 'repo_id', 'issue_number', and 'status'";
        return {};
    }
    if (!arguments["issue_number"].is_number_integer()) {
        outError = "set_issue_status: 'issue_number' must be an integer";
        return {};
    }
    const std::string repoId = arguments["repo_id"].get<std::string>();
    const int issueNumber = arguments["issue_number"].get<int>();
    const std::string status = arguments["status"].get<std::string>();

    if (!GitHubOps::ValidateSetIssueStatus(status, outError)) {
        return {};
    }
    std::string ownerRepo;
    if (!ResolveOwnerRepo(ctx, repoId, ownerRepo, outError)) {
        return {};
    }

    const GitHubOps::Result result = GitHubOps::SetIssueStatus(ownerRepo, issueNumber, status);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", repoId}, {"issue_number", issueNumber}, {"status", status}};
}

// list_repos — read-only, trivial.
json ListReposTool(ToolContext& ctx, const json& /*arguments*/, std::string& /*outError*/) {
    json out = json::array();
    for (const Repo& repo : ctx.repoStore.ListAll()) {
        out.push_back(json{
            {"id", repo.id},
            {"github_url", repo.githubUrl},
            {"status", repo.status},
            {"agent_id", repo.agentId},
            {"notes", repo.notes},
        });
    }
    return json{{"repos", out}};
}

// import_repo — see orchestrator/RepoImport.h's header comment for why this
// deliberately duplicates just the clone+RepoStore::Create half of
// Orchestrator::AddRepo rather than calling AddRepo itself: this tool runs
// in the MCP subprocess, which can't run the full repo-onboarding pipeline
// (posting the onboarding prompt and running Alex's turn) the way AddRepo
// does afterward. The repo is left at status "ready" — a human, or an agent
// asked normally, onboards it from there.
json ImportRepoTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("github_url")) {
        outError = "import_repo requires 'github_url'";
        return {};
    }
    const std::string githubUrl = arguments["github_url"].get<std::string>();
    const std::string notes = arguments.value("notes", std::string());

    const RepoImport::Result result = RepoImport::Import(ctx.repoStore, githubUrl, notes);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", result.repoId}, {"github_url", result.githubUrl}, {"status", "ready"}};
}

// create_repo — creates a brand-new, empty, private GitHub repo via `gh repo
// create`, then imports it exactly like import_repo. Same scoping decision:
// no onboarding chat, status left at "ready".
json CreateRepoTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("name")) {
        outError = "create_repo requires 'name'";
        return {};
    }
    const std::string name = arguments["name"].get<std::string>();
    const std::string notes = arguments.value("notes", std::string());

    const RepoImport::Result result = RepoImport::CreateAndImport(ctx.repoStore, name, notes);
    if (!result.ok) {
        outError = result.error;
        return {};
    }
    return json{{"repo_id", result.repoId}, {"github_url", result.githubUrl}, {"status", "ready"}};
}

// create_task
json CreateTaskTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("title")) {
        outError = "create_task requires 'title'";
        return {};
    }
    const std::string title = arguments["title"].get<std::string>();
    const std::string workspaceId = arguments.value("workspace_id", std::string());
    const std::string assigneeAgentId = arguments.value("assignee_agent_id", std::string());
    const std::string description = arguments.value("description", std::string());

    const int64_t now = static_cast<int64_t>(time(nullptr));
    // Same counter-plus-timestamp uniqueness rule as RequestNewTool's chat
    // ids above — guards against two create_task calls landing in the same
    // second within one MCP subprocess.
    static std::atomic<int> taskCounter{0};

    Task task;
    task.id = "task-" + std::to_string(now) + "-" + std::to_string(++taskCounter);
    task.workspaceId = workspaceId;
    // Only defaults chat_id to the current chat when no workspace_id was
    // given — a workspace-scoped task's natural home is the workspace's own
    // conversation (looked up by whoever reads the task back), not
    // necessarily whatever chat this turn happens to be running in.
    task.chatId = workspaceId.empty() ? ctx.chatId : std::string();
    task.title = title;
    task.description = description;
    task.status = TaskStatus::kNotStarted;
    task.assigneeAgentId = assigneeAgentId;
    task.createdBy = ctx.agentId;
    task.createdAt = now;
    task.updatedAt = now;
    if (!ctx.taskStore.Create(task)) {
        outError = "failed to save the new task";
        return {};
    }
    return json{{"task_id", task.id}, {"status", task.status}};
}

// update_task_status — assignee_agent_id is optional and reassigns in the
// same call only when actually passed (an omitted argument leaves the
// existing assignee untouched, an empty string explicitly unassigns).
json UpdateTaskStatusTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("task_id") || !arguments.contains("status")) {
        outError = "update_task_status requires 'task_id' and 'status'";
        return {};
    }
    const std::string taskId = arguments["task_id"].get<std::string>();
    const std::string status = arguments["status"].get<std::string>();
    if (!TaskStatus::IsValid(status)) {
        outError = "status must be one of " + TaskStatus::ValidList() + " (got '" + status + "')";
        return {};
    }
    Task existing;
    if (!ctx.taskStore.Get(taskId, existing)) {
        outError = "no task with id '" + taskId + "'";
        return {};
    }

    const bool setAssignee = arguments.contains("assignee_agent_id");
    const std::string assigneeAgentId =
        setAssignee ? arguments["assignee_agent_id"].get<std::string>() : std::string();
    if (!ctx.taskStore.UpdateStatus(
            taskId, status, setAssignee, assigneeAgentId, static_cast<int64_t>(time(nullptr)))) {
        outError = "failed to update task '" + taskId + "'";
        return {};
    }
    return json{{"task_id", taskId}, {"status", status}};
}

// list_tasks — every filter is optional; omitting all three searches every
// task regardless of workspace/chat.
json ListTasksTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    const std::string status = arguments.value("status", std::string());
    if (!status.empty() && !TaskStatus::IsValid(status)) {
        outError = "status must be one of " + TaskStatus::ValidList() + " (got '" + status + "')";
        return {};
    }
    const std::string assigneeAgentId = arguments.value("assignee_agent_id", std::string());
    const std::string workspaceId = arguments.value("workspace_id", std::string());

    json out = json::array();
    for (const Task& task : ctx.taskStore.List(status, assigneeAgentId, workspaceId)) {
        out.push_back(json{
            {"id", task.id},
            {"workspace_id", task.workspaceId},
            {"chat_id", task.chatId},
            {"title", task.title},
            {"description", task.description},
            {"status", task.status},
            {"assignee_agent_id", task.assigneeAgentId},
            {"created_by", task.createdBy},
        });
    }
    return json{{"tasks", out}};
}

// schedule_reminder — chat_id is REQUIRED (unlike post_message/read_chat's
// optional chat_id, there's no sensible "current chat" default for a
// reminder meant to fire well after this turn ends) and always goes through
// the same CallerIsParticipant gate post_message/read_chat apply to an
// explicit chat_id, per the security fix in commit 337160a: an agent must
// not be able to schedule a message into a chat it isn't a participant of.
// Exactly one of delay_seconds/fire_at must be given — accepting both would
// leave which one wins ambiguous, and accepting neither leaves fire_at
// undefined.
json ScheduleReminderTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("chat_id") || !arguments.contains("message")) {
        outError = "schedule_reminder requires 'chat_id' and 'message'";
        return {};
    }
    const std::string chatId = arguments["chat_id"].get<std::string>();
    if (!CallerIsParticipant(ctx, chatId)) {
        outError = "schedule_reminder: you are not a participant of that chat";
        return {};
    }

    const bool hasDelay = arguments.contains("delay_seconds");
    const bool hasFireAt = arguments.contains("fire_at");
    if (hasDelay == hasFireAt) {
        outError = "schedule_reminder requires exactly one of 'delay_seconds' or 'fire_at'";
        return {};
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t fireAt = 0;
    if (hasDelay) {
        if (!arguments["delay_seconds"].is_number_integer()) {
            outError = "schedule_reminder: 'delay_seconds' must be an integer";
            return {};
        }
        const int64_t delaySeconds = arguments["delay_seconds"].get<int64_t>();
        if (delaySeconds < 0) {
            outError = "schedule_reminder: 'delay_seconds' must not be negative";
            return {};
        }
        fireAt = now + delaySeconds;
    } else {
        if (!arguments["fire_at"].is_number_integer()) {
            outError = "schedule_reminder: 'fire_at' must be an integer unix timestamp";
            return {};
        }
        fireAt = arguments["fire_at"].get<int64_t>();
    }

    const std::string message = arguments["message"].get<std::string>();
    static std::atomic<int> reminderCounter{0};

    Reminder reminder;
    reminder.id = "reminder-" + ctx.agentId + "-" + std::to_string(now) + "-" + std::to_string(++reminderCounter);
    reminder.chatId = chatId;
    reminder.message = message;
    reminder.fireAt = fireAt;
    reminder.createdBy = ctx.agentId;
    reminder.status = ReminderStatus::kPending;
    reminder.createdAt = now;
    if (!ctx.reminderStore.Create(reminder)) {
        outError = "failed to save the reminder";
        return {};
    }
    return json{{"reminder_id", reminder.id}, {"fire_at", fireAt}, {"status", reminder.status}};
}

// list_reminders — always scoped to the caller's own reminders, no args.
json ListRemindersTool(ToolContext& ctx, const json& /*arguments*/, std::string& /*outError*/) {
    json out = json::array();
    for (const Reminder& reminder : ctx.reminderStore.ListPendingByCreator(ctx.agentId)) {
        out.push_back(json{
            {"id", reminder.id},
            {"chat_id", reminder.chatId},
            {"message", reminder.message},
            {"fire_at", reminder.fireAt},
            {"status", reminder.status},
        });
    }
    return json{{"reminders", out}};
}

// cancel_reminder — only the agent that created a reminder may cancel it.
json CancelReminderTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("reminder_id")) {
        outError = "cancel_reminder requires 'reminder_id'";
        return {};
    }
    const std::string reminderId = arguments["reminder_id"].get<std::string>();
    Reminder reminder;
    if (!ctx.reminderStore.Get(reminderId, reminder)) {
        outError = "no reminder with id '" + reminderId + "'";
        return {};
    }
    if (reminder.createdBy != ctx.agentId) {
        outError = "cancel_reminder: you can only cancel your own reminders";
        return {};
    }
    if (reminder.status != ReminderStatus::kPending) {
        outError = "reminder '" + reminderId + "' is not pending (currently '" + reminder.status + "')";
        return {};
    }
    if (!ctx.reminderStore.SetStatus(reminderId, ReminderStatus::kCancelled)) {
        outError = "failed to cancel reminder '" + reminderId + "'";
        return {};
    }
    return json{{"reminder_id", reminderId}, {"status", ReminderStatus::kCancelled}};
}

// Available to every agent (see IsAlwaysAllowedTool) — an agent that finds
// the existing toolset lacking (missing entirely, or too narrow for what
// it's trying to do) uses this instead of silently failing or improvising.
// Creates a brand-new chat/channel per request (never reused, unlike
// message_user's DM) so each request gets its own discussion thread — set
// to auto_respond (not the tag-only "listening" default new participants
// otherwise get) since the whole point is Cardon can just reply there and
// have the requesting agent engage without needing to @tag it first.
json RequestNewTool(ToolContext& ctx, const json& arguments, std::string& outError) {
    if (!arguments.contains("description")) {
        outError = "request_new_tool requires 'description'";
        return {};
    }
    const std::string description = arguments["description"].get<std::string>();
    const std::string toolName = arguments.value("tool_name", std::string());

    Agent caller;
    if (!ctx.agentStore.Get(ctx.agentId, caller)) {
        outError = "internal error: calling agent '" + ctx.agentId + "' not found";
        return {};
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    // A counter (not just the timestamp) guards against two calls landing
    // in the same second within one MCP subprocess — the timestamp alone
    // isn't fine-grained enough to keep the chat id unique call-to-call.
    static std::atomic<int> requestCounter{0};
    Chat chat;
    chat.id = "toolreq-" + ctx.agentId + "-" + std::to_string(now) + "-" + std::to_string(++requestCounter);
    chat.title = "Tool request: " + (toolName.empty() ? std::string("(unnamed)") : toolName);
    chat.createdBy = ctx.agentId;
    chat.status = "active";
    // discordChannelId left empty on purpose — same lazy-creation pattern as
    // message_user/create_workspace; Orchestrator creates the real channel
    // the first time this chat has something to mirror.
    chat.createdAt = now;
    if (!ctx.chatStore.CreateChat(chat)) {
        outError = "failed to create the tool-request chat";
        return {};
    }
    ctx.chatStore.AddParticipant(chat.id, "agent", ctx.agentId);
    ctx.chatStore.SetParticipantMode(chat.id, "agent", ctx.agentId, ParticipantMode::kAutoRespond);

    std::ostringstream summary;
    summary << "**Tool request from " << caller.name << "**";
    if (!toolName.empty()) {
        summary << " — `" << toolName << "`";
    }
    summary << "\n" << description;

    Message message;
    message.chatId = chat.id;
    message.senderType = "agent";
    message.senderId = ctx.agentId;
    message.type = "text";
    message.content = summary.str();
    message.createdAt = now;
    const int64_t messageId = ctx.chatStore.InsertMessage(message);
    if (messageId < 0) {
        outError = "failed to record the tool request message";
        return {};
    }
    return json{{"status", "requested"}, {"chat_id", chat.id}};
}

// Memory tools (currently just remember) and request_temporary_permission
// itself are available to every agent regardless of tool_permissions —
// gating memory behind a permission serves no real safety purpose (it's
// private per-agent state, see AgentStore::SetFact/GetFact, not something
// that touches Discord, other agents, or the filesystem), and
// request_temporary_permission has to be universally callable or an agent
// that lacks everything else would have no way to ask for anything either.
bool IsAlwaysAllowedTool(const std::string& toolName) {
    return toolName == "remember" || toolName == "request_temporary_permission" || toolName == "request_new_tool";
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
        {
            {"name", "add_agent_to_workspace"},
            {"description",
             "Add another agent to an existing workspace's conversation. The added agent defaults to "
             "listening mode — it gathers context but won't actually respond until it's explicitly "
             "@-tagged, or someone changes it to auto_respond via update_agent. Subject to the same "
             "can_message rule as request_add_agent_to_chat: you can only add an agent you're allowed "
             "to message."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"workspace_id", {{"type", "string"}}},
                      {"agent_id", {{"type", "string"}}},
                  }},
                 {"required", json::array({"workspace_id", "agent_id"})},
             }},
        },
        {
            {"name", "request_temporary_permission"},
            {"description",
             "Ask Cardon for one-time use of a tool you don't currently have permission to call — posts "
             "a request with checkmark/cross reactions. If approved, your very next call to that tool "
             "succeeds regardless of your normal tool_permissions, and the grant is then consumed — it "
             "does not persist, and does not change your permanent tool_permissions (ask Cardon or an "
             "agent holding update_agent for that instead). Available to every agent unconditionally, "
             "unlike every other tool here."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"tool_name", {{"type", "string"}, {"description", "The exact tool name you need."}}},
                      {"reason", {{"type", "string"}}},
                  }},
                 {"required", json::array({"tool_name", "reason"})},
             }},
        },
        {
            {"name", "create_pull_request"},
            {"description",
             "Push a workspace repo's worktree branch and open a pull request for it via the gh CLI. "
             "Commit your work in the worktree first (via your Bash tool access — repo-linked agents get "
             "that automatically) — this only pushes and opens the PR, it does not commit anything for "
             "you. base_branch defaults to 'main' if omitted."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"workspace_id", {{"type", "string"}}},
                      {"repo_id", {{"type", "string"}}},
                      {"title", {{"type", "string"}}},
                      {"body", {{"type", "string"}}},
                      {"base_branch", {{"type", "string"}}},
                  }},
                 {"required", json::array({"workspace_id", "repo_id", "title", "body"})},
             }},
        },
        {
            {"name", "list_pull_requests"},
            {"description",
             "List a repo's pull requests via the gh CLI (no local worktree needed -- targets "
             "owner/repo directly). status filters by state: 'open' (default), 'closed', 'merged', "
             "or 'all'."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"status", {{"type", "string"}, {"description", "'open' (default), 'closed', 'merged', or 'all'."}}},
                  }},
                 {"required", json::array({"repo_id"})},
             }},
        },
        {
            {"name", "get_pr_status"},
            {"description",
             "Get one pull request's current state, title, url, mergeable status, and review decision "
             "via the gh CLI."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"pr_number", {{"type", "integer"}}},
                  }},
                 {"required", json::array({"repo_id", "pr_number"})},
             }},
        },
        {
            {"name", "set_pr_status"},
            {"description",
             "Reopen, close, approve, or request changes on a pull request via the gh CLI. status must "
             "be one of 'open', 'closed', 'approved', 'changes-requested' -- this tool deliberately does "
             "not accept 'merged'; use merge_pull_request to actually merge a PR. comment is optional "
             "free-form text used as the review body for 'approved'/'changes-requested' (a placeholder "
             "is substituted for 'changes-requested' if you omit it, since gh requires a non-empty body "
             "for that review type)."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"pr_number", {{"type", "integer"}}},
                      {"status",
                       {{"type", "string"},
                        {"description", "'open', 'closed', 'approved', or 'changes-requested'. Not 'merged'."}}},
                      {"comment", {{"type", "string"}}},
                  }},
                 {"required", json::array({"repo_id", "pr_number", "status"})},
             }},
        },
        {
            {"name", "merge_pull_request"},
            {"description",
             "Merge a pull request via the gh CLI. merge_method is 'merge' (default), 'squash', or "
             "'rebase'."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"pr_number", {{"type", "integer"}}},
                      {"merge_method", {{"type", "string"}, {"description", "'merge' (default), 'squash', or 'rebase'."}}},
                  }},
                 {"required", json::array({"repo_id", "pr_number"})},
             }},
        },
        {
            {"name", "list_issues"},
            {"description",
             "List a repo's issues via the gh CLI. status filters by state: 'open' (default), "
             "'closed', or 'all'."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"status", {{"type", "string"}, {"description", "'open' (default), 'closed', or 'all'."}}},
                  }},
                 {"required", json::array({"repo_id"})},
             }},
        },
        {
            {"name", "create_issue"},
            {"description", "Open a new issue on a repo via the gh CLI. body is optional."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"title", {{"type", "string"}}},
                      {"body", {{"type", "string"}}},
                  }},
                 {"required", json::array({"repo_id", "title"})},
             }},
        },
        {
            {"name", "set_issue_status"},
            {"description", "Reopen or close an issue via the gh CLI. status must be 'open' or 'closed'."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"repo_id", {{"type", "string"}}},
                      {"issue_number", {{"type", "integer"}}},
                      {"status", {{"type", "string"}, {"description", "'open' or 'closed'."}}},
                  }},
                 {"required", json::array({"repo_id", "issue_number", "status"})},
             }},
        },
        {
            {"name", "list_repos"},
            {"description",
             "List every imported repo (id, github_url, status, agent_id, notes). Read-only."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", json::object()},
             }},
        },
        {
            {"name", "import_repo"},
            {"description",
             "Import an already-existing GitHub repo: clones it and records it as a known repo. Unlike "
             "/add-repo, this does NOT kick off the repo-onboarding conversation with Alex or propose a "
             "dedicated agent for it -- the repo is left at status 'ready' for a human, or an agent asked "
             "normally, to onboard afterward. github_url accepts https://github.com/org/repo, "
             "git@github.com:org/repo.git, or the bare 'org/repo' form. Idempotent -- importing an "
             "already-known repo again is a no-op."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"github_url", {{"type", "string"}}},
                      {"notes", {{"type", "string"}}},
                  }},
                 {"required", json::array({"github_url"})},
             }},
        },
        {
            {"name", "create_repo"},
            {"description",
             "Create a brand-new, empty, private GitHub repo via the gh CLI (gh repo create --private), "
             "then import it exactly like import_repo (no onboarding conversation, status left 'ready'). "
             "name may be 'org/reponame' or just 'reponame' (created under the authenticated gh account)."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"name", {{"type", "string"}}},
                      {"notes", {{"type", "string"}}},
                  }},
                 {"required", json::array({"name"})},
             }},
        },
        {
            {"name", "create_task"},
            {"description",
             "Create a task. Status starts at 'not_started'. If workspace_id is omitted, the task's chat "
             "defaults to the current chat; if given, the task is scoped to that workspace instead."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"title", {{"type", "string"}}},
                      {"workspace_id", {{"type", "string"}}},
                      {"assignee_agent_id", {{"type", "string"}}},
                      {"description", {{"type", "string"}}},
                  }},
                 {"required", json::array({"title"})},
             }},
        },
        {
            {"name", "update_task_status"},
            {"description",
             "Update a task's status, and optionally reassign it in the same call. status must be one of "
             "'not_started', 'in_progress', 'blocked', 'in_review', 'done', 'cancelled'."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"task_id", {{"type", "string"}}},
                      {"status",
                       {{"type", "string"},
                        {"description",
                         "'not_started', 'in_progress', 'blocked', 'in_review', 'done', or 'cancelled'."}}},
                      {"assignee_agent_id", {{"type", "string"}}},
                  }},
                 {"required", json::array({"task_id", "status"})},
             }},
        },
        {
            {"name", "list_tasks"},
            {"description",
             "List tasks, optionally filtered by status, assignee_agent_id, and/or workspace_id. Omitting "
             "all three searches every task."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"status", {{"type", "string"}}},
                      {"assignee_agent_id", {{"type", "string"}}},
                      {"workspace_id", {{"type", "string"}}},
                  }},
             }},
        },
        {
            {"name", "schedule_reminder"},
            {"description",
             "Schedule a message to be posted into a chat at a future time. Requires chat_id (you must "
             "already be a participant of it) and message, plus exactly one of delay_seconds (fire this "
             "many seconds from now) or fire_at (an absolute unix timestamp)."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"chat_id", {{"type", "string"}}},
                      {"message", {{"type", "string"}}},
                      {"delay_seconds", {{"type", "integer"}}},
                      {"fire_at", {{"type", "integer"}}},
                  }},
                 {"required", json::array({"chat_id", "message"})},
             }},
        },
        {
            {"name", "list_reminders"},
            {"description", "List your own still-pending scheduled reminders."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", json::object()},
             }},
        },
        {
            {"name", "cancel_reminder"},
            {"description",
             "Cancel one of your own pending reminders before it fires. Rejected if the reminder belongs "
             "to another agent or has already fired/been cancelled."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties", {{"reminder_id", {{"type", "string"}}}}},
                 {"required", json::array({"reminder_id"})},
             }},
        },
        {
            {"name", "request_new_tool"},
            {"description",
             "Ask Cardon for a brand-new tool, or for an existing tool's functionality to be extended, "
             "when nothing currently available covers what you're trying to do. Creates a fresh channel "
             "just for this request so Cardon can discuss it with you, ask clarifying questions, or come "
             "back to it later — you're free to keep talking there. Available to every agent "
             "unconditionally, unlike every other tool here."},
            {"inputSchema",
             {
                 {"type", "object"},
                 {"properties",
                  {
                      {"tool_name",
                       {{"type", "string"},
                        {"description", "A short proposed name for the tool, if you have one in mind (optional)."}}},
                      {"description",
                       {{"type", "string"},
                        {"description",
                         "What you're trying to do and why nothing existing covers it — as much detail "
                         "as would help Cardon decide whether/how to build it."}}},
                  }},
                 {"required", json::array({"description"})},
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
    // Set when `permitted` came from a one-time grant (request_temporary_
    // permission) rather than agent.tool_permissions — burned via
    // ctx.tempPermissionStore.Consume below once the call actually succeeds,
    // never on a validation failure inside the tool itself (bad args
    // shouldn't cost the agent its one shot).
    bool usedTempGrant = false;
    if (!ctx.agentStore.Get(ctx.agentId, agent)) {
        outError = "unknown agent: " + ctx.agentId;
    } else if (IsAlwaysAllowedTool(toolName)) {
        permitted = true;
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
            permitted = ctx.tempPermissionStore.HasActiveGrant(ctx.agentId, toolName);
            usedTempGrant = permitted;
        }
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
    } else if (toolName == "add_agent_to_workspace") {
        result = AddAgentToWorkspace(ctx, arguments, outError);
    } else if (toolName == "request_temporary_permission") {
        result = RequestTemporaryPermission(ctx, arguments, outError);
    } else if (toolName == "create_pull_request") {
        result = CreatePullRequestTool(ctx, arguments, outError);
    } else if (toolName == "list_pull_requests") {
        result = ListPullRequestsTool(ctx, arguments, outError);
    } else if (toolName == "get_pr_status") {
        result = GetPrStatusTool(ctx, arguments, outError);
    } else if (toolName == "set_pr_status") {
        result = SetPrStatusTool(ctx, arguments, outError);
    } else if (toolName == "merge_pull_request") {
        result = MergePullRequestTool(ctx, arguments, outError);
    } else if (toolName == "list_issues") {
        result = ListIssuesTool(ctx, arguments, outError);
    } else if (toolName == "create_issue") {
        result = CreateIssueTool(ctx, arguments, outError);
    } else if (toolName == "set_issue_status") {
        result = SetIssueStatusTool(ctx, arguments, outError);
    } else if (toolName == "request_new_tool") {
        result = RequestNewTool(ctx, arguments, outError);
    } else if (toolName == "list_repos") {
        result = ListReposTool(ctx, arguments, outError);
    } else if (toolName == "import_repo") {
        result = ImportRepoTool(ctx, arguments, outError);
    } else if (toolName == "create_repo") {
        result = CreateRepoTool(ctx, arguments, outError);
    } else if (toolName == "create_task") {
        result = CreateTaskTool(ctx, arguments, outError);
    } else if (toolName == "update_task_status") {
        result = UpdateTaskStatusTool(ctx, arguments, outError);
    } else if (toolName == "list_tasks") {
        result = ListTasksTool(ctx, arguments, outError);
    } else if (toolName == "schedule_reminder") {
        result = ScheduleReminderTool(ctx, arguments, outError);
    } else if (toolName == "list_reminders") {
        result = ListRemindersTool(ctx, arguments, outError);
    } else if (toolName == "cancel_reminder") {
        result = CancelReminderTool(ctx, arguments, outError);
    } else {
        outError = "unknown tool: " + toolName;
    }

    if (!outError.empty()) {
        ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_error", json{{"tool", toolName}, {"error", outError}});
    } else {
        ctx.activityLog.Log(ctx.chatId, ctx.agentId, "tool_result", json{{"tool", toolName}, {"result", result}});
        if (usedTempGrant) {
            ctx.tempPermissionStore.Consume(ctx.agentId, toolName, static_cast<int64_t>(time(nullptr)));
        }
    }
    return result;
}

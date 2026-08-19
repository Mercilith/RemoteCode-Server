#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../db/AgentStore.h"
#include "../db/ChatStore.h"

namespace dpp {
class cluster;
class channel;
struct message_create_t;
struct message_reaction_add_t;
struct slashcommand_t;
} // namespace dpp

// Called after an incoming Discord message has been written to the
// messages table, so Orchestrator can decide whether to spawn agent turns
// for the chat. Runs on DPP's own event thread.
using IncomingMessageHandler = std::function<void(const std::string& chatId)>;
using DiscordLogHandler = std::function<void(const std::string& message)>;
// Called when a human (never the bot itself) reacts to a message —
// `emoji` is the reaction's unicode/name (e.g. the check or cross used by
// the approval workflow). Runs on DPP's own event thread.
using DiscordReactionHandler =
    std::function<void(const std::string& discordMessageId, const std::string& emoji)>;
// Called when a user invokes the `/add-repo` slash command — `url` is the
// required "url" option, `notes` the optional "notes" option (empty if not
// given). Runs on DPP's own event thread, same as the other handlers; the
// handler itself is expected to ack the interaction immediately (Discord's
// 3-second window) and hand any real work off to a detached thread — see
// Orchestrator::Run's wiring.
using SlashCommandAddRepoHandler =
    std::function<void(const std::string& url, const std::string& notes)>;

// create-chat/add-agent/remove-agent are pure bookkeeping (DB writes plus at
// most one Discord REST call — channel create or a permission-overwrite
// edit), never an agent turn, so unlike add-repo's "ack now, do the slow
// part later" pattern, they run synchronously and reply with the real result
// (matching the existing synchronous-REST-call precedent already used by
// CreateDmChannel/EnsureWebhook/AddReaction below).
//
// `agentsRaw`/`agentRaw` are the raw space-or-comma-separated agent name/id
// list from the slash command's string option — dpp has no good multi-select
// option type, so resolution against AgentStore::ListAll() happens on the
// handler side (Orchestrator), not here.
using SlashCommandCreateChatHandler =
    std::function<std::string(const std::string& agentsRaw, const std::string& title)>;
using SlashCommandAddAgentHandler =
    std::function<std::string(const std::string& channelId, const std::string& agentRaw)>;
using SlashCommandRemoveAgentHandler =
    std::function<std::string(const std::string& channelId, const std::string& agentRaw)>;
// void, not std::string — see the handlers' own comments in Orchestrator.cpp:
// both of these may need to delete a Discord channel (close-chat always;
// create-dm when retiring a previous DM before starting a fresh one), and
// that delete has to happen strictly after this interaction's ack, not
// before a synchronous reply is computed — otherwise a slow/rate-limited
// delete call risks missing Discord's 3-second ack window ("The application
// didn't respond"). `invokingChannelId` lets the handler report failures
// (unknown agent, channel-create failure) back into wherever the command was
// actually run from, since there's no synchronous return value to carry that
// text anymore.
using SlashCommandCreateDmHandler =
    std::function<void(const std::string& agentRaw, const std::string& invokingChannelId)>;
using SlashCommandCloseChatHandler = std::function<void(const std::string& channelId)>;

// /create-workspace — `reposRaw` is the raw space/comma-separated repo
// name/id list (same free-text-then-resolve-on-the-handler-side pattern as
// agentsRaw above; there's no dpp multi-select type here either), `title`
// the optional title option. Runs synchronously and replies with the real
// result, same class of work as create-chat/create-dm (plain bookkeeping
// plus a handful of Discord REST calls — category + channel create, no
// agent turn) rather than add-repo's ack-now/work-later pattern.
using SlashCommandCreateWorkspaceHandler =
    std::function<std::string(const std::string& reposRaw, const std::string& title)>;

// Thin DPP wrapper: gateway connect, message handler (writes into
// ChatStore), and a webhook-based post helper so each agent can appear
// under its own name/avatar in a channel.
class DiscordBot {
public:
    // `agentStore` is used to auto-join every *active* agent into any
    // channel-backed chat it sees a message in (see HandleMessageCreate) —
    // there's no explicit invite/addressing mechanism yet, so "active agent"
    // is the whole membership rule for now.
    DiscordBot(std::string token, ChatStore& chatStore, AgentStore& agentStore);
    ~DiscordBot();

    DiscordBot(const DiscordBot&) = delete;
    DiscordBot& operator=(const DiscordBot&) = delete;

    void SetIncomingMessageHandler(IncomingMessageHandler handler);
    // Surfaces DPP's own gateway/REST diagnostics (connect attempts,
    // errors, warnings) — without this, gateway connection failures fail
    // completely silently.
    void SetLogHandler(DiscordLogHandler handler);
    void SetReactionHandler(DiscordReactionHandler handler);
    // Registers /add-repo as a global slash command on connect (on_ready)
    // and dispatches invocations here. No-op (never registers/dispatches)
    // if never called — matches the pattern of the other optional handlers.
    void SetSlashCommandAddRepoHandler(SlashCommandAddRepoHandler handler);
    // Registers /create-chat, /create-dm, /add-agent, /remove-agent, and
    // /close-chat as global slash commands on connect and dispatches
    // invocations to the handlers below. Each is independently optional
    // (never registered/dispatched) — same no-op-if-unset pattern as
    // SetSlashCommandAddRepoHandler.
    void SetSlashCommandCreateChatHandler(SlashCommandCreateChatHandler handler);
    void SetSlashCommandCreateDmHandler(SlashCommandCreateDmHandler handler);
    void SetSlashCommandAddAgentHandler(SlashCommandAddAgentHandler handler);
    void SetSlashCommandRemoveAgentHandler(SlashCommandRemoveAgentHandler handler);
    void SetSlashCommandCloseChatHandler(SlashCommandCloseChatHandler handler);
    void SetSlashCommandCreateWorkspaceHandler(SlashCommandCreateWorkspaceHandler handler);

    // Connects to the Gateway and blocks until Stop() is called. Intended
    // to be run on its own thread by Orchestrator.
    void Run();
    void Stop();

    // Posts `content` into `channelId` under `agentName`'s identity via a
    // per-(channel, agent) webhook, creating and caching the webhook on
    // first use. Returns the posted Discord message id, or an empty string
    // on any REST failure.
    std::string PostAsAgent(
        const std::string& channelId, const std::string& agentId, const std::string& agentName,
        const std::string& content);

    // Posts `content` as the bot's own identity (not a webhook) — used for
    // system-level messages (e.g. approval outcomes) that no single agent
    // "said."
    void PostPlain(const std::string& channelId, const std::string& content);

    // Adds `emoji` (a literal unicode reaction, e.g. a check/cross) to a
    // message. Used to seed the approve/reject options on approval-request
    // messages.
    bool AddReaction(const std::string& channelId, const std::string& messageId, const std::string& emoji);

    // This bot's own Discord user id (populated from dpp::cluster::me once
    // connected) — empty before the gateway is ready. Needed so a created
    // DM channel can grant the shared bot itself view/send access alongside
    // whichever human/agent-bot it's actually for.
    std::string BotUserId() const;

    // Creates a private text channel in `guildId` named `channelName`,
    // denies @everyone view, and grants view/send/read-history to
    // `humanUserId`, this bot's own account, and every id in
    // `extraBotUserIds` (each participating agent's own bot, when it has
    // one — it may end up posting there instead of this shared bot; empty
    // ids are skipped). Returns the new channel's id, or an empty string on
    // failure.
    // `parentCategoryId`, if non-empty, sets the created channel's parent
    // category (dpp::channel::set_parent_id) — used by workspace chat
    // channels, which live under a dedicated category (see CreateCategory
    // below). Empty (the default) preserves the original parent-less
    // behavior every other caller (DM/group chats) still wants.
    std::string CreateDmChannel(
        const std::string& guildId, const std::string& channelName, const std::string& humanUserId,
        const std::vector<std::string>& extraBotUserIds, const std::string& parentCategoryId = "");

    // Creates a private category (channel group) in `guildId` named
    // `categoryName`, with the exact same deny-@everyone/allow-listed-
    // accounts permission pattern CreateDmChannel uses — child channels
    // parented under it (via CreateDmChannel's parentCategoryId) inherit
    // those overwrites unless a channel-level overwrite says otherwise, so
    // this is what actually makes a workspace's whole category private.
    // Returns the new category's id, or an empty string on failure. Used by
    // Orchestrator::EnsureWorkspaceDiscordAssets to back a new workspace.
    std::string CreateCategory(
        const std::string& guildId, const std::string& categoryName, const std::string& humanUserId,
        const std::vector<std::string>& extraBotUserIds);

    // Grants view/send/read-history to `userId` on an already-existing
    // channel — used when an agent with its own Discord bot joins a chat
    // whose Discord channel was created before that agent was a participant
    // (e.g. via /add-agent, or an approved request_add_agent_to_chat), so it
    // isn't stuck unable to see a channel it just joined. A no-op for
    // shared-webhook agents (no separate Discord account to grant) — callers
    // only invoke this when the joining agent has its own bot user id.
    bool GrantChannelAccess(const std::string& channelId, const std::string& userId);

    // Deletes a channel outright — used by /close-chat. Best-effort: returns
    // false (never throws/crashes) on any failure, e.g. the bot lacking
    // Manage Channels in that particular channel, or the channel already
    // being gone. Every channel this server creates (CreateDmChannel above)
    // is created by this same shared bot, so this should always succeed for
    // a chat's own channel in practice; the false-return path exists for
    // channels created some other way.
    bool DeleteChannel(const std::string& channelId);

    // True if shard 0 hasn't received a heartbeat ACK from Discord in an
    // unreasonably long time — the local socket can look connected (and
    // keep sending outbound heartbeats on its own timer) while the remote
    // end has silently stopped responding, e.g. after a bad resume. This is
    // the actual "is anything getting through" signal; Orchestrator polls
    // it and force-reconnects when it goes true.
    bool IsZombied() const;

private:
    void HandleMessageCreate(const dpp::message_create_t& event);
    void HandleReactionAdd(const dpp::message_reaction_add_t& event);
    void HandleSlashCommand(const dpp::slashcommand_t& event);
    bool EnsureWebhook(
        const std::string& channelId, const std::string& agentId, const std::string& agentName,
        std::string& outWebhookId, std::string& outWebhookToken);
    // Deny @everyone view, then allow humanUserId, this bot's own account,
    // and every id in extraBotUserIds — the "private channel/category"
    // permission pattern shared by CreateDmChannel and CreateCategory,
    // factored out here so it lives in exactly one place.
    void ApplyPrivateChannelOverwrites(
        dpp::channel& c, const std::string& guildId, const std::string& humanUserId,
        const std::vector<std::string>& extraBotUserIds);

    std::string token_;
    ChatStore& chatStore_;
    AgentStore& agentStore_;
    IncomingMessageHandler onIncomingMessage_;
    DiscordLogHandler onLog_;
    DiscordReactionHandler onReaction_;
    SlashCommandAddRepoHandler onSlashCommandAddRepo_;
    SlashCommandCreateChatHandler onSlashCommandCreateChat_;
    SlashCommandCreateDmHandler onSlashCommandCreateDm_;
    SlashCommandAddAgentHandler onSlashCommandAddAgent_;
    SlashCommandRemoveAgentHandler onSlashCommandRemoveAgent_;
    SlashCommandCloseChatHandler onSlashCommandCloseChat_;
    SlashCommandCreateWorkspaceHandler onSlashCommandCreateWorkspace_;
    // Guards bot_ against a race between Run() reassigning it (fresh
    // cluster per (re)connect) and IsZombied() reading it from the
    // watchdog's own timer thread. Other members' cross-thread use of bot_
    // (Stop(), PostAsAgent, ...) never overlaps a reassignment by
    // construction of the surrounding control flow, so isn't covered here.
    mutable std::mutex botMutex_;
    std::unique_ptr<dpp::cluster> bot_;
};

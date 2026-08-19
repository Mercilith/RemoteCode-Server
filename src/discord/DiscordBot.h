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
    std::string CreateDmChannel(
        const std::string& guildId, const std::string& channelName, const std::string& humanUserId,
        const std::vector<std::string>& extraBotUserIds);

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

    std::string token_;
    ChatStore& chatStore_;
    AgentStore& agentStore_;
    IncomingMessageHandler onIncomingMessage_;
    DiscordLogHandler onLog_;
    DiscordReactionHandler onReaction_;
    SlashCommandAddRepoHandler onSlashCommandAddRepo_;
    // Guards bot_ against a race between Run() reassigning it (fresh
    // cluster per (re)connect) and IsZombied() reading it from the
    // watchdog's own timer thread. Other members' cross-thread use of bot_
    // (Stop(), PostAsAgent, ...) never overlaps a reassignment by
    // construction of the surrounding control flow, so isn't covered here.
    mutable std::mutex botMutex_;
    std::unique_ptr<dpp::cluster> bot_;
};

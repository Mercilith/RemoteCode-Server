#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "../db/AgentStore.h"
#include "../db/ChatStore.h"

namespace dpp {
class cluster;
struct message_create_t;
struct message_reaction_add_t;
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
    bool EnsureWebhook(
        const std::string& channelId, const std::string& agentId, const std::string& agentName,
        std::string& outWebhookId, std::string& outWebhookToken);

    std::string token_;
    ChatStore& chatStore_;
    AgentStore& agentStore_;
    IncomingMessageHandler onIncomingMessage_;
    DiscordLogHandler onLog_;
    DiscordReactionHandler onReaction_;
    // Guards bot_ against a race between Run() reassigning it (fresh
    // cluster per (re)connect) and IsZombied() reading it from the
    // watchdog's own timer thread. Other members' cross-thread use of bot_
    // (Stop(), PostAsAgent, ...) never overlaps a reassignment by
    // construction of the surrounding control flow, so isn't covered here.
    mutable std::mutex botMutex_;
    std::unique_ptr<dpp::cluster> bot_;
};

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../db/ChatStore.h"

namespace dpp {
class cluster;
struct message_create_t;
} // namespace dpp

// Called after an incoming Discord message has been written to the
// messages table, so Orchestrator can decide whether to spawn agent turns
// for the chat. Runs on DPP's own event thread.
using IncomingMessageHandler = std::function<void(const std::string& chatId)>;
using DiscordLogHandler = std::function<void(const std::string& message)>;

// Thin DPP wrapper: gateway connect, message handler (writes into
// ChatStore), and a webhook-based post helper so each agent can appear
// under its own name/avatar in a channel.
class DiscordBot {
public:
    DiscordBot(std::string token, ChatStore& chatStore);
    ~DiscordBot();

    DiscordBot(const DiscordBot&) = delete;
    DiscordBot& operator=(const DiscordBot&) = delete;

    void SetIncomingMessageHandler(IncomingMessageHandler handler);
    // Surfaces DPP's own gateway/REST diagnostics (connect attempts,
    // errors, warnings) — without this, gateway connection failures fail
    // completely silently.
    void SetLogHandler(DiscordLogHandler handler);

    // Connects to the Gateway and blocks until Stop() is called. Intended
    // to be run on its own thread by Orchestrator.
    void Run();
    void Stop();

    // Posts `content` into `channelId` under `agentName`'s identity via a
    // per-(channel, agent) webhook, creating and caching the webhook on
    // first use. Returns false on any REST failure.
    bool PostAsAgent(
        const std::string& channelId, const std::string& agentId, const std::string& agentName,
        const std::string& content);

private:
    void HandleMessageCreate(const dpp::message_create_t& event);
    bool EnsureWebhook(
        const std::string& channelId, const std::string& agentId, const std::string& agentName,
        std::string& outWebhookId, std::string& outWebhookToken);

    std::string token_;
    ChatStore& chatStore_;
    IncomingMessageHandler onIncomingMessage_;
    DiscordLogHandler onLog_;
    std::unique_ptr<dpp::cluster> bot_;
};

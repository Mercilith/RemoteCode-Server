#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DiscordBot.h" // DiscordAttachment

namespace dpp {
class cluster;
} // namespace dpp

// A REST-only Discord client for a single agent's own bot token — used to
// post that agent's replies "as itself" (its own bot identity/avatar)
// instead of via the shared bot's webhook. Constructed with dpp::cluster's
// shards=0 ("NO_SHARDS") option: no Gateway/websocket connection, only
// HTTPS API calls. Despite being REST-only, DPP's REST queue is only
// actually pumped by cluster::start()'s event loop (not by construction
// alone), so this still owns a background thread running start() —
// confirmed by reading DPP's own cluster.cpp before relying on it.
class AgentBotClient {
public:
    explicit AgentBotClient(std::string token);
    ~AgentBotClient();

    AgentBotClient(const AgentBotClient&) = delete;
    AgentBotClient& operator=(const AgentBotClient&) = delete;

    // Validates the token and resolves this bot's own identity. Called
    // once when a token is first assigned via the admin API — also serves
    // as validation, since it fails if the token is bad.
    bool FetchSelf(std::string& outUserId, std::string& outUsername);

    // Posts `content` into `channelId` as this bot. Returns the posted
    // message id, or an empty string on any REST failure. `attachments`
    // (if any) are uploaded on the first chunk only — see
    // DiscordBot::PostAsAgent's identical comment.
    std::string PostAsSelf(
        const std::string& channelId, const std::string& content,
        const std::vector<DiscordAttachment>& attachments = {});

    // Fires the "X is typing..." indicator in `channelId` as this bot.
    // Fire-and-forget (dpp's default callback just logs on error) — Discord
    // only shows the indicator for ~10 seconds per call, so a caller wanting
    // it to stay visible across a longer operation needs to call this
    // repeatedly (see Orchestrator's typing-indicator loop).
    void TriggerTyping(const std::string& channelId);

private:
    std::unique_ptr<dpp::cluster> bot_;
    std::thread runThread_;
};

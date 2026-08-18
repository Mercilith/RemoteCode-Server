#pragma once

#include <memory>
#include <string>
#include <thread>

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
    // message id, or an empty string on any REST failure.
    std::string PostAsSelf(const std::string& channelId, const std::string& content);

private:
    std::unique_ptr<dpp::cluster> bot_;
    std::thread runThread_;
};

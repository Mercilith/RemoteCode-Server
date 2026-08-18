#include "AgentBotClient.h"

#include <dpp/dpp.h>

#include <future>

AgentBotClient::AgentBotClient(std::string token) {
    // shards=0 (NO_SHARDS): REST-only, never opens a websocket to the
    // Gateway. start() still has to run — that's what pumps the REST
    // request queue — just with nothing to reconnect/heartbeat.
    bot_ = std::make_unique<dpp::cluster>(std::move(token), dpp::i_default_intents, 0);
    runThread_ = std::thread([this]() { bot_->start(dpp::st_wait); });
}

AgentBotClient::~AgentBotClient() {
    bot_->shutdown();
    if (runThread_.joinable()) {
        runThread_.join();
    }
}

bool AgentBotClient::FetchSelf(std::string& outUserId, std::string& outUsername) {
    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->current_user_get(
        [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    if (result.is_error()) {
        return false;
    }
    const dpp::user_identified self = std::get<dpp::user_identified>(result.value);
    outUserId = std::to_string(self.id);
    outUsername = self.username;
    return true;
}

std::string AgentBotClient::PostAsSelf(const std::string& channelId, const std::string& content) {
    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->message_create(
        dpp::message(std::stoull(channelId), content),
        [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    if (result.is_error()) {
        return "";
    }
    const dpp::message posted = std::get<dpp::message>(result.value);
    return std::to_string(posted.id);
}

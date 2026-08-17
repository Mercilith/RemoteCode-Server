#include "DiscordBot.h"

#include <dpp/dpp.h>

#include <ctime>
#include <future>

DiscordBot::DiscordBot(std::string token, ChatStore& chatStore)
    : token_(std::move(token)), chatStore_(chatStore) {}

DiscordBot::~DiscordBot() = default;

void DiscordBot::SetIncomingMessageHandler(IncomingMessageHandler handler) {
    onIncomingMessage_ = std::move(handler);
}

void DiscordBot::SetLogHandler(DiscordLogHandler handler) {
    onLog_ = std::move(handler);
}

void DiscordBot::Run() {
    bot_ = std::make_unique<dpp::cluster>(token_, dpp::i_default_intents | dpp::i_message_content);

    if (onLog_) {
        bot_->on_log([this](const dpp::log_t& event) { onLog_(event.message); });
    }

    bot_->on_ready([this](const dpp::ready_t&) {
        if (onLog_) {
            onLog_("Discord gateway ready.");
        }
    });

    bot_->on_message_create([this](const dpp::message_create_t& event) { HandleMessageCreate(event); });

    bot_->start(dpp::st_wait);
}

void DiscordBot::Stop() {
    if (bot_) {
        bot_->shutdown();
    }
}

void DiscordBot::HandleMessageCreate(const dpp::message_create_t& event) {
    if (event.msg.author.is_bot()) {
        // Ignore our own webhook posts (and any other bots) to avoid loops.
        return;
    }

    const std::string channelId = std::to_string(event.msg.channel_id);

    Chat chat;
    if (!chatStore_.GetChatByDiscordChannel(channelId, chat)) {
        chat.id = channelId; // 1:1 channel<->chat mapping for this pass
        chat.title = "";
        chat.createdBy = "user";
        chat.status = "active";
        chat.discordChannelId = channelId;
        chat.createdAt = static_cast<int64_t>(time(nullptr));
        if (!chatStore_.CreateChat(chat)) {
            return;
        }
        // Alex is the only agent that exists this pass — auto-join every
        // channel-backed chat so "message in a channel Alex is in" holds
        // trivially, without needing real invite/addressing logic yet.
        chatStore_.AddParticipant(chat.id, "agent", "alex");
    }

    const std::string userId = std::to_string(event.msg.author.id);
    chatStore_.AddParticipant(chat.id, "user", userId);

    Message message;
    message.chatId = chat.id;
    message.senderType = "user";
    message.senderId = userId;
    message.type = "text";
    message.content = event.msg.content;
    message.discordMessageId = std::to_string(event.msg.id);
    message.createdAt = static_cast<int64_t>(time(nullptr));
    chatStore_.InsertMessage(message);

    if (onIncomingMessage_) {
        onIncomingMessage_(chat.id);
    }
}

bool DiscordBot::EnsureWebhook(
    const std::string& channelId, const std::string& agentId, const std::string& agentName,
    std::string& outWebhookId, std::string& outWebhookToken) {
    if (chatStore_.GetWebhook(channelId, agentId, outWebhookId, outWebhookToken)) {
        return true;
    }
    if (!bot_) {
        return false;
    }

    dpp::webhook newHook;
    newHook.name = agentName;
    newHook.channel_id = std::stoull(channelId);

    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->create_webhook(
        newHook, [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    if (result.is_error()) {
        return false;
    }

    const dpp::webhook created = std::get<dpp::webhook>(result.value);
    outWebhookId = std::to_string(created.id);
    outWebhookToken = created.token;
    chatStore_.SetWebhook(channelId, agentId, outWebhookId, outWebhookToken);
    return true;
}

bool DiscordBot::PostAsAgent(
    const std::string& channelId, const std::string& agentId, const std::string& agentName,
    const std::string& content) {
    std::string webhookId, webhookToken;
    if (!EnsureWebhook(channelId, agentId, agentName, webhookId, webhookToken) || !bot_) {
        return false;
    }

    dpp::webhook hook;
    hook.id = std::stoull(webhookId);
    hook.token = webhookToken;

    dpp::message msg;
    msg.content = content;

    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->execute_webhook(
        hook, msg, false, 0, "",
        [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    return !result.is_error();
}

#include "DiscordBot.h"

#include <dpp/dpp.h>

#include <ctime>
#include <future>

#include "../util/Text.h"

DiscordBot::DiscordBot(std::string token, ChatStore& chatStore, AgentStore& agentStore)
    : token_(std::move(token)), chatStore_(chatStore), agentStore_(agentStore) {}

DiscordBot::~DiscordBot() = default;

void DiscordBot::SetIncomingMessageHandler(IncomingMessageHandler handler) {
    onIncomingMessage_ = std::move(handler);
}

void DiscordBot::SetLogHandler(DiscordLogHandler handler) {
    onLog_ = std::move(handler);
}

void DiscordBot::SetReactionHandler(DiscordReactionHandler handler) {
    onReaction_ = std::move(handler);
}

void DiscordBot::SetSlashCommandAddRepoHandler(SlashCommandAddRepoHandler handler) {
    onSlashCommandAddRepo_ = std::move(handler);
}

void DiscordBot::SetSlashCommandCreateChatHandler(SlashCommandCreateChatHandler handler) {
    onSlashCommandCreateChat_ = std::move(handler);
}

void DiscordBot::SetSlashCommandCreateDmHandler(SlashCommandCreateDmHandler handler) {
    onSlashCommandCreateDm_ = std::move(handler);
}

void DiscordBot::SetSlashCommandAddAgentHandler(SlashCommandAddAgentHandler handler) {
    onSlashCommandAddAgent_ = std::move(handler);
}

void DiscordBot::SetSlashCommandRemoveAgentHandler(SlashCommandRemoveAgentHandler handler) {
    onSlashCommandRemoveAgent_ = std::move(handler);
}

void DiscordBot::SetSlashCommandCloseChatHandler(SlashCommandCloseChatHandler handler) {
    onSlashCommandCloseChat_ = std::move(handler);
}

void DiscordBot::SetSlashCommandCreateWorkspaceHandler(SlashCommandCreateWorkspaceHandler handler) {
    onSlashCommandCreateWorkspace_ = std::move(handler);
}

void DiscordBot::Run() {
    everReadyThisRun_ = false;
    runStartedAt_ = time(nullptr);

    {
        std::lock_guard<std::mutex> lock(botMutex_);
        bot_ = std::make_unique<dpp::cluster>(token_, dpp::i_default_intents | dpp::i_message_content);
    }

    if (onLog_) {
        bot_->on_log([this](const dpp::log_t& event) { onLog_(event.message); });
    }

    bot_->on_ready([this](const dpp::ready_t&) {
        everReadyThisRun_ = true;
        if (onLog_) {
            onLog_("Discord gateway ready.");
        }
        if (onSlashCommandAddRepo_) {
            // Global command registration — no guild id needed, and it's a
            // no-op (harmless overwrite) on every reconnect, so simplest to
            // just always do it here rather than tracking "did we already
            // register this session."
            dpp::slashcommand addRepoCommand(
                "add-repo", "Clone a GitHub repo and set up a dedicated expert agent for it.", bot_->me.id);
            addRepoCommand.add_option(
                dpp::command_option(dpp::co_string, "url", "GitHub repo URL (or org/repo)", true));
            addRepoCommand.add_option(
                dpp::command_option(dpp::co_string, "notes", "Optional notes to inject into the onboarding prompt", false));
            bot_->global_command_create(addRepoCommand);
        }
        if (onSlashCommandCreateChat_) {
            dpp::slashcommand createChatCommand(
                "create-chat", "Start a brand-new group chat with 2+ agents.", bot_->me.id);
            createChatCommand.add_option(dpp::command_option(
                dpp::co_string, "agents", "Space or comma separated agent names/ids (2 or more)", true));
            createChatCommand.add_option(
                dpp::command_option(dpp::co_string, "title", "Optional chat title", false));
            bot_->global_command_create(createChatCommand);
        }
        if (onSlashCommandCreateDm_) {
            dpp::slashcommand createDmCommand(
                "create-dm", "Start a fresh, private DM with a single agent (retires any existing one).",
                bot_->me.id);
            createDmCommand.add_option(
                dpp::command_option(dpp::co_string, "agent", "Agent name or id", true));
            bot_->global_command_create(createDmCommand);
        }
        if (onSlashCommandAddAgent_) {
            dpp::slashcommand addAgentCommand(
                "add-agent", "Add an agent to the chat this command is run in.", bot_->me.id);
            addAgentCommand.add_option(
                dpp::command_option(dpp::co_string, "agent", "Agent name or id", true));
            bot_->global_command_create(addAgentCommand);
        }
        if (onSlashCommandRemoveAgent_) {
            dpp::slashcommand removeAgentCommand(
                "remove-agent", "Remove an agent from the chat this command is run in (keeps history).",
                bot_->me.id);
            removeAgentCommand.add_option(
                dpp::command_option(dpp::co_string, "agent", "Agent name or id", true));
            bot_->global_command_create(removeAgentCommand);
        }
        if (onSlashCommandCloseChat_) {
            dpp::slashcommand closeChatCommand(
                "close-chat", "Archive this chat and delete its Discord channel.", bot_->me.id);
            bot_->global_command_create(closeChatCommand);
        }
        if (onSlashCommandCreateWorkspace_) {
            dpp::slashcommand createWorkspaceCommand(
                "create-workspace",
                "Create a git-worktree workspace across one or more already-imported repos.", bot_->me.id);
            createWorkspaceCommand.add_option(dpp::command_option(
                dpp::co_string, "repos", "Space or comma separated repo names/ids (1 or more)", true));
            createWorkspaceCommand.add_option(
                dpp::command_option(dpp::co_string, "title", "Optional workspace title", false));
            bot_->global_command_create(createWorkspaceCommand);
        }
    });

    bot_->on_message_create([this](const dpp::message_create_t& event) { HandleMessageCreate(event); });
    bot_->on_message_reaction_add([this](const dpp::message_reaction_add_t& event) { HandleReactionAdd(event); });
    bot_->on_slashcommand([this](const dpp::slashcommand_t& event) { HandleSlashCommand(event); });

    bot_->start(dpp::st_wait);
    runStartedAt_ = 0; // Run() returned normally (Stop() was called) — nothing to watch until the next attempt
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
    }

    // No explicit invite/addressing mechanism exists yet, so "every active
    // agent is a member of every channel-backed chat" is the whole
    // membership rule for now — checked (cheaply; AddParticipant is
    // INSERT OR IGNORE) on every incoming message, not just chat creation,
    // so an agent activated after a chat already existed still joins it.
    for (const Agent& agent : agentStore_.ListAll()) {
        if (agent.status == "active") {
            chatStore_.AddParticipant(chat.id, "agent", agent.id);
        }
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

void DiscordBot::HandleReactionAdd(const dpp::message_reaction_add_t& event) {
    if (event.reacting_user.is_bot()) {
        // Ignore the reactions we ourselves seed onto approval messages.
        return;
    }
    if (onReaction_) {
        onReaction_(std::to_string(event.message_id), event.reacting_emoji.name);
    }
}

namespace {
// get_parameter returns command_value by value — bind it to a named local
// before taking std::get_if's pointer into it, since a pointer into an
// unnamed temporary would dangle past the statement (same pattern add-repo's
// handler already used, factored out now that five more commands need it).
std::string GetStringParam(const dpp::slashcommand_t& event, const std::string& name) {
    const dpp::command_value param = event.get_parameter(name);
    if (const std::string* value = std::get_if<std::string>(&param)) {
        return *value;
    }
    return "";
}
} // namespace

void DiscordBot::HandleSlashCommand(const dpp::slashcommand_t& event) {
    const std::string name = event.command.get_command_name();

    if (name == "add-repo" && onSlashCommandAddRepo_) {
        const std::string url = GetStringParam(event, "url");
        const std::string notes = GetStringParam(event, "notes");
        // Discord requires an ack within 3 seconds — reply immediately, the
        // actual clone+onboarding work happens off this thread (see
        // Orchestrator::Run's wiring of this handler, which hands off to a
        // detached thread before calling Orchestrator::AddRepo).
        event.reply("Cloning and setting things up \xE2\x80\x94 I'll update you in the channel once it's ready.");
        onSlashCommandAddRepo_(url, notes);
        return;
    }

    if (name == "create-chat" && onSlashCommandCreateChat_) {
        const std::string agents = GetStringParam(event, "agents");
        const std::string title = GetStringParam(event, "title");
        // Pure bookkeeping (DB writes + one channel-create REST call) — see
        // the comment on SlashCommandCreateChatHandler in DiscordBot.h for
        // why this replies synchronously with the real result instead of
        // the ack-now/work-later pattern add-repo uses.
        event.reply(onSlashCommandCreateChat_(agents, title));
        return;
    }

    if (name == "create-dm" && onSlashCommandCreateDm_) {
        const std::string agent = GetStringParam(event, "agent");
        const std::string invokingChannelId = std::to_string(event.command.channel_id);
        // Ack first, same reasoning as close-chat below — this may delete a
        // previous DM channel, and that REST call (plus the one to create
        // the new channel) can be slow enough to blow Discord's 3-second ack
        // window if it ran before the reply.
        event.reply("Starting a fresh DM \xE2\x80\x94 I'll update you there once it's ready.");
        onSlashCommandCreateDm_(agent, invokingChannelId);
        return;
    }

    if (name == "add-agent" && onSlashCommandAddAgent_) {
        const std::string channelId = std::to_string(event.command.channel_id);
        const std::string agent = GetStringParam(event, "agent");
        event.reply(onSlashCommandAddAgent_(channelId, agent));
        return;
    }

    if (name == "remove-agent" && onSlashCommandRemoveAgent_) {
        const std::string channelId = std::to_string(event.command.channel_id);
        const std::string agent = GetStringParam(event, "agent");
        event.reply(onSlashCommandRemoveAgent_(channelId, agent));
        return;
    }

    if (name == "create-workspace" && onSlashCommandCreateWorkspace_) {
        const std::string repos = GetStringParam(event, "repos");
        const std::string title = GetStringParam(event, "title");
        // Synchronous, same as create-chat above — plain bookkeeping plus a
        // handful of Discord REST calls (category + channel create), never
        // an agent turn. The actual git worktree creation is a fast local
        // subprocess call, not a network round-trip, so this stays well
        // inside Discord's 3-second ack window in practice.
        event.reply(onSlashCommandCreateWorkspace_(repos, title));
        return;
    }

    if (name == "close-chat" && onSlashCommandCloseChat_) {
        const std::string channelId = std::to_string(event.command.channel_id);
        // Fire-and-forget, unlike the four handlers above — see
        // SlashCommandCloseChatHandler's comment in DiscordBot.h: the actual
        // channel deletion must happen strictly AFTER this ack is sent (you
        // can't reply into a channel you just deleted), so the real work
        // can't be "compute the reply text synchronously" here.
        event.reply("Archiving this chat and deleting the channel...");
        onSlashCommandCloseChat_(channelId);
        return;
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

std::string DiscordBot::PostAsAgent(
    const std::string& channelId, const std::string& agentId, const std::string& agentName,
    const std::string& content) {
    std::string webhookId, webhookToken;
    if (!EnsureWebhook(channelId, agentId, agentName, webhookId, webhookToken) || !bot_) {
        return "";
    }

    dpp::webhook hook;
    hook.id = std::stoull(webhookId);
    hook.token = webhookToken;

    // Discord rejects a single message over ~2000 chars outright — found
    // the hard way (a longer agent report got stored in the DB but never
    // appeared in Discord, with no error surfaced anywhere). Post every
    // chunk; the first chunk's message id is what gets tracked as "this
    // message's" Discord id.
    std::string firstMessageId;
    for (const std::string& chunk : ChunkForDiscord(content)) {
        dpp::message msg;
        msg.content = chunk;

        std::promise<dpp::confirmation_callback_t> promise;
        std::future<dpp::confirmation_callback_t> future = promise.get_future();
        // wait=true so the callback carries the posted dpp::message (with
        // its real id) instead of firing immediately with nothing.
        bot_->execute_webhook(
            hook, msg, true, 0, "",
            [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
        const dpp::confirmation_callback_t result = future.get();
        if (result.is_error()) {
            if (onLog_) {
                onLog_("execute_webhook failed for agent '" + agentId + "': " + result.get_error().message);
            }
            continue; // best-effort — a later chunk failing shouldn't lose earlier ones
        }
        const dpp::message posted = std::get<dpp::message>(result.value);
        if (firstMessageId.empty()) {
            firstMessageId = std::to_string(posted.id);
        }
    }
    return firstMessageId;
}

void DiscordBot::PostPlain(const std::string& channelId, const std::string& content) {
    if (!bot_) {
        return;
    }
    for (const std::string& chunk : ChunkForDiscord(content)) {
        bot_->message_create(dpp::message(std::stoull(channelId), chunk));
    }
}

bool DiscordBot::AddReaction(
    const std::string& channelId, const std::string& messageId, const std::string& emoji) {
    if (!bot_) {
        return false;
    }
    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->message_add_reaction(
        std::stoull(messageId), std::stoull(channelId), emoji,
        [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    return !result.is_error();
}

std::string DiscordBot::BotUserId() const {
    std::lock_guard<std::mutex> lock(botMutex_);
    if (!bot_ || bot_->me.id == 0) {
        return "";
    }
    return std::to_string(bot_->me.id);
}

void DiscordBot::ApplyPrivateChannelOverwrites(
    dpp::channel& c, const std::string& guildId, const std::string& humanUserId,
    const std::vector<std::string>& extraBotUserIds) {
    constexpr uint64_t kView = dpp::p_view_channel;
    constexpr uint64_t kUse = dpp::p_view_channel | dpp::p_send_messages | dpp::p_read_message_history;

    // Deny the @everyone role (its id is always the guild's own id) view
    // access, then explicitly allow the specific accounts that should see
    // this channel/category — that combination is what makes it "private."
    c.add_permission_overwrite(std::stoull(guildId), dpp::ot_role, 0, kView);
    c.add_permission_overwrite(std::stoull(humanUserId), dpp::ot_member, kUse, 0);
    const std::string ownBotUserId = BotUserId();
    if (!ownBotUserId.empty()) {
        c.add_permission_overwrite(std::stoull(ownBotUserId), dpp::ot_member, kUse, 0);
    }
    for (const std::string& extraBotUserId : extraBotUserIds) {
        if (!extraBotUserId.empty()) {
            c.add_permission_overwrite(std::stoull(extraBotUserId), dpp::ot_member, kUse, 0);
        }
    }
}

std::string DiscordBot::CreateDmChannel(
    const std::string& guildId, const std::string& channelName, const std::string& humanUserId,
    const std::vector<std::string>& extraBotUserIds, const std::string& parentCategoryId) {
    if (!bot_ || guildId.empty() || humanUserId.empty()) {
        return "";
    }

    dpp::channel c;
    c.set_name(channelName);
    c.set_type(dpp::CHANNEL_TEXT);
    c.set_guild_id(std::stoull(guildId));
    if (!parentCategoryId.empty()) {
        c.set_parent_id(std::stoull(parentCategoryId));
    }
    ApplyPrivateChannelOverwrites(c, guildId, humanUserId, extraBotUserIds);

    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->channel_create(c, [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    if (result.is_error()) {
        return "";
    }
    const dpp::channel created = std::get<dpp::channel>(result.value);
    return std::to_string(created.id);
}

std::string DiscordBot::CreateCategory(
    const std::string& guildId, const std::string& categoryName, const std::string& humanUserId,
    const std::vector<std::string>& extraBotUserIds) {
    if (!bot_ || guildId.empty() || humanUserId.empty()) {
        return "";
    }

    dpp::channel c;
    c.set_name(categoryName);
    c.set_type(dpp::CHANNEL_CATEGORY);
    c.set_guild_id(std::stoull(guildId));
    ApplyPrivateChannelOverwrites(c, guildId, humanUserId, extraBotUserIds);

    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->channel_create(c, [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    if (result.is_error()) {
        return "";
    }
    const dpp::channel created = std::get<dpp::channel>(result.value);
    return std::to_string(created.id);
}

bool DiscordBot::GrantChannelAccess(const std::string& channelId, const std::string& userId) {
    if (!bot_ || channelId.empty() || userId.empty()) {
        return false;
    }
    constexpr uint64_t kUse = dpp::p_view_channel | dpp::p_send_messages | dpp::p_read_message_history;

    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->channel_edit_permissions(
        std::stoull(channelId), std::stoull(userId), kUse, 0, /*member=*/true,
        [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    return !result.is_error();
}

bool DiscordBot::DeleteChannel(const std::string& channelId) {
    if (!bot_ || channelId.empty()) {
        return false;
    }
    std::promise<dpp::confirmation_callback_t> promise;
    std::future<dpp::confirmation_callback_t> future = promise.get_future();
    bot_->channel_delete(
        std::stoull(channelId), [&promise](const dpp::confirmation_callback_t& result) { promise.set_value(result); });
    const dpp::confirmation_callback_t result = future.get();
    return !result.is_error();
}

bool DiscordBot::IsZombied() const {
    std::lock_guard<std::mutex> lock(botMutex_);
    if (!bot_) {
        return false;
    }
    const dpp::shard_list shards = bot_->get_shards();
    const auto it = shards.find(0);
    if (it == shards.end() || it->second == nullptr) {
        return false;
    }
    const dpp::discord_client* shard = it->second;
    if (!shard->ready) {
        return false; // still (re)connecting — not zombied, just not up yet
    }
    // Discord's heartbeat interval is well under a minute; missing this long
    // means several heartbeats in a row went unacknowledged.
    constexpr time_t kZombieThresholdSeconds = 90;
    return (time(nullptr) - shard->last_heartbeat_ack) > kZombieThresholdSeconds;
}

bool DiscordBot::IsStuckConnecting() const {
    const time_t startedAt = runStartedAt_.load();
    if (startedAt == 0 || everReadyThisRun_.load()) {
        return false; // no attempt in flight, or it already succeeded
    }
    // A real connect (even a slow one) reaches on_ready within a few
    // seconds; this is generous headroom for a normal cold start, not a
    // tight timeout.
    constexpr time_t kStuckConnectingThresholdSeconds = 60;
    return (time(nullptr) - startedAt) > kStuckConnectingThresholdSeconds;
}

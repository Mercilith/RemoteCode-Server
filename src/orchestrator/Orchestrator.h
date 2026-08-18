#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <windows.h>

#include "../db/AgentSessionStore.h"
#include "../db/AgentStore.h"
#include "../db/ApprovalStore.h"
#include "../db/ChatStore.h"
#include "../db/Database.h"
#include "../discord/AgentBotClient.h"
#include "../discord/DiscordBot.h"
#include "../http/AdminServer.h"
#include "../third_party/json.hpp"
#include "../util/ActivityLog.h"

using LogFn = std::function<void(const std::wstring&)>;

// Ties the database, Discord bot, and per-turn worker spawning together.
// Blocks the calling thread until `shutdownEvent` is signaled — meant to be
// called directly from ServiceMain::RunMainLoop in place of the old MQTT
// loop.
class Orchestrator {
public:
    // Opens the database (creating/seeding it if needed), loads the Discord
    // config, connects the bot, and blocks until `shutdownEvent` is
    // signaled. If the config is missing/invalid, logs a clear message via
    // `log` and idles on `shutdownEvent` instead of crash-looping.
    void Run(HANDLE shutdownEvent, LogFn log);

    // DEBUG/DEV-ONLY: simulates an incoming human message without a real
    // Discord round-trip, for local testing/iteration of the message-
    // dispatch pipeline. Inserts the message, then runs the exact same
    // dispatch logic (HandleIncomingMessage) a real Discord message would
    // trigger, and returns a JSON summary of every message the resulting
    // dispatch produced. Wired up to AdminServer's `/debug/inject-message`
    // endpoint (loopback-only, no auth — see AdminServer.h). Fails clearly
    // (does not crash) if Discord isn't configured, since dispatch
    // unconditionally dereferences discordBot_ (PostAsAgent, EnsureDmChannel,
    // PostPlain, ...).
    nlohmann::json InjectTestMessage(
        const std::string& chatId, const std::string& content, const std::string& senderId);

private:
    // Runs a tag-driven dispatch loop for `chatId`: seeds a work queue with
    // every active participant agent (the unchanged default for a real
    // incoming user/Discord message), then for each turn mirrors every
    // message it produced to Discord and enqueues any agent it @tagged
    // (subject to can_message + the turn-limit guard) — see Mentions.h and
    // kMaxAgentChainTurns. Agent-to-agent follow-ups never wait on Discord:
    // the queue is driven entirely by what's in the DB.
    void HandleIncomingMessage(const std::string& chatId);
    // Trailing agent-authored messages in `chatId` since the last
    // user-authored one — the turn-limit guard's counter.
    int CountTrailingAgentTurns(const std::string& chatId);
    // Posts any newly-created approval drafts (from submit_agent_for_approval)
    // to Discord with the approve/reject reactions seeded on them.
    void PostPendingApprovals();
    void HandleReaction(const std::string& discordMessageId, const std::string& emoji);

    // Posts `content` into `channelId` as `agent` — via `agent`'s own
    // Discord bot if one is configured, otherwise the shared bot's
    // per-(channel,agent) webhook. Returns the posted Discord message id,
    // or an empty string on failure. The one place both reply-posting
    // (HandleIncomingMessage) and draft-posting (PostPendingApprovals)
    // funnel through, so they stay consistent as own-bot support was added
    // after both already existed.
    std::string PostAsAgent(const Agent& agent, const std::string& channelId, const std::string& content);
    // Lazily builds (and caches for the process lifetime) the REST-only
    // bot client for an agent's own token; rebuilt if the stored encrypted
    // token changes (e.g. reassigned via the admin API). Returns nullptr
    // if the agent has no own bot configured or the token fails to decrypt.
    AgentBotClient* GetOrCreateAgentBotClient(const Agent& agent);

    // Ensures `agent`'s private "dm-<id>" chat has a real Discord channel,
    // creating one (and persisting it via ChatStore::SetChatDiscordChannel)
    // if it doesn't yet. Returns the channel id, or an empty string if it
    // can't be created (no discordOwnerUserId configured, no guild id, or
    // the Discord API call failed).
    std::string EnsureDmChannel(const Agent& agent);

    LogFn log_;
    std::wstring dbPath_;
    std::wstring logDir_;
    std::string claudeConfigDir_;
    std::string discordGuildId_;
    std::string discordOwnerUserId_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<ChatStore> chatStore_;
    std::unique_ptr<AgentStore> agentStore_;
    std::unique_ptr<ApprovalStore> approvalStore_;
    std::unique_ptr<AgentSessionStore> agentSessionStore_;
    std::unique_ptr<DiscordBot> discordBot_;
    std::unique_ptr<AdminServer> adminServer_;
    std::unique_ptr<ActivityLog> activityLog_;

    struct CachedAgentBotClient {
        std::string encryptedToken; // what the client was built from, to detect a reassigned token
        std::unique_ptr<AgentBotClient> client;
    };
    std::mutex agentBotClientsMutex_;
    std::unordered_map<std::string, CachedAgentBotClient> agentBotClients_;
};

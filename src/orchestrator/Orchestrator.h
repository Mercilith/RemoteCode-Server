#pragma once

#include <functional>
#include <memory>
#include <string>
#include <windows.h>

#include "../db/AgentStore.h"
#include "../db/ApprovalStore.h"
#include "../db/ChatStore.h"
#include "../db/Database.h"
#include "../discord/DiscordBot.h"

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

private:
    void HandleIncomingMessage(const std::string& chatId);
    // Posts any newly-created approval drafts (from submit_agent_for_approval)
    // to Discord with the approve/reject reactions seeded on them.
    void PostPendingApprovals();
    void HandleReaction(const std::string& discordMessageId, const std::string& emoji);

    LogFn log_;
    std::wstring dbPath_;
    std::string claudeConfigDir_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<ChatStore> chatStore_;
    std::unique_ptr<AgentStore> agentStore_;
    std::unique_ptr<ApprovalStore> approvalStore_;
    std::unique_ptr<DiscordBot> discordBot_;
};

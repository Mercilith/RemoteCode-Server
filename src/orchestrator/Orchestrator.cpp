#include "Orchestrator.h"

#include <shlobj.h>

#include <ctime>
#include <filesystem>
#include <thread>

#include "../config/ServerConfig.h"
#include "../db/Schema.h"
#include "AgentTurn.h"

namespace fs = std::filesystem;

namespace {

// IDs and error text handled here are always ASCII (generated identifiers,
// hand-written error strings) — a byte-for-byte widen is exact.
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::wstring DbPath() {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) ||
        programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    const std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\ServerData";
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + L"\\orchestrator.db";
}

} // namespace

void Orchestrator::Run(HANDLE shutdownEvent, LogFn log) {
    log_ = std::move(log);

    dbPath_ = DbPath();
    if (dbPath_.empty()) {
        log_(L"Orchestrator: could not resolve %ProgramData% — idling.");
        WaitForSingleObject(shutdownEvent, INFINITE);
        return;
    }

    db_ = std::make_unique<Database>();
    if (!db_->Open(dbPath_) || !Schema::EnsureCreated(*db_)) {
        log_(L"Orchestrator: failed to open/initialize database at " + dbPath_ + L" — idling.");
        WaitForSingleObject(shutdownEvent, INFINITE);
        return;
    }

    chatStore_ = std::make_unique<ChatStore>(*db_);
    agentStore_ = std::make_unique<AgentStore>(*db_);
    agentStore_->SeedAlexIfEmpty();

    const ServerConfig config = ServerConfigStore::Load();
    if (!config.valid) {
        log_(L"Orchestrator: no Discord bot token configured (see config/ServerConfig.h for setup "
             L"instructions) - idling.");
        WaitForSingleObject(shutdownEvent, INFINITE);
        return;
    }
    claudeConfigDir_ = config.claudeConfigDir;

    discordBot_ = std::make_unique<DiscordBot>(config.discordBotToken, *chatStore_);
    discordBot_->SetLogHandler([this](const std::string& message) { log_(L"Discord: " + AsciiToWide(message)); });
    discordBot_->SetIncomingMessageHandler([this](const std::string& chatId) {
        // Runs on DPP's gateway thread — hop to a detached worker thread so
        // a slow agent turn (spawning Node, running the Agent SDK) can't
        // stall the websocket heartbeat. Acceptable for this pass's single-
        // agent scale; a real work queue can replace this later.
        std::thread([this, chatId]() { HandleIncomingMessage(chatId); }).detach();
    });

    log_(L"Orchestrator: connecting to Discord...");
    std::thread discordThread([this]() { discordBot_->Run(); });

    WaitForSingleObject(shutdownEvent, INFINITE);

    log_(L"Orchestrator: shutting down Discord connection...");
    discordBot_->Stop();
    discordThread.join();
}

void Orchestrator::HandleIncomingMessage(const std::string& chatId) {
    Chat chat;
    if (!chatStore_->GetChat(chatId, chat)) {
        return;
    }

    const std::vector<std::string> agentIds = chatStore_->ListParticipantAgentIds(chatId);
    for (const std::string& agentId : agentIds) {
        Agent agent;
        if (!agentStore_->Get(agentId, agent)) {
            continue;
        }

        const std::vector<Message> recent = chatStore_->RecentMessages(chatId, 50);
        const AgentTurnResult turnResult = AgentTurn::Run(agent, recent, dbPath_, claudeConfigDir_);
        if (!turnResult.ok) {
            log_(L"Orchestrator: turn failed for agent '" + AsciiToWide(agent.id) + L"': " +
                 AsciiToWide(turnResult.error));
            continue;
        }

        Message reply;
        reply.chatId = chatId;
        reply.senderType = "agent";
        reply.senderId = agent.id;
        reply.type = "text";
        reply.content = turnResult.response;
        reply.createdAt = static_cast<int64_t>(time(nullptr));
        chatStore_->InsertMessage(reply);

        if (!chat.discordChannelId.empty()) {
            discordBot_->PostAsAgent(chat.discordChannelId, agent.id, agent.name, turnResult.response);
        }
    }
}

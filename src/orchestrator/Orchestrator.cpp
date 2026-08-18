#include "Orchestrator.h"

#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>

#include "../config/Secrets.h"
#include "../config/ServerConfig.h"
#include "../db/Schema.h"
#include "../third_party/json.hpp"
#include "AgentTurn.h"

using nlohmann::json;

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

// Discord's actual UTF-8 bytes for the two reaction emoji this pass's
// approval workflow understands.
constexpr const char* kApproveEmoji = "\xE2\x9C\x85"; // check mark
constexpr const char* kRejectEmoji = "\xE2\x9D\x8C";  // cross mark

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
    approvalStore_ = std::make_unique<ApprovalStore>(*db_);
    agentSessionStore_ = std::make_unique<AgentSessionStore>(*db_);
    agentStore_->SeedAlexIfEmpty();

    const ServerConfig config = ServerConfigStore::Load();
    claudeConfigDir_ = config.claudeConfigDir;

    // Started regardless of Discord config validity — agent management
    // (list/create/edit, assigning a bot token) is useful even before
    // Discord is set up, and /revise degrades gracefully (Alex just can't
    // post anywhere, but can still update the agent record).
    adminServer_ =
        std::make_unique<AdminServer>(*agentStore_, *agentSessionStore_, dbPath_, claudeConfigDir_);
    std::thread adminThread([this]() { adminServer_->Run(kAdminServerPort); });
    log_(L"Orchestrator: admin API listening on 127.0.0.1:" + std::to_wstring(kAdminServerPort) + L".");

    if (!config.valid) {
        log_(L"Orchestrator: no Discord bot token configured (see config/ServerConfig.h for setup "
             L"instructions) - idling (agent management via the admin API still works).");
        WaitForSingleObject(shutdownEvent, INFINITE);
        adminServer_->Stop();
        adminThread.join();
        return;
    }

    discordBot_ = std::make_unique<DiscordBot>(config.discordBotToken, *chatStore_);
    discordBot_->SetLogHandler([this](const std::string& message) { log_(L"Discord: " + AsciiToWide(message)); });
    discordBot_->SetIncomingMessageHandler([this](const std::string& chatId) {
        // Runs on DPP's gateway thread — hop to a detached worker thread so
        // a slow agent turn (spawning Node, running the Agent SDK) can't
        // stall the websocket heartbeat. Acceptable for this pass's single-
        // agent scale; a real work queue can replace this later.
        std::thread([this, chatId]() { HandleIncomingMessage(chatId); }).detach();
    });
    discordBot_->SetReactionHandler([this](const std::string& discordMessageId, const std::string& emoji) {
        std::thread([this, discordMessageId, emoji]() { HandleReaction(discordMessageId, emoji); }).detach();
    });

    // discordBot_->Stop() is the one mechanism used both for a real
    // shutdown and for a watchdog-triggered reconnect — `stopping` is what
    // lets the loop below tell the two apart once Run() returns.
    std::atomic<bool> stopping{false};

    std::thread discordThread([this, &stopping]() {
        while (!stopping.load()) {
            log_(L"Orchestrator: connecting to Discord...");
            discordBot_->Run(); // blocks until Stop() is called
            if (stopping.load()) {
                break;
            }
            log_(L"Orchestrator: Discord connection ended unexpectedly — reconnecting in 5s...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    // Watchdog: DPP can end up in a state where the local socket looks
    // connected and keeps sending outbound heartbeats on its own timer, but
    // the remote end has silently stopped responding (observed after a bad
    // resume) — no crash, no disconnect event, just a connection that never
    // delivers another message again. Poll for that and force a reconnect
    // rather than requiring a manual service restart.
    std::thread watchdogThread([this, &stopping]() {
        while (!stopping.load()) {
            for (int i = 0; i < 30 && !stopping.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (stopping.load()) {
                break;
            }
            if (discordBot_->IsZombied()) {
                log_(L"Orchestrator: Discord connection looks zombied (no heartbeat ACK in a while) — "
                     L"forcing a reconnect.");
                discordBot_->Stop();
            }
        }
    });

    WaitForSingleObject(shutdownEvent, INFINITE);

    stopping = true;
    log_(L"Orchestrator: shutting down Discord connection...");
    discordBot_->Stop();
    discordThread.join();
    watchdogThread.join();

    adminServer_->Stop();
    adminThread.join();
}

void Orchestrator::HandleIncomingMessage(const std::string& chatId) {
    Chat chat;
    if (!chatStore_->GetChat(chatId, chat)) {
        return;
    }

    const std::vector<std::string> agentIds = chatStore_->ListParticipantAgentIds(chatId);
    for (const std::string& agentId : agentIds) {
        Agent agent;
        if (!agentStore_->Get(agentId, agent) || agent.status != "active") {
            continue;
        }

        std::string resumeSessionId;
        agentSessionStore_->Get(agent.id, chatId, resumeSessionId);

        const std::vector<Message> recent = chatStore_->RecentMessages(chatId, 50);
        const AgentTurnResult turnResult =
            AgentTurn::Run(agent, recent, dbPath_, claudeConfigDir_, chatId, resumeSessionId);
        if (!turnResult.ok) {
            log_(L"Orchestrator: turn failed for agent '" + AsciiToWide(agent.id) + L"': " +
                 AsciiToWide(turnResult.error));
            if (!resumeSessionId.empty()) {
                // The resume itself may be what failed (stale/missing
                // session file) — drop it so the next turn starts fresh
                // with full history instead of repeatedly failing the
                // same way.
                agentSessionStore_->Clear(agent.id, chatId);
            }
            continue;
        }
        if (!turnResult.sdkSessionId.empty()) {
            agentSessionStore_->Set(agent.id, chatId, turnResult.sdkSessionId);
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
            PostAsAgent(agent, chat.discordChannelId, turnResult.response);
        }
    }

    // A turn above may have called submit_agent_for_approval, which only
    // writes to the DB (the MCP server is a separate subprocess with no
    // live Discord connection) — post any such drafts now that we're back
    // on the orchestrator's own thread with a real DiscordBot.
    PostPendingApprovals();
}

void Orchestrator::PostPendingApprovals() {
    for (const Approval& approval : approvalStore_->ListUnposted()) {
        Message message;
        if (!chatStore_->GetMessageById(approval.messageId, message)) {
            continue;
        }
        Chat chat;
        if (!chatStore_->GetChat(approval.chatId, chat) || chat.discordChannelId.empty()) {
            continue;
        }

        Agent requester;
        if (!agentStore_->Get(approval.requestedBy, requester)) {
            requester = Agent{}; // fall back to a minimal stand-in (id-as-name, no own bot)
            requester.id = approval.requestedBy;
            requester.name = approval.requestedBy;
        }

        const std::string discordMessageId = PostAsAgent(requester, chat.discordChannelId, message.content);
        if (discordMessageId.empty()) {
            continue;
        }

        chatStore_->SetMessageDiscordId(approval.messageId, discordMessageId);
        discordBot_->AddReaction(chat.discordChannelId, discordMessageId, kApproveEmoji);
        discordBot_->AddReaction(chat.discordChannelId, discordMessageId, kRejectEmoji);
    }
}

void Orchestrator::HandleReaction(const std::string& discordMessageId, const std::string& emoji) {
    const bool isApprove = emoji == kApproveEmoji;
    const bool isReject = emoji == kRejectEmoji;
    if (!isApprove && !isReject) {
        return;
    }

    Message message;
    if (!chatStore_->GetMessageByDiscordId(discordMessageId, message)) {
        return;
    }

    Approval approval;
    if (!approvalStore_->GetByMessageId(message.id, approval) || approval.status != "pending") {
        return; // not an approval message, or someone already resolved it
    }

    const std::string newStatus = isApprove ? "approved" : "rejected";
    approvalStore_->Resolve(approval.id, newStatus, static_cast<int64_t>(time(nullptr)));

    if (approval.kind != "create_agent") {
        return; // no other approval kinds exist yet
    }

    json payload;
    try {
        payload = json::parse(approval.payloadJson);
    } catch (const json::parse_error&) {
        return;
    }
    const std::string agentId = payload.value("agent_id", "");

    Agent agent;
    if (agentId.empty() || !agentStore_->Get(agentId, agent)) {
        return;
    }
    agent.status = isApprove ? "active" : "disabled";
    agent.updatedAt = static_cast<int64_t>(time(nullptr));
    agentStore_->Upsert(agent);

    const std::string confirmationText = isApprove
        ? ("Agent '" + agent.name + "' approved and is now active.")
        : ("Agent '" + agent.name + "' was rejected.");

    Message reply;
    reply.chatId = message.chatId;
    reply.senderType = "system";
    reply.senderId = "system";
    reply.type = "system_event";
    reply.content = confirmationText;
    reply.createdAt = static_cast<int64_t>(time(nullptr));
    chatStore_->InsertMessage(reply);

    Chat chat;
    if (chatStore_->GetChat(message.chatId, chat) && !chat.discordChannelId.empty()) {
        // Plain bot message, not a webhook identity — no single agent
        // "said" this, it's a system-level confirmation.
        discordBot_->PostPlain(chat.discordChannelId, confirmationText);
    }
}

std::string Orchestrator::PostAsAgent(const Agent& agent, const std::string& channelId, const std::string& content) {
    if (AgentBotClient* ownBot = GetOrCreateAgentBotClient(agent)) {
        return ownBot->PostAsSelf(channelId, content);
    }
    return discordBot_->PostAsAgent(channelId, agent.id, agent.name, content);
}

AgentBotClient* Orchestrator::GetOrCreateAgentBotClient(const Agent& agent) {
    if (agent.discordBotTokenEncrypted.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(agentBotClientsMutex_);
    auto it = agentBotClients_.find(agent.id);
    if (it != agentBotClients_.end() && it->second.encryptedToken == agent.discordBotTokenEncrypted) {
        return it->second.client.get();
    }

    const std::string token = Secrets::Unprotect(agent.discordBotTokenEncrypted);
    if (token.empty()) {
        log_(L"Orchestrator: could not decrypt stored bot token for agent '" + AsciiToWide(agent.id) +
             L"' — falling back to shared-bot posting.");
        agentBotClients_.erase(agent.id);
        return nullptr;
    }

    CachedAgentBotClient entry;
    entry.encryptedToken = agent.discordBotTokenEncrypted;
    entry.client = std::make_unique<AgentBotClient>(token);
    AgentBotClient* raw = entry.client.get();
    agentBotClients_[agent.id] = std::move(entry);
    return raw;
}

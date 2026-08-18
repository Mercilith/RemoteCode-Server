#include "Orchestrator.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <thread>

#include "../config/Secrets.h"
#include "../config/ServerConfig.h"
#include "../db/Schema.h"
#include "../third_party/json.hpp"
#include "../util/Mentions.h"
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

// Caps how many agent-authored messages can pile up (since the last thing
// a human said) before auto-dispatch stops — a blunt but effective guard
// against any @tag cycle (A->B->A, fan-out, ...) regardless of its shape.
constexpr int kMaxAgentChainTurns = 8;

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

    // Sits next to the DB file (…\ServerData\logs\) — see util/ActivityLog.
    // Per-turn detail (tool calls, blocked mentions, turn-limit trips) goes
    // here as JSON lines, not into the DB, so it can be tailed/greped
    // directly without a query tool.
    {
        const size_t slash = dbPath_.find_last_of(L"\\/");
        const std::wstring serverDataDir = slash == std::wstring::npos ? L"." : dbPath_.substr(0, slash);
        logDir_ = serverDataDir + L"\\logs";
    }
    activityLog_ = std::make_unique<ActivityLog>(logDir_, "orchestrator");

    const ServerConfig config = ServerConfigStore::Load();
    claudeConfigDir_ = config.claudeConfigDir;
    discordGuildId_ = config.discordGuildId;
    discordOwnerUserId_ = config.discordOwnerUserId;

    // Started regardless of Discord config validity — agent management
    // (list/create/edit, assigning a bot token) is useful even before
    // Discord is set up, and /revise degrades gracefully (Alex just can't
    // post anywhere, but can still update the agent record).
    adminServer_ = std::make_unique<AdminServer>(
        *agentStore_, *agentSessionStore_, *chatStore_, dbPath_, claudeConfigDir_, logDir_,
        [this](const std::string& chatId, const std::string& content, const std::string& senderId) {
            return InjectTestMessage(chatId, content, senderId);
        });
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

    discordBot_ = std::make_unique<DiscordBot>(config.discordBotToken, *chatStore_, *agentStore_);
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

// DEBUG/DEV-ONLY — see the declaration in Orchestrator.h.
json Orchestrator::InjectTestMessage(
    const std::string& chatId, const std::string& content, const std::string& senderId) {
    if (!discordBot_) {
        return json{
            {"error",
             "Discord is not configured — dispatch requires a live DiscordBot for posting/mirroring"}};
    }

    Chat chat;
    if (!chatStore_->GetChat(chatId, chat)) {
        return json{{"error", "no chat with that id"}};
    }

    // Watermark taken BEFORE the insert so MessagesAfter below picks up
    // both the injected message and everything the dispatch produces from
    // it.
    const int64_t watermark = chatStore_->LatestMessageId(chatId);

    Message injected;
    injected.chatId = chatId;
    injected.senderType = "user";
    injected.senderId = senderId.empty() ? "debug-user" : senderId;
    injected.type = "text";
    injected.content = content;
    injected.createdAt = static_cast<int64_t>(time(nullptr));
    if (chatStore_->InsertMessage(injected) < 0) {
        return json{{"error", "failed to insert the test message"}};
    }

    // Same dispatch loop a real Discord message would trigger — runs
    // synchronously on the HTTP handler thread, same as /agents/:id/revise.
    HandleIncomingMessage(chatId);

    json messages = json::array();
    for (const Message& m : chatStore_->MessagesAfter(chatId, watermark)) {
        messages.push_back(json{
            {"id", m.id},
            {"sender_type", m.senderType},
            {"sender_id", m.senderId},
            {"type", m.type},
            {"content", m.content},
        });
    }
    return json{{"chat_id", chatId}, {"messages", messages}};
}

void Orchestrator::HandleIncomingMessage(const std::string& chatId) {
    Chat chat;
    if (!chatStore_->GetChat(chatId, chat)) {
        return;
    }

    const std::vector<std::string> participantIds = chatStore_->ListParticipantAgentIds(chatId);
    // Every tag ("@agentB") is resolved against this — the full participant
    // roster, not just whoever's currently queued — so an agent can address
    // any teammate in the chat, active or not (inactive/unknown ones just
    // won't actually get dispatched below).
    std::vector<Agent> participants;
    for (const std::string& id : participantIds) {
        Agent a;
        if (agentStore_->Get(id, a)) {
            participants.push_back(a);
        }
    }

    // Seed: if the triggering (human) message explicitly @tagged one or
    // more participants, only they run — otherwise fall back to broadcasting
    // to every participant, same as before tagging existed. Without this,
    // a message like "@alex ..." would still wake every other agent in the
    // chat via the broadcast *and* whoever Alex goes on to tag, double-
    // firing them. Anything queued after this point instead comes from an
    // agent explicitly @tagging another agent (see below).
    std::string triggeringContent;
    const std::vector<Message> latest = chatStore_->RecentMessages(chatId, 1);
    if (!latest.empty()) {
        triggeringContent = latest.back().content;
    }
    const std::vector<std::string> taggedInTrigger = Mentions::ParseMentions(triggeringContent, participants);
    const std::vector<std::string>& seedIds = taggedInTrigger.empty() ? participantIds : taggedInTrigger;
    std::deque<std::string> queue(seedIds.begin(), seedIds.end());

    while (!queue.empty()) {
        const std::string agentId = queue.front();
        queue.pop_front();

        Agent agent;
        if (!agentStore_->Get(agentId, agent) || agent.status != "active") {
            continue;
        }

        if (CountTrailingAgentTurns(chatId) >= kMaxAgentChainTurns) {
            log_(L"Orchestrator: turn limit reached in chat '" + AsciiToWide(chatId) + L"' — pausing "
                 L"auto-dispatch until you say something.");
            activityLog_->Log(chatId, agentId, "turn_limit_reached");

            const std::string notice = "Turn limit reached (" + std::to_string(kMaxAgentChainTurns) +
                " agent turns in a row) — waiting for your input.";
            Message noticeMessage;
            noticeMessage.chatId = chatId;
            noticeMessage.senderType = "system";
            noticeMessage.senderId = "system";
            noticeMessage.type = "system_event";
            noticeMessage.content = notice;
            noticeMessage.createdAt = static_cast<int64_t>(time(nullptr));
            chatStore_->InsertMessage(noticeMessage);
            if (!chat.discordChannelId.empty()) {
                discordBot_->PostPlain(chat.discordChannelId, notice);
            }
            break;
        }

        activityLog_->Log(chatId, agent.id, "turn_start");

        // Global (not chat-scoped) watermark — a turn's messages aren't
        // confined to the chat it ran in (message_user writes into the
        // agent's own separate DM chat), so MessagesBySenderAfter below
        // needs a watermark that covers every chat.
        const int64_t maxIdBeforeTurn = chatStore_->LatestMessageId();

        std::string resumeSessionId;
        agentSessionStore_->Get(agent.id, chatId, resumeSessionId);

        const std::vector<Message> recent = chatStore_->RecentMessages(chatId, 50);
        const AgentTurnResult turnResult =
            AgentTurn::Run(agent, recent, dbPath_, claudeConfigDir_, chatId, resumeSessionId, logDir_);
        if (!turnResult.ok) {
            log_(L"Orchestrator: turn failed for agent '" + AsciiToWide(agent.id) + L"': " +
                 AsciiToWide(turnResult.error));
            activityLog_->Log(chatId, agent.id, "turn_end", json{{"ok", false}, {"error", turnResult.error}});
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
        activityLog_->Log(chatId, agent.id, "turn_end", json{{"ok", true}});

        Message reply;
        reply.chatId = chatId;
        reply.senderType = "agent";
        reply.senderId = agent.id;
        reply.type = "text";
        reply.content = turnResult.response;
        reply.createdAt = static_cast<int64_t>(time(nullptr));
        chatStore_->InsertMessage(reply);

        // Everything this turn produced, in ANY chat — the primary reply
        // just inserted above, plus any post_message calls the MCP
        // subprocess made mid-turn (it only ever writes to the DB; it has
        // no live Discord connection of its own), plus any message_user
        // calls (which write into this agent's own separate DM chat) —
        // gets mirrored to Discord and scanned for tags here, on the
        // orchestrator's own thread, in one place.
        for (const Message& produced : chatStore_->MessagesBySenderAfter(agent.id, maxIdBeforeTurn)) {
            // approval_request messages are posted separately by
            // PostPendingApprovals (with the approve/reject reactions
            // seeded on them) — mirroring them here first would leave them
            // with a discord_message_id already set and skip that step.
            if (produced.type == "approval_request") {
                continue;
            }

            Chat producedChat;
            if (!chatStore_->GetChat(produced.chatId, producedChat)) {
                continue;
            }

            // A DM chat is created without a Discord channel — create one
            // now, the first time it actually has something to post.
            std::string discordChannelId = producedChat.discordChannelId;
            if (discordChannelId.empty() && produced.chatId.rfind("dm-", 0) == 0) {
                discordChannelId = EnsureDmChannel(agent);
            }

            if (!discordChannelId.empty() && produced.discordMessageId.empty()) {
                const std::string discordText = Mentions::ReflectMentionsForDiscord(produced.content, participants);
                const std::string discordMessageId = PostAsAgent(agent, discordChannelId, discordText);
                if (!discordMessageId.empty()) {
                    chatStore_->SetMessageDiscordId(produced.id, discordMessageId);
                }
            }

            // A DM chat has exactly one agent participant — nothing to
            // address, so tag-driven dispatch never runs against it.
            if (produced.chatId.rfind("dm-", 0) == 0) {
                continue;
            }

            for (const std::string& targetId : Mentions::ParseMentions(produced.content, participants)) {
                if (targetId == produced.senderId) {
                    continue; // no self-trigger
                }
                if (!Mentions::IsAllowedToMessage(agent, targetId)) {
                    activityLog_->Log(chatId, produced.senderId, "mention_blocked", json{{"target", targetId}});
                    continue;
                }
                // Don't double-queue an agent that's already waiting to run
                // this pass (e.g. two different messages in the same turn
                // both tagging the same target) — a *later*, separate
                // dispatch pass tagging them again is still fine and is
                // what the turn-limit guard exists for.
                if (std::find(queue.begin(), queue.end(), targetId) == queue.end()) {
                    queue.push_back(targetId);
                }
            }
        }
    }

    // A turn above may have called submit_agent_for_approval, which only
    // writes to the DB (the MCP server is a separate subprocess with no
    // live Discord connection) — post any such drafts now that we're back
    // on the orchestrator's own thread with a real DiscordBot.
    PostPendingApprovals();
}

int Orchestrator::CountTrailingAgentTurns(const std::string& chatId) {
    const std::vector<Message> recent = chatStore_->RecentMessages(chatId, 50);
    int count = 0;
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->senderType == "user") {
            break;
        }
        if (it->senderType == "agent") {
            ++count;
        }
    }
    return count;
}

std::string Orchestrator::EnsureDmChannel(const Agent& agent) {
    const std::string dmChatId = "dm-" + agent.id;
    Chat dmChat;
    if (!chatStore_->GetChat(dmChatId, dmChat)) {
        return ""; // message_user creates the chat row; nothing to do if it hasn't yet
    }
    if (!dmChat.discordChannelId.empty()) {
        return dmChat.discordChannelId;
    }
    if (discordGuildId_.empty() || discordOwnerUserId_.empty()) {
        log_(L"Orchestrator: agent '" + AsciiToWide(agent.id) + L"' tried to message you directly, but "
             L"no discord_owner_user_id is configured (see config/ServerConfig.h) — dropping the "
             L"message rather than posting it somewhere unintended.");
        return "";
    }

    const std::string channelId = discordBot_->CreateDmChannel(
        discordGuildId_, agent.id + "-dm", discordOwnerUserId_, agent.discordBotUserId);
    if (channelId.empty()) {
        log_(L"Orchestrator: failed to create a private DM channel for agent '" + AsciiToWide(agent.id) + L"'.");
        return "";
    }
    chatStore_->SetChatDiscordChannel(dmChatId, channelId);
    return channelId;
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

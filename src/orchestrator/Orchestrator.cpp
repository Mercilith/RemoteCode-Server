#include "Orchestrator.h"

#include <shlobj.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <thread>

#include "../config/Secrets.h"
#include "../config/ServerConfig.h"
#include "../db/Schema.h"
#include "../third_party/json.hpp"
#include "../util/GitHubRepo.h"
#include "../util/Mentions.h"
#include "../util/ProcessRunner.h"
#include "../util/Text.h"
#include "AgentTurn.h"
#include "WorkspaceCreator.h"

using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// IDs and error text handled here are always ASCII (generated identifiers,
// hand-written error strings) — a byte-for-byte widen is exact.
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Real UTF-8 conversion (unlike AsciiToWide above) — needed for
// RepoLocalPath, which is built from %ProgramData% (via
// SHGetKnownFolderPath) and can in principle contain non-ASCII characters on
// a non-English Windows install.
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Resolves discord.com via a throwaway Winsock session — used to gate the
// very first (and every re-)connect attempt so DPP never gets handed a
// doomed request. Observed for real after a Windows crash/reboot: the
// service's Automatic-start trigger fires before the network stack/DNS
// resolver is actually usable, so the first REST call inside
// dpp::cluster::start() failed with "getaddrinfo: No such host is known" and
// DPP's own retry handling for that specific failure never recovered on its
// own (see DiscordBot::IsStuckConnecting's comment) — required a manual
// service restart. Self-contained WSAStartup/WSACleanup rather than relying
// on DPP having already initialized Winsock, since this runs before the
// first dpp::cluster is even constructed.
bool CanResolveDiscordHost() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const bool ok = getaddrinfo("discord.com", "443", &hints, &result) == 0;
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    WSACleanup();
    return ok;
}

// Used to decide whether an untagged agent's turn actually said something
// or genuinely chose to stay silent (a bare response of "  \n" shouldn't
// count as a reply any more than "" does).
std::string TrimWhitespace(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// True if `trimmed` is (modulo case and a little stray punctuation/markup
// models tend to add despite instructions not to, e.g. "*SILENT.*") the
// SILENT sentinel worker/src/index.ts's prompt asks for. Deliberately more
// lenient than an exact match — an untagged agent choosing not to reply
// should never accidentally count as "said something."
bool IsSilenceSentinel(const std::string& trimmed) {
    std::string s = trimmed;
    while (!s.empty() && std::string("*_.!\"'").find(s.back()) != std::string::npos) {
        s.pop_back();
    }
    while (!s.empty() && std::string("*_\"'").find(s.front()) != std::string::npos) {
        s.erase(s.begin());
    }
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "silent";
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

// %ProgramData%\RemoteCode\Repos\<repoId> — mirrors DbPath()'s
// SHGetKnownFolderPath(FOLDERID_ProgramData) + create_directories pattern.
// Returns an empty wstring if %ProgramData% can't be resolved.
std::wstring RepoLocalPath(const std::string& repoId) {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) ||
        programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    const std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\Repos\\" + AsciiToWide(repoId);
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(fs::path(dir).parent_path(), ec); // ensure ...\Repos\ exists; the repo dir itself is gh's job (or already exists)
    return dir;
}

// Splits a slash command's free-text "agents" option (space and/or comma
// separated) into individual name/id tokens — dpp has no good multi-select
// option type, so /create-chat takes one string option and this is how it's
// broken apart before each token is resolved via
// Orchestrator::ResolveActiveAgentByNameOrId.
std::vector<std::string> SplitAgentTokens(const std::string& raw) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : raw) {
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

// Discord's actual UTF-8 bytes for the two reaction emoji this pass's
// approval workflow understands.
constexpr const char* kApproveEmoji = "\xE2\x9C\x85"; // check mark
constexpr const char* kRejectEmoji = "\xE2\x9D\x8C";  // cross mark

// Caps how many agent-authored messages can pile up (since the last thing
// a human said) before auto-dispatch stops — a blunt but effective guard
// against any @tag cycle (A->B->A, fan-out, ...) regardless of its shape.
constexpr int kMaxAgentChainTurns = 8;

// Design-spec Section 12, Decision #4: a session unused for over an hour
// is stale — the next turn starts fresh (full context replay) instead of
// resuming it, rather than resuming indefinitely.
constexpr int64_t kSessionIdleTimeoutSeconds = 3600;

// How many messages may accumulate past the chat_summaries watermark before
// a refresh runs — rare housekeeping, not a per-turn cost multiplier (see
// RefreshChatSummaryIfNeeded, called at most once per HandleIncomingMessage
// call).
constexpr int64_t kSummaryThreshold = 60;

// Caps how many messages since the summary watermark get fed into the
// summarizer turn itself, so that call doesn't blow its own context if the
// chat has grown well past kSummaryThreshold before a refresh happens to
// run (e.g. after downtime).
constexpr int kSummaryInputCap = 150;

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
    chatSummaryStore_ = std::make_unique<ChatSummaryStore>(*db_);
    repoStore_ = std::make_unique<RepoStore>(*db_);
    workspaceStore_ = std::make_unique<WorkspaceStore>(*db_);
    tempPermissionStore_ = std::make_unique<TempPermissionStore>(*db_);
    promptTemplateStore_ = std::make_unique<PromptTemplateStore>(*db_);
    taskStore_ = std::make_unique<TaskStore>(*db_);
    reminderStore_ = std::make_unique<ReminderStore>(*db_);
    agentStore_->SeedAlexIfEmpty();
    promptTemplateStore_->SeedDefaultsIfEmpty();

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
        *agentStore_, *agentSessionStore_, *chatStore_, *repoStore_, *workspaceStore_, *promptTemplateStore_,
        dbPath_, claudeConfigDir_, logDir_,
        [this](const std::string& chatId, const std::string& content, const std::string& senderId) {
            return InjectTestMessage(chatId, content, senderId);
        },
        [this](const std::string& githubUrl, const std::string& notes, std::string& outError) {
            return AddRepo(githubUrl, notes, outError);
        },
        [this](const std::string& repoId, std::string& outError) { return RetryRepo(repoId, outError); },
        [this](const std::vector<std::string>& repoIdsOrNames, const std::string& title, std::string& outError) {
            return CreateWorkspace(repoIdsOrNames, title, "", outError);
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
    discordBot_->SetSlashCommandAddRepoHandler([this](const std::string& url, const std::string& notes) {
        // Same detached-thread pattern as the handlers above — the reply
        // ack already happened inside DiscordBot::HandleSlashCommand before
        // this fires, so there's no 3-second deadline here; AddRepo itself
        // is fast (just a DB insert) and spawns its own further background
        // thread for the actual clone+onboarding work.
        std::thread([this, url, notes]() {
            std::string error;
            AddRepo(url, notes, error);
        }).detach();
    });
    discordBot_->SetSlashCommandCreateChatHandler([this](const std::string& agentsRaw, const std::string& title) {
        return HandleSlashCommandCreateChat(agentsRaw, title);
    });
    discordBot_->SetSlashCommandCreateDmHandler(
        [this](const std::string& agentRaw, const std::string& invokingChannelId) {
            // Detached, same as add-repo/close-chat above — may need to
            // delete a Discord channel, which has to happen strictly after
            // the ack DiscordBot::HandleSlashCommand already sent, not
            // before a synchronous reply is computed.
            std::thread([this, agentRaw, invokingChannelId]() {
                HandleSlashCommandCreateDm(agentRaw, invokingChannelId);
            }).detach();
        });
    discordBot_->SetSlashCommandAddAgentHandler(
        [this](const std::string& channelId, const std::string& agentRaw) {
            return HandleSlashCommandAddAgent(channelId, agentRaw);
        });
    discordBot_->SetSlashCommandRemoveAgentHandler(
        [this](const std::string& channelId, const std::string& agentRaw) {
            return HandleSlashCommandRemoveAgent(channelId, agentRaw);
        });
    discordBot_->SetSlashCommandCloseChatHandler([this](const std::string& channelId) {
        // Detached, same as add-repo's handler above — DeleteChannel is a
        // blocking REST call (see DiscordBot::DeleteChannel), no need to
        // hold the gateway thread for it since the interaction's ack
        // already happened before this callback fires.
        std::thread([this, channelId]() { HandleSlashCommandCloseChat(channelId); }).detach();
    });
    discordBot_->SetSlashCommandCreateWorkspaceHandler(
        [this](const std::string& reposRaw, const std::string& title) {
            return HandleSlashCommandCreateWorkspace(reposRaw, title);
        });

    // discordBot_->Stop() is the one mechanism used both for a real
    // shutdown and for a watchdog-triggered reconnect — `stopping` is what
    // lets the loop below tell the two apart once Run() returns.
    std::atomic<bool> stopping{false};

    std::thread discordThread([this, &stopping]() {
        while (!stopping.load()) {
            // Don't hand DPP a doomed first REST call — wait for DNS to
            // actually be usable first (see CanResolveDiscordHost's
            // comment). A fresh boot right after a crash is the main case
            // this guards, but it's cheap to check every attempt.
            bool loggedWaiting = false;
            while (!stopping.load() && !CanResolveDiscordHost()) {
                if (!loggedWaiting) {
                    log_(L"Orchestrator: network/DNS not ready yet — waiting before connecting to Discord...");
                    loggedWaiting = true;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (stopping.load()) {
                break;
            }
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
    // delivers another message again. Also covers a connection that never
    // got off the ground at all — observed after a Windows crash/reboot: the
    // service's auto-start Run() attempt hit the network before DNS/routing
    // was actually up ("getaddrinfo: No such host is known"), and DPP's own
    // retry handling for that got stuck rather than ever reaching on_ready
    // (IsZombied() explicitly excludes this case — see its comment). Poll
    // for both and force a reconnect rather than requiring a manual service
    // restart.
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
            } else if (discordBot_->IsStuckConnecting()) {
                log_(L"Orchestrator: Discord connection never came up (stuck since the last attempt) — "
                     L"forcing a reconnect.");
                discordBot_->Stop();
            }
        }
    });

    // Periodic reminder firing (~20s poll) — see FireDueReminders' comment
    // in Orchestrator.h for why this can't happen inside the MCP subprocess
    // that schedule_reminder itself runs in. Started here (not earlier,
    // alongside taskStore_/reminderStore_'s construction above) since it
    // needs a live discordBot_ to post into a chat's channel, same
    // precondition as discordThread/watchdogThread.
    std::thread reminderThread([this, &stopping]() {
        while (!stopping.load()) {
            for (int i = 0; i < 20 && !stopping.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (stopping.load()) {
                break;
            }
            FireDueReminders();
            EnsurePendingRepoOnboarding();
        }
    });

    WaitForSingleObject(shutdownEvent, INFINITE);

    stopping = true;
    log_(L"Orchestrator: shutting down Discord connection...");
    discordBot_->Stop();
    discordThread.join();
    watchdogThread.join();
    reminderThread.join();

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

std::string Orchestrator::AddRepo(const std::string& githubUrl, const std::string& notes, std::string& outError) {
    std::string org, repoName;
    if (!GitHubRepo::ParseGitHubUrl(githubUrl, org, repoName)) {
        outError = "could not parse '" + githubUrl + "' as a GitHub repo (expected "
                    "https://github.com/org/repo, git@github.com:org/repo.git, or org/repo)";
        return "";
    }
    const std::string repoId = GitHubRepo::RepoId(org, repoName);

    Repo existing;
    if (repoStore_->Get(repoId, existing)) {
        // Idempotent — adding the same repo twice is a no-op past the first
        // time (no re-clone, no re-onboard).
        return existing.id;
    }

    const std::wstring localPath = RepoLocalPath(repoId);
    if (localPath.empty()) {
        outError = "could not resolve %ProgramData% to determine the clone destination";
        return "";
    }

    Repo repo;
    repo.id = repoId;
    repo.githubUrl = githubUrl;
    repo.localPath = WideToUtf8(localPath);
    repo.status = "cloning";
    repo.notes = notes;
    repo.createdAt = static_cast<int64_t>(time(nullptr));
    repo.updatedAt = repo.createdAt;
    if (!repoStore_->Create(repo)) {
        outError = "failed to save the new repo record";
        return "";
    }

    repoStore_->TryClaimOnboarding(repoId);
    std::thread([this, repoId]() { RunRepoOnboarding(repoId); }).detach();
    return repoId;
}

bool Orchestrator::RetryRepo(const std::string& repoId, std::string& outError) {
    Repo repo;
    if (!repoStore_->Get(repoId, repo)) {
        outError = "no repo with id '" + repoId + "'";
        return false;
    }
    if (repo.status != "failed") {
        outError = "repo '" + repoId + "' is not in a failed state (currently '" + repo.status + "')";
        return false;
    }
    if (!repoStore_->ClearError(repoId, static_cast<int64_t>(time(nullptr)))) {
        outError = "failed to reset repo status";
        return false;
    }
    repoStore_->TryClaimOnboarding(repoId);
    std::thread([this, repoId]() { RunRepoOnboarding(repoId); }).detach();
    return true;
}

// Repos imported via the import_repo/create_repo MCP tools (src/orchestrator/
// RepoImport.cpp) land at status='ready' with onboarding never triggered —
// that tool runs in the isolated MCP subprocess, which can only touch the
// DB, not kick off the real onboarding-chat-with-Alex flow (that needs a
// live Orchestrator with agentStore_/chatStore_/discordBot_ all in the same
// process). This periodic sweep (called from the reminderThread poll in
// Run()) picks those up: TryClaimOnboarding's WHERE-guarded UPDATE is what
// keeps this safe to call repeatedly without ever double-onboarding the same
// repo — see its comment in RepoStore.h for why `status` alone can't be the
// gate.
void Orchestrator::EnsurePendingRepoOnboarding() {
    for (const Repo& repo : repoStore_->ListPendingOnboarding()) {
        if (!repoStore_->TryClaimOnboarding(repo.id)) {
            continue; // lost the race (shouldn't happen — single-threaded caller — but safe either way)
        }
        RunRepoOnboarding(repo.id);
    }
}

void Orchestrator::RunRepoOnboarding(const std::string& repoId) {
    Repo repo;
    if (!repoStore_->Get(repoId, repo)) {
        log_(L"Orchestrator: RunRepoOnboarding called for unknown repo id '" + AsciiToWide(repoId) + L"'.");
        return;
    }

    std::string org, repoName;
    GitHubRepo::ParseGitHubUrl(repo.githubUrl, org, repoName); // already validated by AddRepo
    const std::string repoDisplayName = org.empty() || repoName.empty() ? repo.id : (org + "/" + repoName);

    const std::wstring localPathWide = AsciiToWide(repo.localPath);
    const bool alreadyCloned = fs::exists(fs::path(localPathWide) / L".git");
    if (!alreadyCloned) {
        const std::wstring commandLine =
            L"gh.exe repo clone \"" + AsciiToWide(repoDisplayName) + L"\" \"" + localPathWide + L"\"";

        std::string output;
        int exitCode = -1;
        if (!ProcessRunner::RunCommand(commandLine, L"", output, exitCode)) {
            const std::string error = "failed to launch 'gh repo clone' (is the gh CLI installed and on PATH?)";
            repoStore_->SetError(repoId, error, static_cast<int64_t>(time(nullptr)));
            log_(L"Orchestrator: repo clone failed for '" + AsciiToWide(repoId) + L"': " + AsciiToWide(error));
            return;
        }
        if (exitCode != 0) {
            repoStore_->SetError(repoId, output, static_cast<int64_t>(time(nullptr)));
            log_(L"Orchestrator: 'gh repo clone' exited " + std::to_wstring(exitCode) + L" for '" +
                 AsciiToWide(repoId) + L"'.");
            return;
        }
    }

    repoStore_->SetStatus(repoId, "ready", static_cast<int64_t>(time(nullptr)));

    Agent alex;
    if (!agentStore_->Get("alex", alex)) {
        log_(L"Orchestrator: RunRepoOnboarding could not find Alex — aborting onboarding for '" +
             AsciiToWide(repoId) + L"'.");
        return;
    }

    const std::string onboardChatId = "repo-onboard-" + repoId;
    Chat existingChat;
    if (!chatStore_->GetChat(onboardChatId, existingChat)) {
        Chat onboardChat;
        onboardChat.id = onboardChatId;
        onboardChat.title = repo.githubUrl;
        onboardChat.createdBy = "system";
        onboardChat.status = "active";
        // discordChannelId left empty on purpose — created lazily by
        // EnsureChannelForChat the first time something is posted into it,
        // same as every other chat here.
        onboardChat.createdAt = static_cast<int64_t>(time(nullptr));
        if (!chatStore_->CreateChat(onboardChat)) {
            log_(L"Orchestrator: failed to create onboarding chat for repo '" + AsciiToWide(repoId) + L"'.");
            return;
        }
        chatStore_->AddParticipant(onboardChatId, "agent", "alex");
    }

    std::string alexTemplate;
    if (!promptTemplateStore_->Get(PromptTemplateNames::kRepoOnboardingAlex, alexTemplate)) {
        log_(L"Orchestrator: repo_onboarding_alex template missing — aborting onboarding for '" +
             AsciiToWide(repoId) + L"'.");
        return;
    }
    const std::string renderedAlexPrompt =
        RenderPromptTemplate(alexTemplate, repoDisplayName, repo.githubUrl, repo.localPath, repo.notes);

    Message alexMessage;
    alexMessage.chatId = onboardChatId;
    alexMessage.senderType = "system";
    alexMessage.senderId = "system";
    alexMessage.type = "text";
    alexMessage.content = renderedAlexPrompt;
    alexMessage.createdAt = static_cast<int64_t>(time(nullptr));
    if (chatStore_->InsertMessage(alexMessage) < 0) {
        log_(L"Orchestrator: failed to insert the repo-onboarding prompt for '" + AsciiToWide(repoId) + L"'.");
        return;
    }

    // Runs Alex's turn synchronously on this background thread — same
    // precedent as /debug/inject-message calling HandleIncomingMessage
    // directly off the HTTP handler thread. Alex may call
    // submit_agent_for_approval during this turn; nothing further to do
    // here — the actual agent creation happens once Cardon approves it (see
    // HandleReaction's repo-onboarding hook below).
    HandleIncomingMessage(onboardChatId);
}

void Orchestrator::EnsureMentionedParticipantsJoined(
    const std::string& chatId, const std::string& content, std::vector<std::string>& participantIds,
    std::vector<Agent>& participants, std::unordered_map<std::string, std::string>& participantModes) {
    if (chatId.rfind("dm-", 0) == 0) {
        return; // a DM chat is always exactly one agent — nothing to join
    }
    std::vector<Agent> allActive;
    for (const Agent& a : agentStore_->ListAll()) {
        if (a.status == "active") {
            allActive.push_back(a);
        }
    }
    for (const std::string& id : Mentions::ParseMentions(content, allActive)) {
        if (std::find(participantIds.begin(), participantIds.end(), id) != participantIds.end()) {
            continue; // already a participant
        }
        Agent agent;
        if (!agentStore_->Get(id, agent) || !chatStore_->AddParticipant(chatId, "agent", id)) {
            continue;
        }
        chatStore_->SetParticipantMode(chatId, "agent", id, ParticipantMode::kListening);
        participantIds.push_back(id);
        participants.push_back(agent);
        participantModes[id] = ParticipantMode::kListening;
        activityLog_->Log(chatId, id, "auto_joined_via_mention");
    }
}

void Orchestrator::HandleIncomingMessage(const std::string& chatId) {
    Chat chat;
    if (!chatStore_->GetChat(chatId, chat)) {
        return;
    }

    const std::vector<ParticipantAgent> participantAgents = chatStore_->ListParticipantAgents(chatId);
    std::vector<std::string> participantIds;
    // agentId -> mode ("auto_respond" | "listening") — consulted below so a
    // 'listening' participant only gets queued for a turn when explicitly
    // @-tagged, never on every message. It still has full context whenever
    // it does take a turn, since the message that triggered dispatch (and
    // everything else) was already written to `messages` before this runs —
    // "listening" only affects whether a turn gets queued, not what's in the
    // chat history a queued turn reads.
    std::unordered_map<std::string, std::string> participantModes;
    participantIds.reserve(participantAgents.size());
    for (const ParticipantAgent& pa : participantAgents) {
        participantIds.push_back(pa.agentId);
        participantModes[pa.agentId] = pa.mode;
    }

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

    // Every active 'auto_respond' agent in the chat takes every message into
    // context — dispatched a turn on every message, tagged or not (this is
    // what keeps a resumed SDK session's view of the conversation gapless).
    // 'listening' agents are the exception: they're never queued unless
    // explicitly @-tagged (see participantModes above). `tagged` itself just
    // tells a queued turn whether it's expected to reply (see the
    // addressingNote worker/src/index.ts builds into the prompt) or free to
    // stay silent — an empty response below is simply not posted, not an
    // error. The turn-limit guard is what keeps this from spiraling if
    // agents turn out chattier than expected, not addressing gating.
    struct QueueEntry {
        std::string agentId;
        bool tagged;
    };
    std::deque<QueueEntry> queue;
    auto enqueue = [&queue, &participantModes](const std::string& id, bool tagged) {
        const auto modeIt = participantModes.find(id);
        const bool listening = modeIt != participantModes.end() && modeIt->second == ParticipantMode::kListening;
        if (listening && !tagged) {
            return; // listening-mode agents only get a turn when explicitly @-tagged
        }
        for (QueueEntry& entry : queue) {
            if (entry.agentId == id) {
                entry.tagged = entry.tagged || tagged; // never downgrade an already-tagged entry
                return;
            }
        }
        queue.push_back({id, tagged});
    };

    std::string triggeringContent;
    const std::vector<Message> latest = chatStore_->RecentMessages(chatId, 1);
    if (!latest.empty()) {
        triggeringContent = latest.back().content;
    }
    // Pulls in anyone @tagged in the triggering message who isn't already a
    // participant — covers both a human typing "@newAgent" in Discord and
    // (via the mid-turn call further below) one agent tagging another.
    EnsureMentionedParticipantsJoined(chatId, triggeringContent, participantIds, participants, participantModes);
    const std::vector<std::string> taggedInTrigger = Mentions::ParseMentions(triggeringContent, participants);
    for (const std::string& id : participantIds) {
        const bool tagged = std::find(taggedInTrigger.begin(), taggedInTrigger.end(), id) != taggedInTrigger.end();
        enqueue(id, tagged);
    }

    while (!queue.empty()) {
        const QueueEntry entry = queue.front();
        queue.pop_front();
        const std::string& agentId = entry.agentId;

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

        activityLog_->Log(chatId, agent.id, "turn_start", json{{"tagged", entry.tagged}});

        // Global (not chat-scoped) watermark — a turn's messages aren't
        // confined to the chat it ran in (message_user writes into the
        // agent's own separate DM chat), so MessagesBySenderAfter below
        // needs a watermark that covers every chat.
        const int64_t maxIdBeforeTurn = chatStore_->LatestMessageId();

        std::string resumeSessionId;
        agentSessionStore_->GetIfFresh(agent.id, chatId, kSessionIdleTimeoutSeconds, resumeSessionId);

        // Fresh session (first turn ever, or a resume that failed/was
        // dropped): if there's a chat summary, bootstrap context with it —
        // a synthetic system message carrying the summary, plus the real
        // messages since its cutoff — instead of the flat last-50 window,
        // so continuity isn't lost once a chat has grown past that window.
        // No summary yet: unchanged behavior (last 50 raw messages).
        std::vector<Message> recent;
        if (resumeSessionId.empty()) {
            std::string summary;
            int64_t throughMessageId = 0;
            if (chatSummaryStore_->Get(chatId, summary, throughMessageId)) {
                Message summaryMessage; // synthetic — never persisted to the DB
                summaryMessage.chatId = chatId;
                summaryMessage.senderType = "system";
                summaryMessage.senderId = "system";
                summaryMessage.type = "text";
                summaryMessage.content = "[Summary of earlier conversation: " + summary + "]";
                summaryMessage.createdAt = static_cast<int64_t>(time(nullptr));
                recent.push_back(std::move(summaryMessage));
                for (Message& m : chatStore_->MessagesAfter(chatId, throughMessageId)) {
                    recent.push_back(std::move(m));
                }
            } else {
                recent = chatStore_->RecentMessages(chatId, 50);
            }
        } else {
            recent = chatStore_->RecentMessages(chatId, 50);
        }
        std::atomic<bool> stopTyping{false};
        std::thread typingThread = StartTypingIndicator(agent, chat.discordChannelId, &stopTyping);

        const AgentTurnResult turnResult = AgentTurn::Run(
            agent, recent, dbPath_, claudeConfigDir_, chatId, resumeSessionId, logDir_, entry.tagged);

        stopTyping.store(true);
        if (typingThread.joinable()) {
            typingThread.join();
        }

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

        // An agent that wasn't tagged is free to judge silence is the right
        // call — treat an empty response OR the SILENT sentinel as "chose
        // not to reply," not a bug: nothing gets inserted, mirrored to
        // Discord, or scanned for tags, and it doesn't count toward the
        // turn limit (CountTrailingAgentTurns only counts real inserted
        // messages). Both forms are checked — models tend to prefer writing
        // *something* (the sentinel) over truly empty output, but accepting
        // either means neither convention alone has to be perfectly reliable.
        const std::string trimmedResponse = TrimWhitespace(turnResult.response);
        if (trimmedResponse.empty() || IsSilenceSentinel(trimmedResponse)) {
            activityLog_->Log(chatId, agent.id, "turn_end", json{{"ok", true}, {"replied", false}});
            continue;
        }
        activityLog_->Log(chatId, agent.id, "turn_end", json{{"ok", true}, {"replied", true}});

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
        // gets mirrored to Discord and broadcast to the rest of this chat
        // here, on the orchestrator's own thread, in one place.
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

            // A DM chat (or a start_chat-created group chat) is created
            // without a Discord channel — create one now, the first time it
            // actually has something to post.
            std::string discordChannelId = producedChat.discordChannelId;
            if (discordChannelId.empty()) {
                discordChannelId = EnsureChannelForChat(producedChat);
            }

            if (!discordChannelId.empty() && produced.discordMessageId.empty()) {
                const std::string discordText = Mentions::ReflectMentionsForDiscord(produced.content, participants);
                const std::string discordMessageId = PostAsAgent(agent, discordChannelId, discordText);
                if (!discordMessageId.empty()) {
                    chatStore_->SetMessageDiscordId(produced.id, discordMessageId);
                }
            }

            // A DM chat has exactly one agent participant — nothing to
            // broadcast to. A message posted into some other, unrelated
            // chat (e.g. an explicit post_message chat_id, or a fresh
            // start_chat) isn't this chat's concern either — participants/
            // participantIds below are scoped to `chatId`, not that chat.
            if (produced.chatId.rfind("dm-", 0) == 0 || produced.chatId != chatId) {
                continue;
            }

            // Every other active participant of this chat takes this
            // message into context too — tagged if @mentioned (nudged to
            // reply), otherwise free to judge for itself. can_message isn't
            // consulted here: it gates explicit addressing (start_chat,
            // message_user's target), not "who's allowed to be in the
            // room" for a chat everyone here already belongs to. An agent
            // tagging a teammate who hasn't joined this chat yet pulls them
            // in automatically, same as a human doing it from Discord.
            EnsureMentionedParticipantsJoined(chatId, produced.content, participantIds, participants, participantModes);
            const std::vector<std::string> mentionedHere = Mentions::ParseMentions(produced.content, participants);
            for (const std::string& otherId : participantIds) {
                if (otherId == produced.senderId) {
                    continue; // no self-trigger
                }
                const bool taggedHere =
                    std::find(mentionedHere.begin(), mentionedHere.end(), otherId) != mentionedHere.end();
                enqueue(otherId, taggedHere);
            }
        }
    }

    // A turn above may have called submit_agent_for_approval, which only
    // writes to the DB (the MCP server is a separate subprocess with no
    // live Discord connection) — post any such drafts now that we're back
    // on the orchestrator's own thread with a real DiscordBot.
    PostPendingApprovals();

    // Same rationale as PostPendingApprovals above — a turn may have called
    // create_workspace, which (also being MCP-subprocess-only) can only do
    // the DB/filesystem half of the work.
    EnsurePendingWorkspaceChannels();

    // Same rationale again — add_agent_to_workspace also only does the DB
    // write from the MCP subprocess; this grants the joining agent's own bot
    // access to the (already-existing, by definition) workspace channel.
    SyncPendingWorkspaceAgentGrants();

    // Best-effort summary housekeeping — once per HandleIncomingMessage
    // call (not once per agent turn above), after real dispatch is done.
    RefreshChatSummaryIfNeeded(chatId);
}

void Orchestrator::RefreshChatSummaryIfNeeded(const std::string& chatId) {
    std::string existingSummary;
    int64_t throughMessageId = 0;
    chatSummaryStore_->Get(chatId, existingSummary, throughMessageId); // ok to ignore false (no summary yet)

    const int64_t latestId = chatStore_->LatestMessageId(chatId);
    if (latestId - throughMessageId <= kSummaryThreshold) {
        return;
    }

    std::vector<Message> toSummarize = chatStore_->MessagesAfter(chatId, throughMessageId);
    if (static_cast<int>(toSummarize.size()) > kSummaryInputCap) {
        // Keep only the most recent kSummaryInputCap so the summarizer call
        // itself doesn't blow its own context.
        toSummarize.erase(toSummarize.begin(), toSummarize.end() - kSummaryInputCap);
    }
    if (toSummarize.empty()) {
        return;
    }

    // A pseudo-agent, deliberately NOT persisted to AgentStore. Because
    // "chat-summarizer" has no backing AgentStore row, Tools::Call's
    // tool-permission enforcement (ctx.agentStore.Get(ctx.agentId, agent))
    // fails closed with "unknown agent" on ANY tool call this turn might
    // attempt — that's intentional and load-bearing: it sandboxes the
    // summarizer from ever posting messages, calling tools, or having any
    // side effect beyond returning summary text. Do not "fix" this by
    // seeding a real AgentStore row for this id.
    Agent summarizer;
    summarizer.id = "chat-summarizer";
    summarizer.name = "Chat Summarizer";
    summarizer.systemPrompt =
        "You summarize a multi-agent chat conversation concisely and factually, "
        "preserving key facts, decisions, and open questions. Respond with ONLY "
        "the summary text - no preamble, no meta-commentary.";

    const AgentTurnResult turnResult =
        AgentTurn::Run(summarizer, toSummarize, dbPath_, claudeConfigDir_, chatId, /*resumeSessionId=*/"", logDir_, /*tagged=*/true);
    if (!turnResult.ok) {
        log_(L"Orchestrator: chat summary refresh failed for chat '" + AsciiToWide(chatId) + L"': " +
             AsciiToWide(turnResult.error));
        return;
    }

    const std::string trimmedSummary = TrimWhitespace(turnResult.response);
    if (trimmedSummary.empty()) {
        log_(L"Orchestrator: chat summary refresh for chat '" + AsciiToWide(chatId) +
             L"' produced an empty summary — skipping update.");
        return;
    }

    if (!chatSummaryStore_->Set(chatId, trimmedSummary, latestId)) {
        log_(L"Orchestrator: failed to persist refreshed chat summary for chat '" + AsciiToWide(chatId) + L"'.");
    }
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

std::string Orchestrator::EnsureChannelForChat(const Chat& chat) {
    if (!chat.discordChannelId.empty()) {
        return chat.discordChannelId;
    }
    if (discordGuildId_.empty() || discordOwnerUserId_.empty()) {
        log_(L"Orchestrator: chat '" + AsciiToWide(chat.id) + L"' needs a private Discord channel, but "
             L"no discord_owner_user_id is configured (see config/ServerConfig.h) — dropping the "
             L"message rather than posting it somewhere unintended.");
        return "";
    }

    const bool isDm = chat.id.rfind("dm-", 0) == 0;
    std::vector<std::string> extraBotUserIds;
    std::string channelName;

    if (isDm) {
        // DM chat ids are no longer always exactly "dm-<agentId>" — a
        // recreated DM (see HandleSlashCommandCreateDm) gets a
        // "dm-<agentId>-<timestamp>" id instead, since the original id
        // belongs to the archived chat it replaced. Resolve the participant
        // agent from chat_participants instead of parsing the id.
        const std::vector<std::string> participantAgentIds = chatStore_->ListParticipantAgentIds(chat.id);
        if (participantAgentIds.empty()) {
            return "";
        }
        const std::string& agentId = participantAgentIds.front();
        Agent agent;
        if (!agentStore_->Get(agentId, agent) || agent.status != "active") {
            return "";
        }
        extraBotUserIds.push_back(agent.discordBotUserId);
        channelName = agentId + "-dm";
    } else {
        // Every active agent participant of the chat gets access — their
        // own bot's id where they have one, else the shared bot's webhook
        // (already granted unconditionally) covers them.
        for (const std::string& agentId : chatStore_->ListParticipantAgentIds(chat.id)) {
            Agent agent;
            if (agentStore_->Get(agentId, agent) && agent.status == "active") {
                extraBotUserIds.push_back(agent.discordBotUserId);
            }
        }
        channelName = chat.title.empty() ? chat.id : Slugify(chat.title);
    }

    const std::string channelId =
        discordBot_->CreateDmChannel(discordGuildId_, channelName, discordOwnerUserId_, extraBotUserIds);
    if (channelId.empty()) {
        log_(L"Orchestrator: failed to create a private Discord channel for chat '" + AsciiToWide(chat.id) + L"'.");
        return "";
    }
    chatStore_->SetChatDiscordChannel(chat.id, channelId);
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

    if (approval.kind == "add_agent_to_chat") {
        json payload;
        try {
            payload = json::parse(approval.payloadJson);
        } catch (const json::parse_error&) {
            return;
        }
        const std::string targetChatId = payload.value("chat_id", "");
        const std::string targetAgentId = payload.value("target_agent_id", "");

        Agent targetAgent;
        if (targetChatId.empty() || targetAgentId.empty() || !agentStore_->Get(targetAgentId, targetAgent)) {
            return;
        }

        std::string confirmationText;
        if (isApprove) {
            // The whole of "converting a DM to a multi-agent chat": add the
            // second agent as an ordinary chat_participants row. Chat
            // membership already lives entirely in chat_participants (see
            // ChatStore::ListParticipantAgentIds/ListParticipantAgents) —
            // nothing else in the system treats a "dm-<agentId>" chat id as
            // structurally different from any other chat id, it's purely a
            // naming convention message_user/EnsureChannelForChat use to
            // find/create the single-agent case. So a dm- chat that grows a
            // second agent participant simply becomes a normal multi-agent
            // group chat under its original (still dm-prefixed) id — no id
            // change, no migration, no special-casing needed anywhere else
            // in the dispatch/broadcast logic.
            chatStore_->AddParticipant(targetChatId, "agent", targetAgentId);
            confirmationText = "Agent '" + targetAgent.name + "' was added to the chat.";
        } else {
            confirmationText = "Request to add '" + targetAgent.name + "' to the chat was rejected.";
        }

        Message reply;
        reply.chatId = message.chatId;
        reply.senderType = "system";
        reply.senderId = "system";
        reply.type = "system_event";
        reply.content = confirmationText;
        reply.createdAt = static_cast<int64_t>(time(nullptr));
        chatStore_->InsertMessage(reply);

        Chat targetChat;
        if (chatStore_->GetChat(targetChatId, targetChat) && !targetChat.discordChannelId.empty()) {
            discordBot_->PostPlain(targetChat.discordChannelId, confirmationText);
            // The channel already exists (created before this agent joined)
            // — if the newly-added agent has its own Discord bot, it needs
            // an explicit permission overwrite to actually see a channel it
            // wasn't originally granted access to at creation time (see
            // CreateDmChannel). Shared-webhook agents need nothing extra —
            // they have no separate Discord account to grant.
            if (isApprove && !targetAgent.discordBotUserId.empty()) {
                discordBot_->GrantChannelAccess(targetChat.discordChannelId, targetAgent.discordBotUserId);
            }
        }
        return;
    }

    if (approval.kind == "temp_tool_permission") {
        json payload;
        try {
            payload = json::parse(approval.payloadJson);
        } catch (const json::parse_error&) {
            return;
        }
        const std::string targetAgentId = payload.value("agent_id", "");
        const std::string toolName = payload.value("tool_name", "");

        Agent targetAgent;
        if (targetAgentId.empty() || toolName.empty() || !agentStore_->Get(targetAgentId, targetAgent)) {
            return;
        }

        std::string confirmationText;
        if (isApprove) {
            tempPermissionStore_->Grant(targetAgentId, toolName, static_cast<int64_t>(time(nullptr)));
            confirmationText =
                "Granted '" + targetAgent.name + "' one-time use of '" + toolName + "'.";
        } else {
            confirmationText = "Request from '" + targetAgent.name + "' for one-time use of '" + toolName +
                                "' was rejected.";
        }

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
            discordBot_->PostPlain(chat.discordChannelId, confirmationText);
        }
        return;
    }

    if (approval.kind != "create_agent") {
        return; // no other approval kinds exist
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

    // Repo-onboarding hook: if this create_agent approval was requested
    // inside a "repo-onboard-<repoId>" chat (i.e. Alex proposed this agent
    // in response to RunRepoOnboarding's prompt) and Cardon just approved
    // it, this IS the repo's dedicated expert agent — grant it real
    // file/Bash tool access scoped to the repo, add it to the onboarding
    // chat, and send it the second built-in prompt telling it to explore
    // the repo and build its own memory of it. Rejection (isApprove ==
    // false) needs no special handling here: the repo just stays without an
    // agent, same as if Alex is asked again later.
    constexpr const char* kRepoOnboardPrefix = "repo-onboard-";
    if (isApprove && message.chatId.rfind(kRepoOnboardPrefix, 0) == 0) {
        const std::string repoId = message.chatId.substr(std::string(kRepoOnboardPrefix).size());
        Repo repo;
        if (repoStore_->Get(repoId, repo) && repo.agentId.empty()) {
            const int64_t now = static_cast<int64_t>(time(nullptr));
            agentStore_->SetRepoLocalPath(agent.id, repo.localPath);
            repoStore_->SetAgentId(repoId, agent.id, now);
            repoStore_->SetStatus(repoId, "onboarding", now);
            chatStore_->AddParticipant(message.chatId, "agent", agent.id);

            std::string org, repoName;
            GitHubRepo::ParseGitHubUrl(repo.githubUrl, org, repoName);
            const std::string repoDisplayName = org.empty() || repoName.empty() ? repo.id : (org + "/" + repoName);

            std::string agentTemplate;
            if (promptTemplateStore_->Get(PromptTemplateNames::kRepoOnboardingAgent, agentTemplate)) {
                const std::string rendered =
                    RenderPromptTemplate(agentTemplate, repoDisplayName, repo.githubUrl, repo.localPath, repo.notes);
                // Explicitly @-tag the new agent so the broadcast/addressing
                // dispatch nudges specifically it to respond (Alex, still a
                // participant, will just judge this isn't for it and reply
                // SILENT — the normal mechanism, no special-casing needed).
                Message onboardMessage;
                onboardMessage.chatId = message.chatId;
                onboardMessage.senderType = "system";
                onboardMessage.senderId = "system";
                onboardMessage.type = "text";
                onboardMessage.content = "@" + agent.id + " " + rendered;
                onboardMessage.createdAt = now;
                if (chatStore_->InsertMessage(onboardMessage) >= 0) {
                    HandleIncomingMessage(message.chatId);
                }
            } else {
                log_(L"Orchestrator: repo_onboarding_agent template missing — could not send the "
                     L"onboarding prompt for repo '" + AsciiToWide(repoId) + L"'.");
            }

            // Fire-and-forget past this point, same as everything else in
            // this pipeline — "active" means "the prompt was sent," not
            // "the agent's own setup turn definitely succeeded."
            repoStore_->SetStatus(repoId, "active", static_cast<int64_t>(time(nullptr)));
        }
    }
}

std::string Orchestrator::PostAsAgent(const Agent& agent, const std::string& channelId, const std::string& content) {
    if (AgentBotClient* ownBot = GetOrCreateAgentBotClient(agent)) {
        return ownBot->PostAsSelf(channelId, content);
    }
    return discordBot_->PostAsAgent(channelId, agent.id, agent.name, content);
}

std::thread Orchestrator::StartTypingIndicator(
    const Agent& agent, const std::string& channelId, std::atomic<bool>* stopFlag) {
    if (channelId.empty()) {
        return std::thread();
    }
    AgentBotClient* ownBot = GetOrCreateAgentBotClient(agent);
    if (ownBot == nullptr) {
        return std::thread(); // shared-webhook agent — no bot identity of its own to type as
    }

    ownBot->TriggerTyping(channelId); // fire one immediately, don't wait ~8s for the first sign of life
    return std::thread([ownBot, channelId, stopFlag]() {
        while (!stopFlag->load()) {
            // Sleep in short slices rather than one long sleep so Stop
            // (stopFlag->store(true)) is noticed promptly instead of up to
            // 8s late — the turn is usually done well before that.
            for (int i = 0; i < 80 && !stopFlag->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stopFlag->load()) {
                break;
            }
            ownBot->TriggerTyping(channelId);
        }
    });
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

bool Orchestrator::ResolveActiveAgentByNameOrId(const std::string& token, Agent& outAgent) {
    std::string lowerToken = token;
    for (char& c : lowerToken) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const Agent& candidate : agentStore_->ListAll()) {
        if (candidate.status != "active") {
            continue;
        }
        std::string lowerId = candidate.id;
        for (char& c : lowerId) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowerId == lowerToken) {
            outAgent = candidate;
            return true;
        }
        std::string lowerSlug = Slugify(candidate.name);
        for (char& c : lowerSlug) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowerSlug == lowerToken) {
            outAgent = candidate;
            return true;
        }
    }
    return false;
}

std::string Orchestrator::HandleSlashCommandCreateChat(const std::string& agentsRaw, const std::string& title) {
    const std::vector<std::string> tokens = SplitAgentTokens(agentsRaw);
    std::vector<Agent> resolved;
    std::vector<std::string> unresolvedTokens;
    for (const std::string& token : tokens) {
        Agent agent;
        if (ResolveActiveAgentByNameOrId(token, agent)) {
            const bool alreadyResolved =
                std::find_if(resolved.begin(), resolved.end(), [&](const Agent& a) { return a.id == agent.id; }) !=
                resolved.end();
            if (!alreadyResolved) {
                resolved.push_back(agent);
            }
        } else {
            unresolvedTokens.push_back(token);
        }
    }

    if (!unresolvedTokens.empty()) {
        std::string joined;
        for (size_t i = 0; i < unresolvedTokens.size(); ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += unresolvedTokens[i];
        }
        return "Couldn't resolve these to known, active agents: " + joined;
    }
    if (resolved.size() < 2) {
        return "/create-chat needs 2 or more agents (space or comma separated names/ids).";
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const std::string chatId = "agentchat-" + Slugify(title.empty() ? "chat" : title) + "-" + std::to_string(now);

    Chat chat;
    chat.id = chatId;
    chat.title = title;
    chat.createdBy = "user";
    chat.status = "active";
    // discordChannelId left empty on purpose — created below via
    // EnsureChannelForChat, the same lazy-creation path every other chat
    // uses, once every participant is already added (so the channel gets
    // created with everyone's permission overwrites in one shot).
    chat.createdAt = now;
    if (!chatStore_->CreateChat(chat)) {
        return "Failed to create the chat (internal error).";
    }
    for (const Agent& agent : resolved) {
        chatStore_->AddParticipant(chatId, "agent", agent.id);
    }

    const std::string channelId = EnsureChannelForChat(chat);
    if (channelId.empty()) {
        return "Chat '" + chatId +
               "' was created, but its Discord channel could not be — check discord_owner_user_id/"
               "discord_guild_id configuration.";
    }

    std::string names;
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (i > 0) {
            names += ", ";
        }
        names += resolved[i].name;
    }
    return "Created a new chat with " + names + ": <#" + channelId + ">";
}

void Orchestrator::HandleSlashCommandCreateDm(const std::string& agentRaw, const std::string& invokingChannelId) {
    Agent agent;
    if (!ResolveActiveAgentByNameOrId(agentRaw, agent)) {
        discordBot_->PostPlain(invokingChannelId, "'" + agentRaw + "' is not a known, active agent.");
        return;
    }

    // /create-dm means "start over" — a genuinely new conversation with
    // fresh context, not a reopen of whatever DM already exists (that's
    // what message_user/MessageUser's get-or-create is for). If an active
    // DM with this agent already exists, retire it first: archive the chat
    // and delete its Discord channel, exactly like /close-chat would, so
    // there's never more than one live channel per agent hanging around.
    // This runs on a detached thread (see the handler wiring in Run()) — the
    // interaction was already ack'd before this call, so there's no 3-second
    // window to worry about here.
    Chat previousDm;
    if (chatStore_->GetActiveDmChatForAgent(agent.id, previousDm)) {
        chatStore_->SetChatStatus(previousDm.id, "archived");
        if (!previousDm.discordChannelId.empty() && !discordBot_->DeleteChannel(previousDm.discordChannelId)) {
            log_(L"Orchestrator: failed to delete previous DM channel '" +
                 AsciiToWide(previousDm.discordChannelId) + L"' for chat '" + AsciiToWide(previousDm.id) +
                 L"' while recreating it — the chat is archived regardless.");
        }
    }

    // The very first DM with an agent keeps the plain "dm-<agentId>" id
    // (matches MessageUser's get-or-create default and any pre-existing
    // live data); every DM created after that needs a distinct id since
    // "dm-<agentId>" now belongs to the just-archived chat. Either way the
    // "dm-" prefix is what the rest of the system (mention-dispatch
    // skipping, EnsureChannelForChat's DM branch) actually keys off of, not
    // the exact suffix.
    const std::string dmChatId =
        previousDm.id.empty() ? "dm-" + agent.id : "dm-" + agent.id + "-" + std::to_string(time(nullptr));

    Chat dmChat;
    dmChat.id = dmChatId;
    dmChat.title = agent.id + " (DM)";
    dmChat.createdBy = "user";
    dmChat.status = "active";
    dmChat.createdAt = static_cast<int64_t>(time(nullptr));
    if (!chatStore_->CreateChat(dmChat)) {
        discordBot_->PostPlain(invokingChannelId, "Failed to create the DM chat (internal error).");
        return;
    }
    chatStore_->AddParticipant(dmChatId, "agent", agent.id);

    const std::string channelId = EnsureChannelForChat(dmChat);
    if (channelId.empty()) {
        discordBot_->PostPlain(
            invokingChannelId, "DM chat with '" + agent.name +
                                    "' was created, but its Discord channel could not be — check "
                                    "discord_owner_user_id/discord_guild_id configuration.");
        return;
    }

    // The confirmation lives in the new channel itself rather than being
    // relayed back to invokingChannelId — that's correct whether /create-dm
    // was run from some other channel (they'll see the ack reply there, plus
    // this as the new DM's opening message) or from within the very DM
    // being replaced (that channel is gone by the time this runs).
    discordBot_->PostPlain(channelId, "This is a fresh conversation with " + agent.name + ".");
}

std::string Orchestrator::HandleSlashCommandAddAgent(const std::string& channelId, const std::string& agentRaw) {
    Chat chat;
    if (!chatStore_->GetChatByDiscordChannel(channelId, chat)) {
        return "This channel isn't a RemoteCode chat.";
    }
    Agent agent;
    if (!ResolveActiveAgentByNameOrId(agentRaw, agent)) {
        return "'" + agentRaw + "' is not a known, active agent.";
    }
    if (chatStore_->IsParticipant(chat.id, "agent", agent.id)) {
        return "'" + agent.name + "' is already in this chat.";
    }

    chatStore_->AddParticipant(chat.id, "agent", agent.id);
    // The channel already existed before this agent joined — grant its own
    // bot explicit access if it has one (shared-webhook agents need nothing
    // extra; see GrantChannelAccess's own comment).
    if (!agent.discordBotUserId.empty()) {
        discordBot_->GrantChannelAccess(channelId, agent.discordBotUserId);
    }
    return "Added '" + agent.name + "' to this chat.";
}

std::string Orchestrator::HandleSlashCommandRemoveAgent(const std::string& channelId, const std::string& agentRaw) {
    Chat chat;
    if (!chatStore_->GetChatByDiscordChannel(channelId, chat)) {
        return "This channel isn't a RemoteCode chat.";
    }
    Agent agent;
    if (!ResolveActiveAgentByNameOrId(agentRaw, agent)) {
        return "'" + agentRaw + "' is not a known, active agent.";
    }
    if (!chatStore_->IsParticipant(chat.id, "agent", agent.id)) {
        return "'" + agent.name + "' isn't in this chat.";
    }

    chatStore_->RemoveParticipant(chat.id, "agent", agent.id);
    return "Removed '" + agent.name + "' from this chat (history preserved).";
}

void Orchestrator::HandleSlashCommandCloseChat(const std::string& channelId) {
    Chat chat;
    if (!chatStore_->GetChatByDiscordChannel(channelId, chat)) {
        log_(L"Orchestrator: /close-chat invoked in a channel with no matching chat (channel '" +
             AsciiToWide(channelId) + L"').");
        return;
    }

    chatStore_->SetChatStatus(chat.id, "archived");

    if (!discordBot_->DeleteChannel(channelId)) {
        log_(L"Orchestrator: failed to delete Discord channel '" + AsciiToWide(channelId) + L"' for closed chat '" +
             AsciiToWide(chat.id) +
             L"' (bot may lack Manage Channels there, or the channel was already gone) — the chat is "
             L"archived regardless.");
    }
}

std::string Orchestrator::CreateWorkspace(
    const std::vector<std::string>& repoIdsOrNames, const std::string& title,
    const std::string& requestedByAgentId, std::string& outError) {
    const WorkspaceCreator::Result result = WorkspaceCreator::Create(
        *repoStore_, *workspaceStore_, *chatStore_, repoIdsOrNames, title, requestedByAgentId);
    if (!result.ok) {
        outError = result.error;
        return "";
    }

    // The DB/filesystem half is done — attempt the Discord half right away
    // (this is the synchronous, in-process path: either Cardon's slash
    // command, or a rare direct C++ caller). If it fails here (missing
    // guild/owner config, a transient Discord API error, ...) the workspace
    // is left in "ready" status and EnsurePendingWorkspaceChannels will keep
    // retrying it on every subsequent HandleIncomingMessage call, same as
    // if it had come from the MCP tool path.
    if (!EnsureWorkspaceDiscordAssets(result.workspaceId)) {
        log_(L"Orchestrator: workspace '" + AsciiToWide(result.workspaceId) +
             L"' created, but its Discord category/channel could not be (yet) — will keep retrying.");
    }
    return result.workspaceId;
}

std::string Orchestrator::HandleSlashCommandCreateWorkspace(const std::string& reposRaw, const std::string& title) {
    // SplitAgentTokens is generic (space/comma/tab/newline splitting) despite
    // its name — reused here for the repos option rather than duplicating
    // the same handful of lines under a repo-specific name.
    const std::vector<std::string> tokens = SplitAgentTokens(reposRaw);
    if (tokens.empty()) {
        return "/create-workspace needs at least one repo (space or comma separated names/ids).";
    }

    std::string error;
    const std::string workspaceId = CreateWorkspace(tokens, title, /*requestedByAgentId=*/"", error);
    if (workspaceId.empty()) {
        return "Failed to create the workspace: " + error;
    }

    Workspace workspace;
    if (!workspaceStore_->Get(workspaceId, workspace)) {
        return "Workspace '" + workspaceId + "' was created.";
    }
    Chat chat;
    if (chatStore_->GetChat(workspace.chatId, chat) && !chat.discordChannelId.empty()) {
        return "Created workspace '" + workspaceId + "': <#" + chat.discordChannelId + ">";
    }
    return "Created workspace '" + workspaceId +
           "' — its Discord channel is still being set up, check back shortly.";
}

bool Orchestrator::EnsureWorkspaceDiscordAssets(const std::string& workspaceId) {
    Workspace workspace;
    if (!workspaceStore_->Get(workspaceId, workspace)) {
        return false;
    }
    if (discordGuildId_.empty() || discordOwnerUserId_.empty()) {
        log_(L"Orchestrator: workspace '" + AsciiToWide(workspaceId) + L"' needs a private Discord category, "
             L"but no discord_owner_user_id/discord_guild_id is configured (see config/ServerConfig.h).");
        return false;
    }

    Chat chat;
    if (!chatStore_->GetChat(workspace.chatId, chat)) {
        return false;
    }

    // Every involved repo's dedicated agent gets access, same rule
    // EnsureChannelForChat uses for an ordinary multi-agent chat.
    std::vector<std::string> extraBotUserIds;
    for (const std::string& agentId : chatStore_->ListParticipantAgentIds(workspace.chatId)) {
        Agent agent;
        if (agentStore_->Get(agentId, agent) && agent.status == "active") {
            extraBotUserIds.push_back(agent.discordBotUserId);
        }
    }

    std::string categoryId = workspace.discordCategoryId;
    if (categoryId.empty()) {
        const std::string categoryName = Slugify(workspace.title.empty() ? workspace.id : workspace.title);
        categoryId = discordBot_->CreateCategory(discordGuildId_, categoryName, discordOwnerUserId_, extraBotUserIds);
        if (categoryId.empty()) {
            log_(L"Orchestrator: failed to create a Discord category for workspace '" + AsciiToWide(workspaceId) +
                 L"'.");
            return false;
        }
        workspaceStore_->SetDiscordCategoryId(workspaceId, categoryId, static_cast<int64_t>(time(nullptr)));
    }

    if (chat.discordChannelId.empty()) {
        const std::string channelId = discordBot_->CreateDmChannel(
            discordGuildId_, "general", discordOwnerUserId_, extraBotUserIds, categoryId);
        if (channelId.empty()) {
            log_(L"Orchestrator: workspace '" + AsciiToWide(workspaceId) +
                 L"' has its category, but its initial channel could not be created.");
            return false;
        }
        chatStore_->SetChatDiscordChannel(workspace.chatId, channelId);
    }

    workspaceStore_->SetStatus(workspaceId, "active", static_cast<int64_t>(time(nullptr)));
    return true;
}

void Orchestrator::EnsurePendingWorkspaceChannels() {
    for (const Workspace& workspace : workspaceStore_->ListPendingDiscordSetup()) {
        EnsureWorkspaceDiscordAssets(workspace.id); // best-effort — logs and keeps retrying on failure
    }
}

void Orchestrator::SyncPendingWorkspaceAgentGrants() {
    for (const WorkspaceStore::PendingAgentGrant& grant : workspaceStore_->ListPendingAgentGrants()) {
        Workspace workspace;
        Agent agent;
        if (!workspaceStore_->Get(grant.workspaceId, workspace) || !agentStore_->Get(grant.agentId, agent) ||
            agent.status != "active") {
            // Nothing sensible to grant — drop the pending row rather than
            // retrying forever against a workspace/agent that no longer
            // exists or is disabled.
            workspaceStore_->ClearPendingAgentGrant(grant.workspaceId, grant.agentId);
            continue;
        }

        Chat chat;
        if (!chatStore_->GetChat(workspace.chatId, chat) || chat.discordChannelId.empty()) {
            continue; // channel doesn't exist yet — leave pending, retry next tick
        }

        // Shared-webhook agents (no own bot) need nothing extra — same rule
        // as every other "grant channel access" call site in this file.
        if (!agent.discordBotUserId.empty()) {
            discordBot_->GrantChannelAccess(chat.discordChannelId, agent.discordBotUserId);
        }
        workspaceStore_->ClearPendingAgentGrant(grant.workspaceId, grant.agentId);
    }
}

void Orchestrator::FireDueReminders() {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (const Reminder& reminder : reminderStore_->ListDue(now)) {
        Chat chat;
        if (!chatStore_->GetChat(reminder.chatId, chat)) {
            log_(L"Orchestrator: reminder '" + AsciiToWide(reminder.id) + L"' targets unknown chat '" +
                 AsciiToWide(reminder.chatId) + L"' — dropping rather than retrying forever.");
            reminderStore_->SetStatus(reminder.id, "fired");
            continue;
        }

        Agent agent;
        if (!agentStore_->Get(reminder.createdBy, agent)) {
            // Fall back to a minimal stand-in (id-as-name, no own bot) — same
            // rule PostPendingApprovals uses for a requester that no longer
            // exists — rather than dropping the reminder's message entirely.
            agent = Agent{};
            agent.id = reminder.createdBy;
            agent.name = reminder.createdBy;
        }

        Message message;
        message.chatId = reminder.chatId;
        message.senderType = "agent";
        message.senderId = reminder.createdBy;
        message.type = "text";
        message.content = reminder.message;
        message.createdAt = now;
        chatStore_->InsertMessage(message);

        if (!chat.discordChannelId.empty()) {
            PostAsAgent(agent, chat.discordChannelId, reminder.message);
        }

        reminderStore_->SetStatus(reminder.id, "fired");
    }
}

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
#include "../db/ChatSummaryStore.h"
#include "../db/Database.h"
#include "../db/PromptTemplateStore.h"
#include "../db/RepoStore.h"
#include "../db/WorkspaceStore.h"
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

    // Parses `githubUrl`, computes the deterministic repo id, and either:
    //  - if a repo with that id already exists, returns its id as-is
    //    (idempotent — adding the same repo twice does not re-clone or
    //    re-onboard it); or
    //  - inserts a new `repos` row (status "cloning") and kicks off
    //    cloning+onboarding on a detached background thread, returning the
    //    new id immediately (does NOT block for the clone to finish).
    // Returns an empty string and sets outError if `githubUrl` doesn't
    // parse. Called from both AdminServer's POST /repos and the Discord
    // `/add-repo` slash command handler.
    std::string AddRepo(const std::string& githubUrl, const std::string& notes, std::string& outError);

    // --- Chat-lifecycle slash commands (see DiscordBot::SetSlashCommandX
    // for wiring) — each is plain C++ bookkeeping (chat/participant DB
    // writes plus at most one Discord REST call), never an agent turn.
    // create-chat/create-dm/add-agent/remove-agent run synchronously and
    // return the exact text to reply to the interaction with (success
    // message with a channel mention, or a user-facing error). close-chat
    // is void/fire-and-forget instead — see its own comment.

    // Resolves 2+ agents (space/comma separated names or ids) into a brand
    // new "agentchat-..." chat (never resumed from anywhere), adds them all
    // as auto_respond participants, and lazily creates its Discord channel.
    std::string HandleSlashCommandCreateChat(const std::string& agentsRaw, const std::string& title);
    // Gets-or-creates the same "dm-<agentId>" chat message_user itself uses
    // (see mcp/Tools.cpp's MessageUser) so a DM opened this way and one an
    // agent opens via message_user are always the same chat, never
    // duplicated.
    void HandleSlashCommandCreateDm(const std::string& agentRaw, const std::string& invokingChannelId);
    // Adds one agent to the chat backing `channelId`. Grants the agent's own
    // bot explicit channel access if it has one (the channel may already
    // exist from before this agent joined).
    std::string HandleSlashCommandAddAgent(const std::string& channelId, const std::string& agentRaw);
    // Drops the chat_participants row only — chat history is untouched.
    std::string HandleSlashCommandRemoveAgent(const std::string& channelId, const std::string& agentRaw);
    // Archives the chat backing `channelId` and deletes the channel itself.
    // Fire-and-forget: the interaction's ack already happened in
    // DiscordBot::HandleSlashCommand before this runs (see
    // SlashCommandCloseChatHandler's comment) — replying into a channel this
    // call is about to delete would fail if attempted afterward, so there's
    // nothing meaningful left to return here.
    void HandleSlashCommandCloseChat(const std::string& channelId);
    // Case-insensitive match of `token` against an active agent's id or
    // Slugify(name) — the same rule Mentions::ParseMentions uses for @tags,
    // reused here so a Discord slash command's free-text "agent" option
    // behaves consistently with in-chat @mentions.
    bool ResolveActiveAgentByNameOrId(const std::string& token, Agent& outAgent);

    // --- create_workspace: the single core method both the /create-workspace
    // slash command and the create_workspace MCP tool funnel into (the MCP
    // tool runs in a separate subprocess with no live Discord connection —
    // see mcp/Tools.cpp's CreateWorkspaceTool and WorkspaceCreator.h's header
    // comment — so it calls WorkspaceCreator::Create directly instead of this
    // method, then leaves the Discord half for EnsurePendingWorkspaceChannels
    // below to pick up).

    // Resolves repos (id or name, via RepoStore), pulls in best-effort
    // heuristic dependencies, creates a git worktree per resolved repo, and
    // creates the workspace's Discord category + initial channel — all
    // synchronously (worktree creation is a fast local subprocess call, not
    // a network round-trip, so this comfortably fits inside a slash command's
    // 3-second ack window in practice). `requestedByAgentId` is empty for
    // Cardon via the slash command. Returns the new workspace's id, or an
    // empty string with `outError` set on any failure.
    std::string CreateWorkspace(
        const std::vector<std::string>& repoIdsOrNames, const std::string& title,
        const std::string& requestedByAgentId, std::string& outError);
    // /create-workspace's Discord entry point — splits `reposRaw` the same
    // way /create-chat splits its agents option, calls CreateWorkspace, and
    // returns the exact text to reply to the interaction with.
    std::string HandleSlashCommandCreateWorkspace(const std::string& reposRaw, const std::string& title);
    // Creates `workspace`'s Discord category and initial text channel
    // (parented under it), persisting whichever of the two succeed and
    // advancing workspace.status to "active" only once both exist. Safe to
    // call repeatedly on the same workspace — re-reads the row so it never
    // creates a second category once one already exists, and only attempts
    // whichever of category/channel is still missing. Returns true once both
    // exist (whether they already did, or were just created here).
    bool EnsureWorkspaceDiscordAssets(const std::string& workspaceId);
    // Polls WorkspaceStore::ListPendingDiscordSetup and calls
    // EnsureWorkspaceDiscordAssets for each — the workspace equivalent of
    // PostPendingApprovals, called from the same housekeeping spot at the
    // end of HandleIncomingMessage. Picks up workspaces the create_workspace
    // MCP tool created (DB/filesystem-only, no Discord access of its own).
    void EnsurePendingWorkspaceChannels();

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
    // Best-effort housekeeping: if `chatId` has grown by more than
    // kSummaryThreshold messages since the current chat_summaries watermark
    // (0 if none yet), runs a summarizer turn over the messages since that
    // watermark and updates the stored summary. Called at most once per
    // HandleIncomingMessage invocation, near PostPendingApprovals — never
    // once per agent turn. Logs and returns without throwing if the
    // summarizer turn fails; this must never block real dispatch.
    void RefreshChatSummaryIfNeeded(const std::string& chatId);
    void HandleReaction(const std::string& discordMessageId, const std::string& emoji);

    // Runs on the detached background thread AddRepo spawns for `repoId`:
    // clones the repo (if not already cloned), marks it "ready", creates
    // the "repo-onboard-<repoId>" chat, renders and posts the
    // "repo_onboarding_alex" template into it, then runs Alex's turn
    // synchronously (HandleIncomingMessage) so Alex can propose the new
    // repo-expert agent for approval. See the design doc section on the
    // onboarding pipeline for the full flow (this is only step one of it —
    // step two, granting tool access and onboarding the new agent itself,
    // happens in HandleReaction once Cardon approves).
    void RunRepoOnboarding(const std::string& repoId);

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

    // Ensures `chat` has a real Discord channel, creating one (and
    // persisting it via ChatStore::SetChatDiscordChannel) if it doesn't yet.
    // For a "dm-<id>" chat this behaves exactly like the old EnsureDmChannel
    // did: single agent (parsed out of the chat id) plus Cardon. For any
    // other chat, every *active* agent participant of `chat` gets access
    // (their own bot's id where they have one) plus Cardon. Returns the
    // channel id, or an empty string if it can't be created (no
    // discordOwnerUserId configured, no guild id, unknown/inactive agent for
    // a dm- chat, or the Discord API call failed).
    std::string EnsureChannelForChat(const Chat& chat);

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
    std::unique_ptr<ChatSummaryStore> chatSummaryStore_;
    std::unique_ptr<RepoStore> repoStore_;
    std::unique_ptr<WorkspaceStore> workspaceStore_;
    std::unique_ptr<PromptTemplateStore> promptTemplateStore_;
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

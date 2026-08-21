#include <algorithm>
#include <ctime>
#include <iostream>
#include <string>

#include "../src/db/AgentSessionStore.h"
#include "../src/db/AgentStore.h"
#include "../src/db/ApprovalStore.h"
#include "../src/db/ChatStore.h"
#include "../src/db/ChatSummaryStore.h"
#include "../src/db/Database.h"
#include "../src/db/PromptTemplateStore.h"
#include "../src/db/ReminderStore.h"
#include "../src/db/RepoStore.h"
#include "../src/db/Schema.h"
#include "../src/db/TaskStore.h"
#include "../src/db/TempPermissionStore.h"
#include "../src/db/WorkspaceStore.h"
#include "../src/greeting.h"
#include "../src/mcp/McpServer.h"
#include "../src/orchestrator/WorkspaceCreator.h"
#include "../src/third_party/json.hpp"
#include "../src/util/ActivityLog.h"
#include "../src/orchestrator/GitHubOps.h"
#include "../src/util/GitHubRepo.h"
#include "../src/util/Mentions.h"
#include "../src/util/Text.h"

using nlohmann::json;

namespace {

int failures = 0;

// Seeds an Agent row so Tools::Call's permission enforcement (which looks up
// ctx.agentId via AgentStore::Get) has something to check against. Tests
// that exercise tools/call must call this before constructing an McpServer.
void SeedTestAgent(AgentStore& store, const std::string& agentId, const std::vector<std::string>& tools) {
    Agent agent;
    agent.id = agentId;
    agent.name = agentId;
    agent.description = "test fixture";
    agent.systemPrompt = "test";
    agent.status = "active";
    json permissions = json::array();
    for (const std::string& tool : tools) {
        permissions.push_back(tool);
    }
    agent.toolPermissionsJson = permissions.dump();
    agent.canMessageJson = "[]";
    agent.createdBy = "user";
    agent.createdAt = 1;
    agent.updatedAt = 1;
    store.Upsert(agent);
}

void Check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << std::endl;
        ++failures;
    }
}

// Runs one Test* function, catching anything it throws (e.g. json::parse on
// a response a test forgot was an error, not the success shape it assumed)
// so a single bad test reports as a failure with the exception message
// instead of taking down the entire suite with an unhandled-exception abort
// (which on Windows shows up as a bare nonzero/negative exit code and no
// indication of which test — or whether it was a real bug at all — was the
// cause). See main()'s call sites below.
void RunTest(const std::string& name, void (*testFn)()) {
    try {
        testFn();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << name << " threw an exception: " << e.what() << std::endl;
        ++failures;
    } catch (...) {
        std::cerr << "FAIL: " << name << " threw a non-std::exception value" << std::endl;
        ++failures;
    }
}

void TestGreeting() {
    Check(greeting() == "Hello, World!", "greeting() returns the expected string");
}

void TestSchema() {
    Database db;
    Check(db.Open(L":memory:"), "Schema: in-memory database opens");
    Check(Schema::EnsureCreated(db), "Schema: EnsureCreated succeeds");
    // Calling it again against the same handle must stay a no-op, not error.
    Check(Schema::EnsureCreated(db), "Schema: EnsureCreated is idempotent");

    Statement stmt(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='agents';");
    Check(stmt.Valid() && stmt.Step(), "Schema: agents table exists");
}

// Regression test for a real bug (see Schema.cpp's DELETE for the full
// story): DiscordBot::HandleMessageCreate used to add every active agent
// (plus a 'user' row) to whatever chat a human's Discord message landed in,
// including chats — like DMs — that already had a deliberately curated
// participant list. Confirms the cleanup migration in Schema::EnsureCreated
// strips exactly the bogus rows from a corrupted DM chat (once), leaves
// that chat's own rightful agent alone, and doesn't touch a non-DM chat's
// participants at all.
void TestDmParticipantCleanupMigration() {
    Database db;
    db.Open(L":memory:");
    // First-ever EnsureCreated on a fresh db: nothing to clean up yet, but
    // this is also the call that plants the schema_meta marker — reproduce
    // "an old, pre-marker database that still has the historical bug's
    // corrupted rows on disk" by deleting the marker back out before
    // seeding the corrupted data below, since a brand new :memory: db can
    // never actually be in that state on its own.
    Schema::EnsureCreated(db);
    Check(
        db.Exec("DELETE FROM schema_meta WHERE key = 'dm_participant_cleanup_v1';"),
        "Schema: test setup can clear the cleanup marker");
    ChatStore chatStore(db);

    Chat corruptedDm;
    corruptedDm.id = "dm-tyrell-12345";
    corruptedDm.createdBy = "user";
    corruptedDm.status = "active";
    corruptedDm.createdAt = 1;
    chatStore.CreateChat(corruptedDm);
    chatStore.AddParticipant("dm-tyrell-12345", "agent", "tyrell"); // rightful — must survive
    chatStore.AddParticipant("dm-tyrell-12345", "agent", "alex");   // bogus — must be removed
    chatStore.AddParticipant("dm-tyrell-12345", "agent", "wren");   // bogus — must be removed
    chatStore.AddParticipant("dm-tyrell-12345", "user", "439463673333940225"); // bogus — must be removed

    Chat groupChat;
    groupChat.id = "chat-abc";
    groupChat.createdBy = "user";
    groupChat.status = "active";
    groupChat.createdAt = 1;
    chatStore.CreateChat(groupChat);
    chatStore.AddParticipant("chat-abc", "agent", "alex");
    chatStore.AddParticipant("chat-abc", "agent", "tyrell");
    chatStore.AddParticipant("chat-abc", "user", "439463673333940225");

    // Re-running EnsureCreated (already proven idempotent in TestSchema) is
    // what applies the cleanup migration against these freshly-seeded rows,
    // since the marker was cleared above.
    Check(Schema::EnsureCreated(db), "Schema: EnsureCreated re-run applies the DM cleanup migration");

    const std::vector<std::string> dmAgents = chatStore.ListParticipantAgentIds("dm-tyrell-12345");
    Check(
        dmAgents.size() == 1 && dmAgents[0] == "tyrell",
        "Schema: DM cleanup strips every non-matching agent, keeps the rightful one");
    Check(
        !chatStore.IsParticipant("dm-tyrell-12345", "user", "439463673333940225"),
        "Schema: DM cleanup strips the bogus 'user' participant row");

    const std::vector<std::string> groupAgents = chatStore.ListParticipantAgentIds("chat-abc");
    Check(
        groupAgents.size() == 2 &&
            std::find(groupAgents.begin(), groupAgents.end(), "alex") != groupAgents.end() &&
            std::find(groupAgents.begin(), groupAgents.end(), "tyrell") != groupAgents.end(),
        "Schema: DM cleanup leaves a non-DM chat's participants untouched");
    Check(
        chatStore.IsParticipant("chat-abc", "user", "439463673333940225"),
        "Schema: DM cleanup leaves a non-DM chat's 'user' participant untouched");

    // The marker is now set (this run consumed it) — a legitimately added
    // second DM agent (Orchestrator::HandleReaction's "add_agent_to_chat"
    // approval flow) must survive every subsequent EnsureCreated call
    // (i.e. every later agent turn/server restart), not just get treated
    // as more of the same "bogus" shape the migration used to strip.
    chatStore.AddParticipant("dm-tyrell-12345", "agent", "sable");
    Check(Schema::EnsureCreated(db), "Schema: EnsureCreated runs again after the marker is set");
    Check(Schema::EnsureCreated(db), "Schema: EnsureCreated runs a third time after the marker is set");
    const std::vector<std::string> dmAgentsAfter = chatStore.ListParticipantAgentIds("dm-tyrell-12345");
    Check(
        dmAgentsAfter.size() == 2 &&
            std::find(dmAgentsAfter.begin(), dmAgentsAfter.end(), "tyrell") != dmAgentsAfter.end() &&
            std::find(dmAgentsAfter.begin(), dmAgentsAfter.end(), "sable") != dmAgentsAfter.end(),
        "Schema: a legitimately added second DM agent survives later EnsureCreated calls "
        "once the cleanup marker is set");
}

void TestAgentStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    AgentStore store(db);

    Check(store.IsEmpty(), "AgentStore: starts empty");
    Check(store.SeedAlexIfEmpty(), "AgentStore: seeds Alex");
    Check(!store.IsEmpty(), "AgentStore: no longer empty after seeding");

    Agent alex;
    Check(store.Get("alex", alex), "AgentStore: Get('alex') found");
    Check(alex.name == "Alex", "AgentStore: seeded Alex has expected name");
    Check(!alex.systemPrompt.empty(), "AgentStore: seeded Alex has a system prompt");

    // Seeding again once the table is non-empty must not duplicate/overwrite.
    Check(store.SeedAlexIfEmpty(), "AgentStore: SeedAlexIfEmpty is a safe no-op when non-empty");

    Agent custom;
    custom.id = "test-agent";
    custom.name = "Test Agent";
    custom.description = "unit test fixture";
    custom.systemPrompt = "You are a test.";
    custom.status = "active";
    custom.toolPermissionsJson = "[]";
    custom.canMessageJson = "[]";
    custom.createdBy = "user";
    custom.createdAt = 1;
    custom.updatedAt = 1;
    Check(store.Upsert(custom), "AgentStore: Upsert inserts a new agent");

    Agent roundTripped;
    Check(store.Get("test-agent", roundTripped), "AgentStore: Get round-trips a custom agent");
    Check(roundTripped.description == "unit test fixture", "AgentStore: round-tripped fields match");

    Check(store.SetFact("test-agent", "favorite_color", "blue"), "AgentStore: SetFact succeeds");
    std::string factValue;
    Check(
        store.GetFact("test-agent", "favorite_color", factValue) && factValue == "blue",
        "AgentStore: GetFact round-trips");
}

void TestAgentStoreBotToken() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    AgentStore store(db);

    Agent agent;
    agent.id = "bot-agent";
    agent.name = "Bot Agent";
    agent.description = "fixture";
    agent.systemPrompt = "test";
    agent.status = "active";
    agent.toolPermissionsJson = "[]";
    agent.canMessageJson = "[]";
    agent.createdBy = "user";
    agent.createdAt = 1;
    agent.updatedAt = 1;
    Check(store.Upsert(agent), "AgentStore: Upsert inserts the fixture agent");

    Agent fresh;
    Check(
        store.Get("bot-agent", fresh) && fresh.discordBotTokenEncrypted.empty(),
        "AgentStore: a new agent has no own bot by default");

    Check(
        store.SetDiscordBotToken("bot-agent", "fake-token-value", "111", "BotAgentBot"),
        "AgentStore: SetDiscordBotToken succeeds");

    Agent withBot;
    Check(store.Get("bot-agent", withBot), "AgentStore: Get after SetDiscordBotToken succeeds");
    Check(
        !withBot.discordBotTokenEncrypted.empty() && withBot.discordBotTokenEncrypted != "fake-token-value",
        "AgentStore: the stored token is encrypted, not plaintext");
    Check(
        withBot.discordBotUserId == "111" && withBot.discordBotUsername == "BotAgentBot",
        "AgentStore: bot identity fields round-trip");

    // Upsert (used for unrelated field edits, e.g. update_agent) must never
    // touch the bot-token fields.
    withBot.description = "changed via an unrelated edit";
    Check(store.Upsert(withBot), "AgentStore: Upsert on an unrelated field succeeds");
    Agent afterUnrelatedEdit;
    store.Get("bot-agent", afterUnrelatedEdit);
    Check(
        afterUnrelatedEdit.discordBotUsername == "BotAgentBot",
        "AgentStore: Upsert does not wipe an assigned bot token");

    Check(store.ClearDiscordBotToken("bot-agent"), "AgentStore: ClearDiscordBotToken succeeds");
    Agent cleared;
    store.Get("bot-agent", cleared);
    Check(
        cleared.discordBotTokenEncrypted.empty() && cleared.discordBotUserId.empty() &&
            cleared.discordBotUsername.empty(),
        "AgentStore: ClearDiscordBotToken removes all three fields");
}

void TestAgentSessionStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    AgentSessionStore store(db);

    std::string sessionId;
    Check(
        !store.Get("alex", "chat-1", sessionId),
        "AgentSessionStore: Get returns false when no session is stored yet");

    Check(store.Set("alex", "chat-1", "session-abc"), "AgentSessionStore: Set succeeds");
    Check(
        store.Get("alex", "chat-1", sessionId) && sessionId == "session-abc",
        "AgentSessionStore: Get round-trips what Set stored");

    // Same agent, different chat — must not collide.
    std::string otherChatSession;
    Check(
        !store.Get("alex", "chat-2", otherChatSession),
        "AgentSessionStore: sessions are scoped per (agent, chat), not just agent");

    Check(store.Set("alex", "chat-1", "session-xyz"), "AgentSessionStore: Set again (upsert) succeeds");
    store.Get("alex", "chat-1", sessionId);
    Check(sessionId == "session-xyz", "AgentSessionStore: a second Set overwrites the stored session id");

    Check(store.Clear("alex", "chat-1"), "AgentSessionStore: Clear succeeds");
    Check(
        !store.Get("alex", "chat-1", sessionId),
        "AgentSessionStore: a cleared session no longer round-trips");
}

void TestAgentSessionStoreGetIfFresh() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    AgentSessionStore store(db);

    std::string sessionId;
    Check(
        !store.GetIfFresh("alex", "chat-1", 3600, sessionId),
        "AgentSessionStore::GetIfFresh: returns false when no session is stored at all");

    // A session just set (last_used_at == now) is well within any reasonable
    // maxAgeSeconds window.
    Check(store.Set("alex", "chat-1", "session-fresh"), "AgentSessionStore::GetIfFresh: Set succeeds");
    Check(
        store.GetIfFresh("alex", "chat-1", 3600, sessionId) && sessionId == "session-fresh",
        "AgentSessionStore::GetIfFresh: a freshly-set session round-trips within the timeout");

    // Insert a row directly with a last_used_at 2 hours in the past, so a
    // 1-hour maxAgeSeconds treats it as stale.
    {
        Statement stmt(
            db,
            "INSERT INTO agent_chat_sessions (agent_id, chat_id, sdk_session_id, last_used_at) "
            "VALUES (?1,?2,?3,?4) ON CONFLICT(agent_id, chat_id) DO UPDATE SET "
            "sdk_session_id=excluded.sdk_session_id, last_used_at=excluded.last_used_at;");
        stmt.BindText(1, "alex");
        stmt.BindText(2, "chat-stale");
        stmt.BindText(3, "session-old");
        stmt.BindInt64(4, static_cast<int64_t>(time(nullptr)) - 7200);
        stmt.Step();
        Check(stmt.Ok(), "AgentSessionStore::GetIfFresh: fixture insert of a 2-hour-old session succeeds");
    }

    std::string staleSessionId;
    Check(
        !store.GetIfFresh("alex", "chat-stale", 3600, staleSessionId),
        "AgentSessionStore::GetIfFresh: a session older than maxAgeSeconds is treated as absent");
    Check(
        staleSessionId.empty(),
        "AgentSessionStore::GetIfFresh: outSdkSessionId is left untouched when the session is stale");

    // The same row is still fine under a wider window.
    std::string wideWindowSessionId;
    Check(
        store.GetIfFresh("alex", "chat-stale", 10800, wideWindowSessionId) &&
            wideWindowSessionId == "session-old",
        "AgentSessionStore::GetIfFresh: a 2-hour-old session is fresh under a 3-hour window");

    // Get() itself remains unconditional — it should still return the stale
    // row regardless of age.
    std::string unconditional;
    Check(
        store.Get("alex", "chat-stale", unconditional) && unconditional == "session-old",
        "AgentSessionStore::GetIfFresh: Get() is unaffected and still returns stale sessions");
}

void TestChatSummaryStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatSummaryStore store(db);

    std::string summary;
    int64_t throughMessageId = -1;
    Check(
        !store.Get("chat-1", summary, throughMessageId),
        "ChatSummaryStore: Get returns false when nothing is stored yet");

    Check(store.Set("chat-1", "Alice and Bob discussed the roadmap.", 42), "ChatSummaryStore: Set succeeds");
    Check(
        store.Get("chat-1", summary, throughMessageId) &&
            summary == "Alice and Bob discussed the roadmap." && throughMessageId == 42,
        "ChatSummaryStore: Get round-trips what Set stored");

    // Different chat must not collide.
    std::string otherSummary;
    int64_t otherThrough = -1;
    Check(
        !store.Get("chat-2", otherSummary, otherThrough),
        "ChatSummaryStore: summaries are scoped per chat");

    Check(
        store.Set("chat-1", "Updated summary through message 99.", 99),
        "ChatSummaryStore: Set again (upsert) succeeds");
    store.Get("chat-1", summary, throughMessageId);
    Check(
        summary == "Updated summary through message 99." && throughMessageId == 99,
        "ChatSummaryStore: a second Set overwrites the stored summary and watermark");
}

void TestChatStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore store(db);

    Chat chat;
    chat.id = "chat-1";
    chat.title = "";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1234567890";
    chat.createdAt = 1;
    Check(store.CreateChat(chat), "ChatStore: CreateChat succeeds");

    Chat fetched;
    Check(store.GetChatByDiscordChannel("1234567890", fetched), "ChatStore: GetChatByDiscordChannel finds it");
    Check(fetched.id == "chat-1", "ChatStore: fetched chat id matches");

    Check(store.AddParticipant("chat-1", "agent", "alex"), "ChatStore: AddParticipant (agent) succeeds");
    Check(store.AddParticipant("chat-1", "user", "user-1"), "ChatStore: AddParticipant (user) succeeds");
    Check(store.IsParticipant("chat-1", "agent", "alex"), "ChatStore: IsParticipant true for participant");
    Check(!store.IsParticipant("chat-1", "agent", "nobody"), "ChatStore: IsParticipant false for non-participant");

    const std::vector<std::string> agentIds = store.ListParticipantAgentIds("chat-1");
    Check(agentIds.size() == 1 && agentIds[0] == "alex", "ChatStore: ListParticipantAgentIds returns just alex");

    const std::vector<Chat> alexChats = store.ListChatsForParticipant("alex");
    Check(
        alexChats.size() == 1 && alexChats[0].id == "chat-1",
        "ChatStore: ListChatsForParticipant returns only chats alex participates in");
    Check(
        store.ListChatsForParticipant("nobody").empty(),
        "ChatStore: ListChatsForParticipant is empty for a non-participant");

    std::vector<int64_t> insertedIds;
    for (int i = 0; i < 3; ++i) {
        Message m;
        m.chatId = "chat-1";
        m.senderType = "user";
        m.senderId = "user-1";
        m.type = "text";
        m.content = "message " + std::to_string(i);
        m.createdAt = i;
        const int64_t id = store.InsertMessage(m);
        Check(id >= 0, "ChatStore: InsertMessage succeeds for message " + std::to_string(i));
        insertedIds.push_back(id);
    }

    const std::vector<Message> recent = store.RecentMessages("chat-1", 10);
    Check(recent.size() == 3, "ChatStore: RecentMessages returns all 3 messages");
    Check(
        recent.size() == 3 && recent[0].content == "message 0" && recent[2].content == "message 2",
        "ChatStore: RecentMessages is in chronological (oldest-first) order");

    Check(
        store.LatestMessageId("chat-1") == insertedIds.back(),
        "ChatStore: LatestMessageId returns the highest message id");
    Check(store.LatestMessageId("no-such-chat") == 0, "ChatStore: LatestMessageId is 0 for an empty/unknown chat");

    const std::vector<Message> after = store.MessagesAfter("chat-1", insertedIds[0]);
    Check(after.size() == 2, "ChatStore: MessagesAfter excludes the watermark id and everything before it");
    Check(
        after.size() == 2 && after[0].content == "message 1" && after[1].content == "message 2",
        "ChatStore: MessagesAfter is in chronological order");
    Check(
        store.MessagesAfter("chat-1", insertedIds.back()).empty(),
        "ChatStore: MessagesAfter is empty once the watermark is the latest id");

    Check(store.CreateChat(Chat{"chat-2", "", "user", "active", "", 1}), "ChatStore: CreateChat for a second chat");
    Message crossChatMessage;
    crossChatMessage.chatId = "chat-2";
    crossChatMessage.senderType = "agent";
    crossChatMessage.senderId = "alex";
    crossChatMessage.type = "text";
    crossChatMessage.content = "from chat-2";
    crossChatMessage.createdAt = 10;
    Check(store.InsertMessage(crossChatMessage) >= 0, "ChatStore: InsertMessage into chat-2 succeeds");

    Check(
        store.LatestMessageId() >= insertedIds.back(),
        "ChatStore: the no-arg LatestMessageId() is a global (cross-chat) watermark");

    const std::vector<Message> bySender = store.MessagesBySenderAfter("alex", 0);
    Check(
        bySender.size() == 1 && bySender[0].chatId == "chat-2" && bySender[0].content == "from chat-2",
        "ChatStore: MessagesBySenderAfter finds a message in a different chat than the watermark's origin");
    Check(
        store.MessagesBySenderAfter("nobody", 0).empty(),
        "ChatStore: MessagesBySenderAfter only returns that sender's messages");

    Check(
        store.SetChatDiscordChannel("chat-2", "9999"), "ChatStore: SetChatDiscordChannel succeeds");
    Chat updatedChat2;
    Check(
        store.GetChat("chat-2", updatedChat2) && updatedChat2.discordChannelId == "9999",
        "ChatStore: SetChatDiscordChannel persists the new channel id");

    Check(store.SetWebhook("chat-1", "alex", "wh-id", "wh-token"), "ChatStore: SetWebhook succeeds");
    std::string webhookId, webhookToken;
    Check(
        store.GetWebhook("chat-1", "alex", webhookId, webhookToken) && webhookId == "wh-id" &&
            webhookToken == "wh-token",
        "ChatStore: GetWebhook round-trips");

    int turnLimit = -1;
    Check(
        !store.GetTurnLimit("chat-1", turnLimit),
        "ChatStore: GetTurnLimit returns false when no override has been set");
    Check(store.SetTurnLimit("chat-1", 0), "ChatStore: SetTurnLimit(0) succeeds");
    Check(
        store.GetTurnLimit("chat-1", turnLimit) && turnLimit == 0,
        "ChatStore: GetTurnLimit round-trips a 0 (no-limit) override");
    Check(store.SetTurnLimit("chat-1", 20), "ChatStore: SetTurnLimit(20) succeeds");
    Check(
        store.GetTurnLimit("chat-1", turnLimit) && turnLimit == 20,
        "ChatStore: GetTurnLimit round-trips a positive override, replacing the previous one");
    Check(
        !store.GetTurnLimit("chat-2", turnLimit),
        "ChatStore: a different chat with no override set still returns false");

    std::string metaValue;
    Check(
        !store.GetSchemaMetaValue("agent_chats_category_id", metaValue),
        "ChatStore: GetSchemaMetaValue returns false for a key that was never set");
    Check(
        store.SetSchemaMetaValue("agent_chats_category_id", "111"),
        "ChatStore: SetSchemaMetaValue succeeds");
    Check(
        store.GetSchemaMetaValue("agent_chats_category_id", metaValue) && metaValue == "111",
        "ChatStore: GetSchemaMetaValue round-trips the value just set");
    Check(
        store.SetSchemaMetaValue("agent_chats_category_id", "222"),
        "ChatStore: SetSchemaMetaValue on an existing key succeeds (upsert)");
    Check(
        store.GetSchemaMetaValue("agent_chats_category_id", metaValue) && metaValue == "222",
        "ChatStore: a second SetSchemaMetaValue call replaces the previous value, not duplicates it");
}

void TestMcpServer() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);
    SeedTestAgent(agentStore, "alex", {"post_message", "read_chat"});

    ActivityLog activityLog(L"test-logs", "test-mcp");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    // Notification (no "id") must produce no response.
    Check(
        server.HandleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),
        "McpServer: notification produces no response");

    const json initResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    Check(initResponse["id"] == 1, "McpServer: initialize echoes request id");
    Check(initResponse["result"].contains("protocolVersion"), "McpServer: initialize returns a protocolVersion");

    const json listResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    const json& tools = listResponse["result"]["tools"];
    Check(tools.is_array() && tools.size() == 34, "McpServer: tools/list returns exactly 34 tools");

    // post_message with no chat_id must default to the server's scoped chat.
    const std::string postCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"content", "hi there"}}}}},
    }.dump();
    const json postResponse = json::parse(server.HandleLine(postCall));
    Check(postResponse["result"]["isError"] == false, "McpServer: post_message call succeeds");

    const std::vector<Message> stored = chatStore.RecentMessages("chat-1", 10);
    Check(
        stored.size() == 1 && stored[0].content == "hi there" && stored[0].senderId == "alex",
        "McpServer: post_message actually wrote the message as the scoped agent, defaulting chat_id");

    const std::string readCall = json{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", {{"name", "read_chat"}, {"arguments", json::object()}}},
    }.dump();
    const json readResponse = json::parse(server.HandleLine(readCall));
    const json readResult = json::parse(readResponse["result"]["content"][0]["text"].get<std::string>());
    Check(
        readResult["messages"].size() == 1 && readResult["messages"][0]["content"] == "hi there",
        "McpServer: read_chat (defaulting chat_id) returns the message post_message wrote");

    const std::string unknownToolCall = json{
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tools/call"},
        {"params", {{"name", "not_a_real_tool"}, {"arguments", json::object()}}},
    }.dump();
    const json unknownToolResponse = json::parse(server.HandleLine(unknownToolCall));
    Check(unknownToolResponse["result"]["isError"] == true, "McpServer: unknown tool name reports isError");

    const json unknownMethodResponse =
        json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":6,"method":"not/a_method"})"));
    Check(unknownMethodResponse.contains("error"), "McpServer: unknown method returns a JSON-RPC error");

    const json malformed = json::parse(server.HandleLine("not json at all"));
    Check(malformed.contains("error") && malformed["error"]["code"] == -32700, "McpServer: malformed input is a parse error");
}

// Regression test for a real bug: post_message/read_chat used to accept any
// caller-supplied chat_id with no check that the calling agent was actually
// a participant of it — any agent could read or write into any other
// agent's chat (DM or not) just by passing its chat_id, which is trivially
// guessable for DMs ("dm-<agentId>"). Confirms an agent with no
// chat_participants row in a target chat is rejected by both tools when it
// passes that chat_id explicitly, and that the same agent's own chat (the
// default, no explicit chat_id) still works normally.
void TestChatParticipationEnforcement() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat ownChat;
    ownChat.id = "dm-alex";
    ownChat.createdBy = "user";
    ownChat.status = "active";
    ownChat.createdAt = 1;
    chatStore.CreateChat(ownChat);
    chatStore.AddParticipant("dm-alex", "agent", "alex");

    Chat otherChat;
    otherChat.id = "dm-tyrell";
    otherChat.createdBy = "user";
    otherChat.status = "active";
    otherChat.createdAt = 1;
    chatStore.CreateChat(otherChat);
    chatStore.AddParticipant("dm-tyrell", "agent", "tyrell");
    Message seeded;
    seeded.chatId = "dm-tyrell";
    seeded.senderType = "user";
    seeded.senderId = "user";
    seeded.type = "text";
    seeded.content = "private to tyrell";
    seeded.createdAt = 1;
    chatStore.InsertMessage(seeded);

    SeedTestAgent(agentStore, "alex", {"post_message", "read_chat"});

    ActivityLog activityLog(L"test-logs", "test-mcp-participation");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "alex", "dm-alex");

    const json readOther = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "read_chat"}, {"arguments", {{"chat_id", "dm-tyrell"}}}}},
    }.dump()));
    Check(
        readOther["result"]["isError"] == true,
        "read_chat denies an agent with no chat_participants row in the target chat");
    Check(
        chatStore.RecentMessages("dm-tyrell", 10).size() == 1,
        "read_chat did not leak dm-tyrell's message content");

    const json postOther = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"chat_id", "dm-tyrell"}, {"content", "sneaking in"}}}}},
    }.dump()));
    Check(
        postOther["result"]["isError"] == true,
        "post_message denies an agent with no chat_participants row in the target chat");
    Check(
        chatStore.RecentMessages("dm-tyrell", 10).size() == 1,
        "post_message did not actually write into dm-tyrell");

    // Own chat, no explicit chat_id, still works.
    const json postOwn = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"content", "hi from alex"}}}}},
    }.dump()));
    Check(postOwn["result"]["isError"] == false, "post_message still succeeds for the agent's own default chat");
}

void TestApprovalWorkflowTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);
    SeedTestAgent(agentStore, "alex", {"submit_agent_for_approval"});

    ActivityLog activityLog(L"test-logs", "test-approval");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string submitCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "submit_agent_for_approval"},
          {"arguments",
           {{"name", "Research Bot"},
            {"description", "Looks things up."},
            {"system_prompt", "You are Research Bot."}}}}},
    }.dump();
    const json submitResponse = json::parse(server.HandleLine(submitCall));
    Check(submitResponse["result"]["isError"] == false, "submit_agent_for_approval succeeds");
    const json submitResult = json::parse(submitResponse["result"]["content"][0]["text"].get<std::string>());
    Check(
        submitResult.value("agent_id", "") == "research-bot",
        "submit_agent_for_approval slugifies the name into an id");

    Agent draft;
    Check(
        agentStore.Get("research-bot", draft) && draft.status == "pending_approval",
        "submit_agent_for_approval writes the agent row as pending_approval");

    const std::vector<Approval> unposted = approvalStore.ListUnposted();
    Check(unposted.size() == 1, "submit_agent_for_approval leaves exactly one unposted approval");
    Check(
        !unposted.empty() && unposted[0].kind == "create_agent" && unposted[0].status == "pending",
        "the unposted approval is a pending create_agent request");

    // Resolving it (simulating what Orchestrator::HandleReaction does after
    // a Discord reaction) must remove it from the unposted/pending set.
    Check(
        approvalStore.Resolve(unposted[0].id, "approved", 2), "ApprovalStore: Resolve succeeds");
    Check(
        approvalStore.ListUnposted().empty(),
        "a resolved approval no longer shows up as pending/unposted");

    // Submitting the exact same name again must be rejected, not silently
    // overwrite the (now-resolved) existing agent.
    const json duplicateResponse = json::parse(server.HandleLine(submitCall));
    Check(
        duplicateResponse["result"]["isError"] == true,
        "submit_agent_for_approval rejects a duplicate agent id");
}

void TestMessageUserTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);
    SeedTestAgent(agentStore, "alex", {"message_user"});

    ActivityLog activityLog(L"test-logs", "test-message-user");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string messageUserCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "message_user"}, {"arguments", {{"content", "private note"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(messageUserCall));
    Check(response["result"]["isError"] == false, "message_user call succeeds");

    Chat dmChat;
    Check(chatStore.GetChat("dm-alex", dmChat), "message_user creates a dm-<agentId> chat");
    Check(
        dmChat.discordChannelId.empty(),
        "message_user leaves discordChannelId empty — Orchestrator creates it lazily");

    const std::vector<Message> dmMessages = chatStore.RecentMessages("dm-alex", 10);
    Check(
        dmMessages.size() == 1 && dmMessages[0].content == "private note" && dmMessages[0].senderId == "alex",
        "message_user writes into the dm chat, not the current group chat");
    Check(
        chatStore.RecentMessages("chat-1", 10).empty(),
        "message_user does not write anything into the current (group) chat");

    // A second call must reuse the same dm chat, not fail or duplicate it.
    const json secondResponse = json::parse(server.HandleLine(messageUserCall));
    Check(secondResponse["result"]["isError"] == false, "a second message_user call also succeeds");
    Check(
        chatStore.RecentMessages("dm-alex", 10).size() == 2,
        "a second message_user call reuses the existing dm chat (get-or-create is idempotent)");
}

void TestPostMessageAttachments() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);
    SeedTestAgent(agentStore, "tyrell", {"post_message"});

    ActivityLog activityLog(L"test-logs", "test-attachments");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "tyrell", "chat-1");

    // A valid attachment round-trips into the stored message's metadata.
    const std::string goodCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "post_message"},
          {"arguments",
           {{"content", "here's the write-up"},
            {"attachments", json::array({{{"filename", "writeup.md"}, {"content", "# Title\nBody"}}})}}}}},
    }.dump();
    const json goodResponse = json::parse(server.HandleLine(goodCall));
    Check(goodResponse["result"]["isError"] == false, "post_message with a valid attachment succeeds");

    const std::vector<Message> stored = chatStore.RecentMessages("chat-1", 10);
    Check(stored.size() == 1, "post_message with attachments still writes exactly one message");
    const json metadata = json::parse(stored[0].metadataJson);
    Check(
        metadata["attachments"].size() == 1 && metadata["attachments"][0]["filename"] == "writeup.md" &&
            metadata["attachments"][0]["content"] == "# Title\nBody",
        "the attachment's filename/content round-trip through Message::metadataJson");

    // A path separator in the filename is rejected outright — that field
    // becomes the literal filename dpp::message::add_file uploads under.
    const std::string badFilenameCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "post_message"},
          {"arguments",
           {{"content", "bad"},
            {"attachments", json::array({{{"filename", "../escape.md"}, {"content", "x"}}})}}}}},
    }.dump();
    const json badFilenameResponse = json::parse(server.HandleLine(badFilenameCall));
    Check(
        badFilenameResponse["result"]["isError"] == true,
        "post_message rejects an attachment filename containing a path separator");
    Check(
        chatStore.RecentMessages("chat-1", 10).size() == 1,
        "the rejected-filename call did not write a second message");

    // More than the 10-file-per-message cap is rejected.
    json manyAttachments = json::array();
    for (int i = 0; i < 11; ++i) {
        manyAttachments.push_back(json{{"filename", "f" + std::to_string(i) + ".txt"}, {"content", "x"}});
    }
    const std::string tooManyCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"content", "too many"}, {"attachments", manyAttachments}}}}},
    }.dump();
    const json tooManyResponse = json::parse(server.HandleLine(tooManyCall));
    Check(tooManyResponse["result"]["isError"] == true, "post_message rejects more than 10 attachments");
}

// Regression test for a real bug: once an agent's entire DM history ends up
// archived (e.g. via /close-chat, with no active DM left to detect), both
// message_user's get-or-create and /create-dm used to unconditionally
// regenerate the bare "dm-<agentId>" id — which, past the agent's very
// first-ever DM, already belongs to an old archived chat — and
// ChatStore::CreateChat's INSERT collided on that primary key, surfacing as
// "Failed to create the DM chat (internal error)." Confirms message_user
// picks a fresh, non-colliding id instead when the bare one is already
// taken by an archived chat.
void TestMessageUserToolPicksFreshIdWhenBareOneIsArchived() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    // Simulate an agent whose one-and-only DM was closed a while back —
    // no active DM exists, but the bare id is already taken.
    Chat archivedDm;
    archivedDm.id = "dm-alex";
    archivedDm.title = "alex (DM)";
    archivedDm.createdBy = "alex";
    archivedDm.status = "archived";
    archivedDm.createdAt = 1;
    chatStore.CreateChat(archivedDm);
    chatStore.AddParticipant("dm-alex", "agent", "alex");

    SeedTestAgent(agentStore, "alex", {"message_user"});
    ActivityLog activityLog(L"test-logs", "test-message-user-collision");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore,
        taskStore, reminderStore, activityLog, "alex", "chat-1");

    const json response = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "message_user"}, {"arguments", {{"content", "hi again"}}}}},
    }.dump()));
    Check(
        response["result"]["isError"] == false,
        "message_user succeeds even when the bare dm-<agentId> id is already archived");

    const std::vector<Message> archivedMessages = chatStore.RecentMessages("dm-alex", 10);
    Check(archivedMessages.empty(), "message_user does not resurrect the archived chat");
}

void TestUpdateAgentTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Agent target;
    target.id = "target-agent";
    target.name = "Target Agent";
    target.description = "before";
    target.systemPrompt = "before prompt";
    target.status = "active";
    target.toolPermissionsJson = "[]";
    target.canMessageJson = "[]";
    target.createdBy = "user";
    target.createdAt = 1;
    target.updatedAt = 1;
    agentStore.Upsert(target);
    SeedTestAgent(agentStore, "alex", {"update_agent"});

    ActivityLog activityLog(L"test-logs", "test-update");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string updateCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "update_agent"},
          {"arguments",
           {{"agent_id", "target-agent"},
            {"description", "after"},
            {"tool_permissions", json::array({"post_message"})}}}}},
    }.dump();
    const json updateResponse = json::parse(server.HandleLine(updateCall));
    Check(updateResponse["result"]["isError"] == false, "update_agent call succeeds");

    Agent updated;
    Check(agentStore.Get("target-agent", updated), "update_agent: agent still exists afterward");
    Check(updated.description == "after", "update_agent: description field was applied");
    Check(updated.systemPrompt == "before prompt", "update_agent: unspecified fields are left alone");
    Check(
        json::parse(updated.toolPermissionsJson) == json::array({"post_message"}),
        "update_agent: tool_permissions field was applied");

    const std::string unknownAgentCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "update_agent"}, {"arguments", {{"agent_id", "no-such-agent"}, {"name", "x"}}}}},
    }.dump();
    const json unknownAgentResponse = json::parse(server.HandleLine(unknownAgentCall));
    Check(unknownAgentResponse["result"]["isError"] == true, "update_agent rejects an unknown agent id");
}

void TestToolPermissionEnforcement() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    // Agent exists but has no tool permissions at all.
    SeedTestAgent(agentStore, "alex", {});

    ActivityLog activityLog(L"test-logs", "test-permission-denied");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string postCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"content", "should be blocked"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(postCall));
    Check(
        response["result"]["isError"] == true,
        "Tools::Call denies a tool not in the agent's tool_permissions");

    const std::vector<Message> messages = chatStore.RecentMessages("chat-1", 10);
    Check(
        messages.size() == 1 && messages[0].type == "system_event" &&
            messages[0].content.find("post_message") != std::string::npos,
        "Tools::Call logs a system_event message for a denied tool call");
    Check(
        chatStore.RecentMessages("chat-1", 10)[0].content.find("without permission") != std::string::npos,
        "the system_event message explains the call was blocked for lack of permission");

    // Unknown agent id (no row at all) must also fail closed, not crash.
    ActivityLog activityLog2(L"test-logs", "test-permission-unknown-agent");
    McpServer serverUnknownAgent(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog2,
        "nobody", "chat-1");
    const json unknownAgentResponse = json::parse(serverUnknownAgent.HandleLine(postCall));
    Check(
        unknownAgentResponse["result"]["isError"] == true,
        "Tools::Call fails closed for a tool call from an agent with no AgentStore row");
}

void TestRememberTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    SeedTestAgent(agentStore, "alex", {"remember"});

    ActivityLog activityLog(L"test-logs", "test-remember");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string rememberCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "remember"}, {"arguments", {{"key", "favorite_color"}, {"value", "teal"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(rememberCall));
    Check(response["result"]["isError"] == false, "remember call succeeds");
    const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
    Check(result.value("status", "") == "remembered", "remember returns status 'remembered'");

    std::string factValue;
    Check(
        agentStore.GetFact("alex", "favorite_color", factValue) && factValue == "teal",
        "remember actually persists the fact via AgentStore::SetFact");
}

void TestListAgentsTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    SeedTestAgent(agentStore, "alex", {"list_agents"});

    Agent disabled;
    disabled.id = "disabled-agent";
    disabled.name = "Disabled Agent";
    disabled.description = "should not show up";
    disabled.systemPrompt = "test";
    disabled.status = "disabled";
    disabled.toolPermissionsJson = "[]";
    disabled.canMessageJson = "[]";
    disabled.createdBy = "user";
    disabled.createdAt = 1;
    disabled.updatedAt = 1;
    agentStore.Upsert(disabled);

    ActivityLog activityLog(L"test-logs", "test-list-agents");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string listCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "list_agents"}, {"arguments", json::object()}}},
    }.dump();
    const json response = json::parse(server.HandleLine(listCall));
    Check(response["result"]["isError"] == false, "list_agents call succeeds");
    const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
    Check(result.is_array() && result.size() == 1, "list_agents returns only the active agent");
    Check(
        result[0]["id"] == "alex" && result[0]["name"] == "alex" && result[0]["description"] == "test fixture",
        "list_agents returns id/name/description for the active agent");
}

void TestListAgentsToolExposesPermissionsToUpdateAgentHolder() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    // Alex holds update_agent, so should see tool_permissions/can_message.
    SeedTestAgent(agentStore, "alex", {"list_agents", "update_agent"});
    // Tyrell doesn't, so should NOT — but still needs list_agents itself to
    // make the second call below (as tyrell) succeed at all.
    SeedTestAgent(agentStore, "tyrell", {"list_agents", "post_message", "read_chat"});

    Agent tyrell;
    agentStore.Get("tyrell", tyrell);
    tyrell.canMessageJson = json::array({"*"}).dump();
    agentStore.Upsert(tyrell);

    ActivityLog activityLog(L"test-logs", "test-list-agents-permissions");

    // As alex (holds update_agent): permissions should be visible.
    {
        McpServer server(
            chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
            "alex", "chat-1");
        const std::string listCall = json{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", "list_agents"}, {"arguments", json::object()}}},
        }.dump();
        const json response = json::parse(server.HandleLine(listCall));
        Check(response["result"]["isError"] == false, "list_agents (as alex) call succeeds");
        const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
        json tyrellEntry;
        for (const json& entry : result) {
            if (entry["id"] == "tyrell") {
                tyrellEntry = entry;
            }
        }
        Check(
            tyrellEntry.contains("tool_permissions") &&
                tyrellEntry["tool_permissions"] == json::array({"list_agents", "post_message", "read_chat"}),
            "list_agents: a caller holding update_agent sees other agents' tool_permissions");
        Check(
            tyrellEntry.contains("can_message") && tyrellEntry["can_message"] == json::array({"*"}),
            "list_agents: a caller holding update_agent sees other agents' can_message");
        Check(
            tyrellEntry.contains("system_prompt") && tyrellEntry["system_prompt"] == "test",
            "list_agents: a caller holding update_agent sees other agents' system_prompt");
    }

    // As tyrell (no update_agent): permissions should be hidden.
    {
        McpServer server(
            chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
            "tyrell", "chat-1");
        const std::string listCall = json{
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "tools/call"},
            {"params", {{"name", "list_agents"}, {"arguments", json::object()}}},
        }.dump();
        const json response = json::parse(server.HandleLine(listCall));
        Check(response["result"]["isError"] == false, "list_agents (as tyrell) call succeeds");
        const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
        json alexEntry;
        for (const json& entry : result) {
            if (entry["id"] == "alex") {
                alexEntry = entry;
            }
        }
        Check(
            !alexEntry.contains("tool_permissions") && !alexEntry.contains("can_message"),
            "list_agents: a caller without update_agent does not see other agents' permissions");
        Check(
            !alexEntry.contains("system_prompt"),
            "list_agents: a caller without update_agent does not see other agents' system_prompt");
    }
}

void TestListAgentsToolFiltersByAgentIdAndSections() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    SeedTestAgent(agentStore, "alex", {"list_agents", "update_agent"});
    SeedTestAgent(agentStore, "tyrell", {"list_agents", "post_message", "read_chat"});

    ActivityLog activityLog(L"test-logs", "test-list-agents-filters");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    // agent_id narrows to just that one agent.
    {
        const std::string call = json{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", "list_agents"}, {"arguments", {{"agent_id", "tyrell"}}}}},
        }.dump();
        const json response = json::parse(server.HandleLine(call));
        Check(response["result"]["isError"] == false, "list_agents(agent_id) call succeeds");
        const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
        Check(result.is_array() && result.size() == 1 && result[0]["id"] == "tyrell", "list_agents(agent_id) returns only the requested agent");
    }

    // sections narrows which gated fields come back.
    {
        const std::string call = json{
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "tools/call"},
            {"params",
             {{"name", "list_agents"},
              {"arguments", {{"agent_id", "tyrell"}, {"sections", json::array({"system_prompt"})}}}}},
        }.dump();
        const json response = json::parse(server.HandleLine(call));
        Check(response["result"]["isError"] == false, "list_agents(sections) call succeeds");
        const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
        Check(
            result.size() == 1 && result[0].contains("system_prompt") && !result[0].contains("tool_permissions") &&
                !result[0].contains("can_message"),
            "list_agents(sections) includes only the requested gated field");
    }

    // an unknown section is rejected.
    {
        const std::string call = json{
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "tools/call"},
            {"params", {{"name", "list_agents"}, {"arguments", {{"sections", json::array({"bogus"})}}}}},
        }.dump();
        const json response = json::parse(server.HandleLine(call));
        Check(response["result"]["isError"] == true, "list_agents rejects an unknown section name");
    }
}

void TestStartChatAndListMyChatsTools() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    // Seed the calling agent (alex) plus two targets, one alex is allowed to
    // message and one it isn't — start_chat's can_message check needs real
    // rows to check against, like TestMessageUserTool notes.
    Agent alex;
    alex.id = "alex";
    alex.name = "Alex";
    alex.description = "fixture";
    alex.systemPrompt = "test";
    alex.status = "active";
    alex.toolPermissionsJson = R"(["start_chat","list_my_chats"])";
    alex.canMessageJson = R"(["bravo"])";
    alex.createdBy = "user";
    alex.createdAt = 1;
    alex.updatedAt = 1;
    agentStore.Upsert(alex);

    Agent bravo;
    bravo.id = "bravo";
    bravo.name = "Bravo";
    bravo.description = "fixture";
    bravo.systemPrompt = "test";
    bravo.status = "active";
    bravo.toolPermissionsJson = "[]";
    bravo.canMessageJson = "[]";
    bravo.createdBy = "user";
    bravo.createdAt = 1;
    bravo.updatedAt = 1;
    agentStore.Upsert(bravo);

    Agent charlie; // not in alex's can_message
    charlie.id = "charlie";
    charlie.name = "Charlie";
    charlie.description = "fixture";
    charlie.systemPrompt = "test";
    charlie.status = "active";
    charlie.toolPermissionsJson = "[]";
    charlie.canMessageJson = "[]";
    charlie.createdBy = "user";
    charlie.createdAt = 1;
    charlie.updatedAt = 1;
    agentStore.Upsert(charlie);

    Agent disabled; // known but not active
    disabled.id = "delta";
    disabled.name = "Delta";
    disabled.description = "fixture";
    disabled.systemPrompt = "test";
    disabled.status = "disabled";
    disabled.toolPermissionsJson = "[]";
    disabled.canMessageJson = "[]";
    disabled.createdBy = "user";
    disabled.createdAt = 1;
    disabled.updatedAt = 1;
    agentStore.Upsert(disabled);
    // Allow alex to message delta too, so the rejection below is provably
    // about status, not can_message.
    alex.canMessageJson = R"(["bravo","charlie","delta"])";
    agentStore.Upsert(alex);
    // Re-tighten to the original can_message (excluding charlie) for the
    // rejection-path test below, then restore before the happy path.
    Agent alexNoCharlie = alex;
    alexNoCharlie.canMessageJson = R"(["bravo","delta"])";
    agentStore.Upsert(alexNoCharlie);

    ActivityLog activityLog(L"test-logs", "test-start-chat");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    // can_message rejection path: charlie isn't in alex's can_message.
    const std::string rejectedCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "start_chat"},
          {"arguments",
           {{"participant_ids", json::array({"bravo", "charlie"})}, {"initial_message", "hi team"}}}}},
    }.dump();
    const json rejectedResponse = json::parse(server.HandleLine(rejectedCall));
    Check(rejectedResponse["result"]["isError"] == true, "start_chat rejects a target outside can_message");
    Check(
        chatStore.ListChatsForParticipant("alex").empty(),
        "start_chat's can_message rejection does not create a chat as a side effect");

    // Inactive-agent rejection path.
    const std::string inactiveCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "start_chat"},
          {"arguments", {{"participant_ids", json::array({"delta"})}, {"initial_message", "hi"}}}}},
    }.dump();
    const json inactiveResponse = json::parse(server.HandleLine(inactiveCall));
    Check(inactiveResponse["result"]["isError"] == true, "start_chat rejects a non-active agent");

    // Restore alex's can_message to allow bravo (already does) and try the
    // happy path.
    const std::string happyCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         {{"name", "start_chat"},
          {"arguments",
           {{"participant_ids", json::array({"bravo"})},
            {"title", "Project Kickoff"},
            {"initial_message", "let's get started"}}}}},
    }.dump();
    const json happyResponse = json::parse(server.HandleLine(happyCall));
    Check(happyResponse["result"]["isError"] == false, "start_chat happy path succeeds");
    const json happyResult = json::parse(happyResponse["result"]["content"][0]["text"].get<std::string>());
    const std::string newChatId = happyResult.value("chat_id", "");
    Check(!newChatId.empty(), "start_chat returns a new chat id");
    Check(newChatId.rfind("dm-", 0) != 0, "start_chat's chat id never collides with the dm- prefix");

    Chat newChat;
    Check(chatStore.GetChat(newChatId, newChat), "start_chat: the new chat row exists");
    Check(newChat.discordChannelId.empty(), "start_chat leaves discordChannelId empty for lazy creation");

    Check(
        chatStore.IsParticipant(newChatId, "agent", "alex") && chatStore.IsParticipant(newChatId, "agent", "bravo"),
        "start_chat adds both the caller and the validated target as agent participants");
    Check(
        !chatStore.IsParticipant(newChatId, "user", "user"),
        "start_chat does not add an explicit user participant row");

    const std::vector<Message> newChatMessages = chatStore.RecentMessages(newChatId, 10);
    Check(
        newChatMessages.size() == 1 && newChatMessages[0].content == "let's get started" &&
            newChatMessages[0].senderId == "alex",
        "start_chat inserts the initial message from the calling agent");

    // list_my_chats now reflects the new chat plus the original.
    const std::string listCall = json{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", {{"name", "list_my_chats"}, {"arguments", json::object()}}},
    }.dump();
    const json listResponse = json::parse(server.HandleLine(listCall));
    Check(listResponse["result"]["isError"] == false, "list_my_chats call succeeds");
    const json listResult = json::parse(listResponse["result"]["content"][0]["text"].get<std::string>());
    bool foundNewChat = false;
    for (const json& c : listResult["chats"]) {
        if (c.value("id", "") == newChatId) {
            foundNewChat = true;
            Check(c.value("title", "") == "Project Kickoff", "list_my_chats includes the chat's title");
        }
    }
    Check(foundNewChat, "list_my_chats includes the chat start_chat just created");
}

void TestStartChatWithNoOtherParticipants() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    SeedTestAgent(agentStore, "tyrell", {"start_chat"});

    ActivityLog activityLog(L"test-logs", "test-start-chat-solo");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "tyrell", "chat-1");

    // Omitting participant_ids entirely must succeed — a chat with just the
    // caller, meant to have other agents added later via add_agent_to_chat/
    // request_add_agent_to_chat rather than starting as a DM.
    const std::string omittedCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "start_chat"}, {"arguments", {{"initial_message", "starting solo"}}}}},
    }.dump();
    const json omittedResponse = json::parse(server.HandleLine(omittedCall));
    Check(omittedResponse["result"]["isError"] == false, "start_chat succeeds with participant_ids omitted");
    const json omittedResult = json::parse(omittedResponse["result"]["content"][0]["text"].get<std::string>());
    const std::string omittedChatId = omittedResult.value("chat_id", "");
    Check(!omittedChatId.empty(), "start_chat (omitted participant_ids) returns a new chat id");
    Check(
        chatStore.ListParticipantAgentIds(omittedChatId).size() == 1 &&
            chatStore.IsParticipant(omittedChatId, "agent", "tyrell"),
        "start_chat (omitted participant_ids) adds only the caller as a participant");

    // An explicit empty array must succeed the same way.
    const std::string emptyArrayCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "start_chat"},
          {"arguments",
           {{"participant_ids", json::array()},
            {"title", "also solo"}, // distinct title so its id doesn't collide with the call above
            {"initial_message", "also solo"}}}}},
    }.dump();
    const json emptyArrayResponse = json::parse(server.HandleLine(emptyArrayCall));
    Check(
        emptyArrayResponse["result"]["isError"] == false,
        "start_chat succeeds with an explicit empty participant_ids array");

    // A non-array participant_ids is still rejected.
    const std::string badTypeCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         {{"name", "start_chat"}, {"arguments", {{"participant_ids", "not-an-array"}, {"initial_message", "x"}}}}},
    }.dump();
    const json badTypeResponse = json::parse(server.HandleLine(badTypeCall));
    Check(badTypeResponse["result"]["isError"] == true, "start_chat rejects a non-array participant_ids");
}

void TestChunkForDiscord() {
    Check(
        ChunkForDiscord("short message").size() == 1 && ChunkForDiscord("short message")[0] == "short message",
        "ChunkForDiscord: content under the limit is returned as a single unchanged chunk");
    Check(ChunkForDiscord("").size() == 1, "ChunkForDiscord: empty content still returns one (empty) chunk");

    // Over the 1990-char soft cap, no newlines at all — must hard-split,
    // and every chunk must individually be well under Discord's real 2000
    // hard cap.
    const std::string longFlat(2692, 'x');
    const std::vector<std::string> flatChunks = ChunkForDiscord(longFlat);
    Check(flatChunks.size() >= 2, "ChunkForDiscord: content over the limit is split into multiple chunks");
    for (const std::string& chunk : flatChunks) {
        Check(chunk.size() <= 2000, "ChunkForDiscord: every chunk is at or under Discord's 2000-char cap");
    }
    std::string reassembled;
    for (const std::string& chunk : flatChunks) {
        reassembled += chunk;
    }
    Check(reassembled == longFlat, "ChunkForDiscord: concatenating all chunks reproduces the original content");

    // With a newline conveniently placed near the split point, prefer
    // breaking there over a mid-word hard split.
    const std::string withNewline = std::string(1985, 'a') + "\nSecond paragraph starts here.";
    const std::vector<std::string> newlineChunks = ChunkForDiscord(withNewline);
    Check(
        newlineChunks.size() == 2 && newlineChunks[0] == std::string(1985, 'a') &&
            newlineChunks[1] == "Second paragraph starts here.",
        "ChunkForDiscord: prefers splitting on a newline near the limit over a hard mid-content split");
}

void TestMentions() {
    Agent alex;
    alex.id = "alex";
    alex.name = "Alex";
    alex.canMessageJson = R"(["bot-agent"])";

    Agent botAgent;
    botAgent.id = "bot-agent";
    botAgent.name = "Bot Agent"; // Slugify("Bot Agent") == "bot-agent"
    botAgent.canMessageJson = "[]";

    Agent wildcardAgent;
    wildcardAgent.id = "wildcard-agent";
    wildcardAgent.name = "Wildcard Agent";
    wildcardAgent.canMessageJson = R"(["*"])";

    const std::vector<Agent> candidates = {alex, botAgent, wildcardAgent};

    Check(
        Mentions::ParseMentions("hey @alex, take a look", candidates) == std::vector<std::string>{"alex"},
        "Mentions::ParseMentions matches a tag against an agent id");
    Check(
        Mentions::ParseMentions("hey @Bot-Agent can you help", candidates) == std::vector<std::string>{"bot-agent"},
        "Mentions::ParseMentions matches case-insensitively against Slugify(name)");
    Check(
        Mentions::ParseMentions("@nobody-real, ping", candidates).empty(),
        "Mentions::ParseMentions ignores a tag that matches no candidate");
    Check(
        Mentions::ParseMentions("@alex @alex @bot-agent", candidates).size() == 2,
        "Mentions::ParseMentions dedupes repeated tags for the same agent");

    botAgent.discordBotUserId = "999";
    const std::vector<Agent> withBotId = {alex, botAgent};
    Check(
        Mentions::ReflectMentionsForDiscord("cc @bot-agent", withBotId) == "cc <@999>",
        "Mentions::ReflectMentionsForDiscord uses a real mention for an agent with its own bot");
    Check(
        Mentions::ReflectMentionsForDiscord("cc @alex", withBotId) == "cc **@Alex**",
        "Mentions::ReflectMentionsForDiscord falls back to bold text for a shared-webhook agent");
    Check(
        Mentions::ReflectMentionsForDiscord("cc @nobody-real", withBotId) == "cc @nobody-real",
        "Mentions::ReflectMentionsForDiscord leaves an unresolved tag untouched");

    Check(
        Mentions::IsAllowedToMessage(alex, "bot-agent"), "Mentions::IsAllowedToMessage allows a listed target");
    Check(
        !Mentions::IsAllowedToMessage(alex, "wildcard-agent"),
        "Mentions::IsAllowedToMessage denies a target not in can_message");
    Check(
        Mentions::IsAllowedToMessage(wildcardAgent, "anything-at-all"),
        "Mentions::IsAllowedToMessage allows any target when can_message contains \"*\"");

    Agent malformed;
    malformed.canMessageJson = "not json";
    Check(
        !Mentions::IsAllowedToMessage(malformed, "bot-agent"),
        "Mentions::IsAllowedToMessage fails closed on malformed can_message JSON");
}

void TestGitHubRepoParseGitHubUrl() {
    std::string org, repo;

    Check(
        GitHubRepo::ParseGitHubUrl("https://github.com/foo/bar", org, repo) && org == "foo" && repo == "bar",
        "GitHubRepo::ParseGitHubUrl accepts https://github.com/org/repo");

    Check(
        GitHubRepo::ParseGitHubUrl("https://github.com/foo/bar.git", org, repo) && org == "foo" && repo == "bar",
        "GitHubRepo::ParseGitHubUrl strips a trailing .git");

    Check(
        GitHubRepo::ParseGitHubUrl("git@github.com:foo/bar.git", org, repo) && org == "foo" && repo == "bar",
        "GitHubRepo::ParseGitHubUrl accepts the git@github.com:org/repo.git SSH form");

    Check(
        GitHubRepo::ParseGitHubUrl("foo/bar", org, repo) && org == "foo" && repo == "bar",
        "GitHubRepo::ParseGitHubUrl accepts the bare org/repo form");

    Check(
        GitHubRepo::ParseGitHubUrl("https://github.com/foo/bar/", org, repo) && org == "foo" && repo == "bar",
        "GitHubRepo::ParseGitHubUrl tolerates a trailing slash");

    Check(
        !GitHubRepo::ParseGitHubUrl("https://github.com/foo", org, repo),
        "GitHubRepo::ParseGitHubUrl rejects a URL missing the repo segment");

    Check(
        !GitHubRepo::ParseGitHubUrl("https://github.com/foo/bar/tree/main", org, repo),
        "GitHubRepo::ParseGitHubUrl rejects an URL with extra path segments beyond org/repo");

    Check(
        !GitHubRepo::ParseGitHubUrl("https://gitlab.com/foo/bar", org, repo),
        "GitHubRepo::ParseGitHubUrl rejects a non-GitHub host");

    Check(
        !GitHubRepo::ParseGitHubUrl("not a url at all", org, repo),
        "GitHubRepo::ParseGitHubUrl rejects garbage input");

    Check(
        GitHubRepo::RepoId("Some-Org", "Some Repo!") == "some-org__some-repo",
        "GitHubRepo::RepoId slugifies and joins org/repo with a double underscore");
}

void TestRepoStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    RepoStore store(db);

    Repo missing;
    Check(!store.Get("no-such-repo", missing), "RepoStore: Get returns false for an unknown id");

    Repo repo;
    repo.id = "acme__widgets";
    repo.githubUrl = "https://github.com/acme/widgets";
    repo.localPath = "C:\\ProgramData\\RemoteCode\\Repos\\acme__widgets";
    repo.status = "cloning";
    repo.notes = "focus on the parser";
    repo.createdAt = 1;
    repo.updatedAt = 1;
    Check(store.Create(repo), "RepoStore: Create succeeds");

    Repo fetched;
    Check(store.Get("acme__widgets", fetched), "RepoStore: Get finds the created repo");
    Check(
        fetched.githubUrl == repo.githubUrl && fetched.localPath == repo.localPath &&
            fetched.notes == repo.notes && fetched.status == "cloning" && fetched.agentId.empty() &&
            fetched.lastError.empty(),
        "RepoStore: round-tripped fields match, agent_id/last_error empty by default");

    Check(store.SetStatus("acme__widgets", "ready", 2), "RepoStore: SetStatus succeeds");
    Repo afterStatus;
    store.Get("acme__widgets", afterStatus);
    Check(
        afterStatus.status == "ready" && afterStatus.updatedAt == 2,
        "RepoStore: SetStatus updates status and updated_at");

    Check(store.SetAgentId("acme__widgets", "widgets-expert", 3), "RepoStore: SetAgentId succeeds");
    Repo afterAgent;
    store.Get("acme__widgets", afterAgent);
    Check(afterAgent.agentId == "widgets-expert", "RepoStore: SetAgentId round-trips");

    Check(store.SetError("acme__widgets", "gh: repository not found", 4), "RepoStore: SetError succeeds");
    Repo afterError;
    store.Get("acme__widgets", afterError);
    Check(
        afterError.status == "failed" && afterError.lastError == "gh: repository not found",
        "RepoStore: SetError also sets status to 'failed'");

    Check(
        store.Create(Repo{"other__repo", "https://github.com/other/repo", "C:\\x", "", "cloning", "", "", false, 5, 5}),
        "RepoStore: Create a second repo");
    const std::vector<Repo> all = store.ListAll();
    Check(all.size() == 2, "RepoStore: ListAll returns every repo");

    Check(store.ClearError("acme__widgets", 6), "RepoStore: ClearError succeeds");
    Repo afterClear;
    store.Get("acme__widgets", afterClear);
    Check(
        afterClear.status == "cloning" && afterClear.lastError.empty() && afterClear.updatedAt == 6,
        "RepoStore: ClearError resets status to 'cloning' and empties last_error");

    Check(!fetched.onboardingTriggered, "RepoStore: onboarding_triggered defaults to false");
    Check(
        store.TryClaimOnboarding("acme__widgets"), "RepoStore: TryClaimOnboarding succeeds the first time");
    Repo afterClaim;
    store.Get("acme__widgets", afterClaim);
    Check(afterClaim.onboardingTriggered, "RepoStore: TryClaimOnboarding sets onboarding_triggered");
    Check(
        !store.TryClaimOnboarding("acme__widgets"),
        "RepoStore: TryClaimOnboarding returns false on a repo already claimed");

    store.SetStatus("other__repo", "ready", 7);
    const std::vector<Repo> pending = store.ListPendingOnboarding();
    Check(
        pending.size() == 1 && pending[0].id == "other__repo",
        "RepoStore: ListPendingOnboarding returns only ready, unclaimed repos");
}

void TestWorkspaceStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    WorkspaceStore store(db);

    Workspace missing;
    Check(!store.Get("no-such-workspace", missing), "WorkspaceStore: Get returns false for an unknown id");

    Workspace workspace;
    workspace.id = "workspace-widgets-1";
    workspace.title = "Widgets bugfix";
    workspace.repoIdsJson = R"(["acme__widgets"])";
    workspace.chatId = "workspace-chat-workspace-widgets-1";
    workspace.createdBy = "user";
    workspace.status = "ready";
    workspace.createdAt = 1;
    workspace.updatedAt = 1;
    Check(store.Create(workspace), "WorkspaceStore: Create succeeds");

    Workspace fetched;
    Check(store.Get("workspace-widgets-1", fetched), "WorkspaceStore: Get finds the created workspace");
    Check(
        fetched.title == "Widgets bugfix" && fetched.repoIdsJson == R"(["acme__widgets"])" &&
            fetched.chatId == workspace.chatId && fetched.status == "ready" &&
            fetched.discordCategoryId.empty() && fetched.lastError.empty(),
        "WorkspaceStore: round-tripped fields match, discord_category_id/last_error empty by default");

    const std::vector<Workspace> pendingBefore = store.ListPendingDiscordSetup();
    Check(
        pendingBefore.size() == 1 && pendingBefore[0].id == "workspace-widgets-1",
        "WorkspaceStore: ListPendingDiscordSetup finds a 'ready' workspace");

    Check(
        store.SetDiscordCategoryId("workspace-widgets-1", "999", 2), "WorkspaceStore: SetDiscordCategoryId succeeds");
    Workspace afterCategory;
    store.Get("workspace-widgets-1", afterCategory);
    Check(
        afterCategory.discordCategoryId == "999" && afterCategory.updatedAt == 2,
        "WorkspaceStore: SetDiscordCategoryId round-trips");

    Check(store.SetStatus("workspace-widgets-1", "active", 3), "WorkspaceStore: SetStatus succeeds");
    Check(
        store.ListPendingDiscordSetup().empty(),
        "WorkspaceStore: an 'active' workspace no longer shows up as pending Discord setup");

    Check(
        store.SetError("workspace-widgets-1", "git worktree add failed", 4), "WorkspaceStore: SetError succeeds");
    Workspace afterError;
    store.Get("workspace-widgets-1", afterError);
    Check(
        afterError.status == "failed" && afterError.lastError == "git worktree add failed",
        "WorkspaceStore: SetError also sets status to 'failed'");

    Workspace second;
    second.id = "workspace-other-1";
    second.repoIdsJson = "[]";
    second.chatId = "workspace-chat-other-1";
    second.createdBy = "tyrell";
    second.status = "creating";
    second.createdAt = 5;
    second.updatedAt = 5;
    Check(store.Create(second), "WorkspaceStore: Create a second workspace");
    Check(store.ListAll().size() == 2, "WorkspaceStore: ListAll returns every workspace");

    Check(
        store.ListPendingAgentGrants().empty(), "WorkspaceStore: no pending agent grants before any are added");
    Check(
        store.AddPendingAgentGrant("workspace-widgets-1", "tyrell", 10),
        "WorkspaceStore: AddPendingAgentGrant succeeds");
    const std::vector<WorkspaceStore::PendingAgentGrant> pendingGrants = store.ListPendingAgentGrants();
    Check(
        pendingGrants.size() == 1 && pendingGrants[0].workspaceId == "workspace-widgets-1" &&
            pendingGrants[0].agentId == "tyrell",
        "WorkspaceStore: ListPendingAgentGrants reports the pending grant");
    Check(
        store.ClearPendingAgentGrant("workspace-widgets-1", "tyrell"),
        "WorkspaceStore: ClearPendingAgentGrant succeeds");
    Check(
        store.ListPendingAgentGrants().empty(), "WorkspaceStore: cleared grant no longer shows up as pending");
}

void TestTempPermissionStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    TempPermissionStore store(db);

    Check(
        !store.HasActiveGrant("tyrell", "create_workspace"),
        "TempPermissionStore: no active grant before one is created");
    Check(store.Grant("tyrell", "create_workspace", 1), "TempPermissionStore: Grant succeeds");
    Check(
        store.HasActiveGrant("tyrell", "create_workspace"),
        "TempPermissionStore: HasActiveGrant is true right after granting");
    Check(
        !store.HasActiveGrant("tyrell", "add_agent_to_workspace"),
        "TempPermissionStore: a grant for one tool doesn't leak into another tool");
    Check(
        !store.HasActiveGrant("alex", "create_workspace"),
        "TempPermissionStore: a grant for one agent doesn't leak into another agent");

    Check(store.Consume("tyrell", "create_workspace", 2), "TempPermissionStore: Consume succeeds");
    Check(
        !store.HasActiveGrant("tyrell", "create_workspace"),
        "TempPermissionStore: consumed grant is no longer active");

    // Consuming with nothing active to consume is forgiving, not an error.
    Check(
        store.Consume("tyrell", "create_workspace", 3),
        "TempPermissionStore: Consume on an already-consumed/nonexistent grant still returns true");
}

void TestWorkspaceCreatorValidation() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    // Empty request is rejected before anything else is touched.
    const WorkspaceCreator::Result emptyResult =
        WorkspaceCreator::Create(repoStore, workspaceStore, chatStore, {}, "", "");
    Check(!emptyResult.ok && !emptyResult.error.empty(), "WorkspaceCreator: rejects an empty repo list");

    // A token that doesn't resolve to any known repo is rejected outright.
    const WorkspaceCreator::Result unknownResult =
        WorkspaceCreator::Create(repoStore, workspaceStore, chatStore, {"no-such-repo"}, "", "");
    Check(
        !unknownResult.ok && unknownResult.error.find("unknown repo") != std::string::npos,
        "WorkspaceCreator: rejects a repo token that doesn't resolve to any imported repo");
    Check(workspaceStore.ListAll().empty(), "WorkspaceCreator: an unresolved-repo failure writes no workspace row");

    // A known repo that hasn't finished cloning yet is also rejected — same
    // "already-imported" bar RunRepoOnboarding uses.
    Repo cloning;
    cloning.id = "acme__widgets";
    cloning.githubUrl = "https://github.com/acme/widgets";
    cloning.localPath = "C:\\ProgramData\\RemoteCode\\Repos\\acme__widgets";
    cloning.status = "cloning";
    cloning.createdAt = 1;
    cloning.updatedAt = 1;
    repoStore.Create(cloning);

    const WorkspaceCreator::Result cloningResult =
        WorkspaceCreator::Create(repoStore, workspaceStore, chatStore, {"acme__widgets"}, "", "");
    Check(
        !cloningResult.ok && cloningResult.error.find("no local clone yet") != std::string::npos,
        "WorkspaceCreator: rejects a repo that's still cloning (no usable local clone yet)");

    // Resolving by the bare repo name (not just the exact repo id) must work
    // too — same id-or-name rule /create-workspace's free-text option relies
    // on.
    repoStore.SetStatus("acme__widgets", "active", 2);
    Repo active;
    repoStore.Get("acme__widgets", active);
    Check(active.status == "active", "WorkspaceCreator test setup: repo is now 'active'");
    // (Actual worktree creation needs a real git repo on disk, which this
    // hermetic unit test deliberately doesn't set up — see WorkspaceCreator.h
    // for why that's a separate, Discord-touching concern left to
    // Orchestrator/manual verification. This test only exercises the
    // validation that runs before any subprocess/filesystem work begins.)
}

void TestCreateWorkspaceToolValidation() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    ActivityLog activityLog(L"test-logs", "test-create-workspace");

    // No permission granted at all — must fail closed like every other tool.
    SeedTestAgent(agentStore, "tyrell", {"post_message"});
    McpServer serverNoPermission(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "tyrell", "chat-1");
    const std::string createCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "create_workspace"}, {"arguments", {{"repo_ids_or_names", json::array({"acme__widgets"})}}}}},
    }.dump();
    const json noPermissionResponse = json::parse(serverNoPermission.HandleLine(createCall));
    Check(
        noPermissionResponse["result"]["isError"] == true,
        "create_workspace: fails closed for an agent without the create_workspace permission");

    // Permitted, but with a missing/empty repo_ids_or_names — rejected by the
    // tool's own argument validation before WorkspaceCreator ever runs.
    SeedTestAgent(agentStore, "tyrell", {"create_workspace"});
    ActivityLog activityLog2(L"test-logs", "test-create-workspace-2");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog2,
        "tyrell", "chat-1");

    const std::string missingArgCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "create_workspace"}, {"arguments", json::object()}}},
    }.dump();
    const json missingArgResponse = json::parse(server.HandleLine(missingArgCall));
    Check(
        missingArgResponse["result"]["isError"] == true,
        "create_workspace: rejects a call with no repo_ids_or_names");

    const std::string emptyArrayCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "create_workspace"}, {"arguments", {{"repo_ids_or_names", json::array()}}}}},
    }.dump();
    const json emptyArrayResponse = json::parse(server.HandleLine(emptyArrayCall));
    Check(
        emptyArrayResponse["result"]["isError"] == true,
        "create_workspace: rejects a call with an empty repo_ids_or_names array");

    // Permitted, well-formed, but the repo doesn't exist — surfaces
    // WorkspaceCreator's own resolution error through the tool.
    const json unknownRepoResponse = json::parse(server.HandleLine(createCall));
    Check(
        unknownRepoResponse["result"]["isError"] == true,
        "create_workspace: surfaces WorkspaceCreator's 'unknown repo' error through the tool");
    const std::string unknownRepoErrorText = unknownRepoResponse["result"]["content"][0]["text"].get<std::string>();
    Check(
        unknownRepoErrorText.find("unknown repo") != std::string::npos,
        "create_workspace: the tool error message names the actual problem, not a generic failure");

    Check(workspaceStore.ListAll().empty(), "create_workspace: no failed call left behind a workspace row");
}

void TestGitHubOpsValidation() {
    std::string state, method, err;

    Check(
        GitHubOps::ResolveListState("", true, state, err) && state == "open",
        "GitHubOps::ResolveListState: empty filter defaults to 'open'");
    Check(
        GitHubOps::ResolveListState("merged", true, state, err) && state == "merged",
        "GitHubOps::ResolveListState: PR listing accepts 'merged'");
    err.clear();
    Check(
        !GitHubOps::ResolveListState("merged", false, state, err) && !err.empty(),
        "GitHubOps::ResolveListState: issue listing rejects 'merged' (no such issue state)");
    err.clear();
    Check(
        !GitHubOps::ResolveListState("bogus", true, state, err) && !err.empty(),
        "GitHubOps::ResolveListState: an unrecognized filter is rejected with a message");

    err.clear();
    Check(GitHubOps::ValidateSetPrStatus("open", err), "GitHubOps::ValidateSetPrStatus: accepts 'open'");
    Check(GitHubOps::ValidateSetPrStatus("closed", err), "GitHubOps::ValidateSetPrStatus: accepts 'closed'");
    Check(GitHubOps::ValidateSetPrStatus("approved", err), "GitHubOps::ValidateSetPrStatus: accepts 'approved'");
    Check(
        GitHubOps::ValidateSetPrStatus("changes-requested", err),
        "GitHubOps::ValidateSetPrStatus: accepts 'changes-requested'");
    err.clear();
    Check(
        !GitHubOps::ValidateSetPrStatus("merged", err),
        "GitHubOps::ValidateSetPrStatus: explicitly rejects 'merged'");
    Check(
        err.find("merge_pull_request") != std::string::npos,
        "GitHubOps::ValidateSetPrStatus: the 'merged' rejection message points at merge_pull_request");
    err.clear();
    Check(
        !GitHubOps::ValidateSetPrStatus("bogus", err) && !err.empty(),
        "GitHubOps::ValidateSetPrStatus: rejects an unrecognized status");

    err.clear();
    Check(GitHubOps::ValidateSetIssueStatus("open", err), "GitHubOps::ValidateSetIssueStatus: accepts 'open'");
    Check(GitHubOps::ValidateSetIssueStatus("closed", err), "GitHubOps::ValidateSetIssueStatus: accepts 'closed'");
    err.clear();
    Check(
        !GitHubOps::ValidateSetIssueStatus("merged", err) && !err.empty(),
        "GitHubOps::ValidateSetIssueStatus: rejects a PR-only status like 'merged'");

    err.clear();
    Check(
        GitHubOps::ValidateMergeMethod("", method, err) && method == "merge",
        "GitHubOps::ValidateMergeMethod: empty method defaults to 'merge'");
    Check(
        GitHubOps::ValidateMergeMethod("squash", method, err) && method == "squash",
        "GitHubOps::ValidateMergeMethod: accepts 'squash'");
    Check(
        GitHubOps::ValidateMergeMethod("rebase", method, err) && method == "rebase",
        "GitHubOps::ValidateMergeMethod: accepts 'rebase'");
    err.clear();
    Check(
        !GitHubOps::ValidateMergeMethod("bogus", method, err) && !err.empty(),
        "GitHubOps::ValidateMergeMethod: rejects an unrecognized method");
}

// Covers the 7 gh-backed PR/issue tools' own argument validation and
// repo_id resolution failure -- the two things testable here without a real
// `gh`/network call (see this task's brief). Every case below either fails
// validation before a repo lookup happens, or resolves to an unknown repo_id
// and stops there, so none of these ever reach GitHubOps' actual `gh.exe`
// subprocess calls.
void TestGitHubPrIssueToolsValidation() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    Repo repo;
    repo.id = "acme__widgets";
    repo.githubUrl = "https://github.com/acme/widgets";
    repo.localPath = "C:\\ProgramData\\RemoteCode\\Repos\\acme__widgets";
    repo.status = "ready";
    repo.createdAt = 1;
    repo.updatedAt = 1;
    Check(repoStore.Create(repo), "TestGitHubPrIssueToolsValidation: fixture repo created");

    SeedTestAgent(
        agentStore, "tyrell",
        {"list_pull_requests", "get_pr_status", "set_pr_status", "merge_pull_request", "list_issues", "create_issue",
         "set_issue_status"});
    ActivityLog activityLog(L"test-logs", "test-gh-pr-issue-tools");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "tyrell", "chat-1");

    int nextId = 1;
    auto call = [&](const std::string& name, const json& arguments) -> json {
        const std::string line = json{
            {"jsonrpc", "2.0"},
            {"id", nextId++},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", arguments}}},
        }.dump();
        return json::parse(server.HandleLine(line));
    };
    auto errorText = [](const json& response) -> std::string {
        return response["result"]["content"][0]["text"].get<std::string>();
    };

    // Permission enforcement fails closed like every other tool.
    {
        ActivityLog denyLog(L"test-logs", "test-gh-pr-issue-tools-deny");
        McpServer deniedServer(
            chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore,
            tempPermissionStore, taskStore, reminderStore, denyLog, "no-permissions-agent", "chat-1");
        SeedTestAgent(agentStore, "no-permissions-agent", {});
        const std::string line = json{
            {"jsonrpc", "2.0"},
            {"id", 0},
            {"method", "tools/call"},
            {"params", {{"name", "list_pull_requests"}, {"arguments", {{"repo_id", "acme__widgets"}}}}},
        }.dump();
        const json response = json::parse(deniedServer.HandleLine(line));
        Check(
            response["result"]["isError"] == true,
            "list_pull_requests: fails closed for an agent without the tool's permission");
    }

    // Missing required args.
    Check(
        call("list_pull_requests", json::object())["result"]["isError"] == true,
        "list_pull_requests: rejects a call with no repo_id");
    Check(
        call("get_pr_status", {{"repo_id", "acme__widgets"}})["result"]["isError"] == true,
        "get_pr_status: rejects a call with no pr_number");
    Check(
        call("set_pr_status", {{"repo_id", "acme__widgets"}, {"pr_number", 1}})["result"]["isError"] == true,
        "set_pr_status: rejects a call with no status");
    Check(
        call("merge_pull_request", json::object())["result"]["isError"] == true,
        "merge_pull_request: rejects a call with no repo_id/pr_number");
    Check(
        call("list_issues", json::object())["result"]["isError"] == true,
        "list_issues: rejects a call with no repo_id");
    Check(
        call("create_issue", {{"repo_id", "acme__widgets"}})["result"]["isError"] == true,
        "create_issue: rejects a call with no title");
    Check(
        call("set_issue_status", {{"repo_id", "acme__widgets"}, {"issue_number", 1}})["result"]["isError"] == true,
        "set_issue_status: rejects a call with no status");

    // pr_number/issue_number must actually be integers, not e.g. a numeric string.
    const json badPrNumber =
        call("get_pr_status", {{"repo_id", "acme__widgets"}, {"pr_number", "7"}});
    Check(badPrNumber["result"]["isError"] == true, "get_pr_status: rejects a non-integer pr_number");

    // set_pr_status explicitly rejects "merged" -- this is a hard design
    // requirement (merging must go through merge_pull_request instead), not
    // just a would-be-nice validation, so it gets its own dedicated case.
    const json mergedRejected =
        call("set_pr_status", {{"repo_id", "acme__widgets"}, {"pr_number", 1}, {"status", "merged"}});
    Check(mergedRejected["result"]["isError"] == true, "set_pr_status: rejects status 'merged'");
    Check(
        errorText(mergedRejected).find("merge_pull_request") != std::string::npos,
        "set_pr_status: the 'merged' rejection error tells the caller to use merge_pull_request");

    // Invalid status/filter/method values.
    Check(
        call("list_pull_requests", {{"repo_id", "acme__widgets"}, {"status", "bogus"}})["result"]["isError"] == true,
        "list_pull_requests: rejects an invalid status filter");
    Check(
        call("list_issues", {{"repo_id", "acme__widgets"}, {"status", "merged"}})["result"]["isError"] == true,
        "list_issues: rejects 'merged' as a status filter (issues have no merged state)");
    Check(
        call("set_pr_status", {{"repo_id", "acme__widgets"}, {"pr_number", 1}, {"status", "bogus"}})
                ["result"]["isError"] == true,
        "set_pr_status: rejects an unrecognized status");
    Check(
        call("merge_pull_request", {{"repo_id", "acme__widgets"}, {"pr_number", 1}, {"merge_method", "bogus"}})
                ["result"]["isError"] == true,
        "merge_pull_request: rejects an unrecognized merge_method");
    Check(
        call("set_issue_status", {{"repo_id", "acme__widgets"}, {"issue_number", 1}, {"status", "bogus"}})
                ["result"]["isError"] == true,
        "set_issue_status: rejects an unrecognized status");

    // Unknown repo_id resolves to a clear error rather than shelling out to
    // gh with garbage -- every tool below is well-formed and validation-
    // clean, so the only remaining failure point is ResolveOwnerRepo.
    const json unknownRepoList = call("list_pull_requests", {{"repo_id", "no-such-repo"}});
    Check(unknownRepoList["result"]["isError"] == true, "list_pull_requests: unknown repo_id is an error");
    Check(
        errorText(unknownRepoList).find("unknown repo") != std::string::npos,
        "list_pull_requests: the unknown-repo error names the actual problem");

    Check(
        call("get_pr_status", {{"repo_id", "no-such-repo"}, {"pr_number", 1}})["result"]["isError"] == true,
        "get_pr_status: unknown repo_id is an error");
    Check(
        call(
            "set_pr_status", {{"repo_id", "no-such-repo"}, {"pr_number", 1}, {"status", "open"}})["result"]["isError"] ==
            true,
        "set_pr_status: unknown repo_id is an error");
    Check(
        call("merge_pull_request", {{"repo_id", "no-such-repo"}, {"pr_number", 1}})["result"]["isError"] == true,
        "merge_pull_request: unknown repo_id is an error");
    Check(
        call("list_issues", {{"repo_id", "no-such-repo"}})["result"]["isError"] == true,
        "list_issues: unknown repo_id is an error");
    Check(
        call("create_issue", {{"repo_id", "no-such-repo"}, {"title", "x"}})["result"]["isError"] == true,
        "create_issue: unknown repo_id is an error");
    Check(
        call(
            "set_issue_status",
            {{"repo_id", "no-such-repo"}, {"issue_number", 1}, {"status", "open"}})["result"]["isError"] == true,
        "set_issue_status: unknown repo_id is an error");
}

void TestPromptTemplateStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    PromptTemplateStore store(db);

    std::string missing;
    Check(!store.Get("repo_onboarding_alex", missing), "PromptTemplateStore: Get is empty before seeding");

    Check(store.SeedDefaultsIfEmpty(), "PromptTemplateStore: SeedDefaultsIfEmpty succeeds");

    std::string alexContent;
    Check(
        store.Get(PromptTemplateNames::kRepoOnboardingAlex, alexContent) && !alexContent.empty(),
        "PromptTemplateStore: seeding populates repo_onboarding_alex");
    std::string agentContent;
    Check(
        store.Get(PromptTemplateNames::kRepoOnboardingAgent, agentContent) && !agentContent.empty(),
        "PromptTemplateStore: seeding populates repo_onboarding_agent");

    // Idempotent: calling again after an explicit edit must not clobber it.
    Check(store.Set(PromptTemplateNames::kRepoOnboardingAlex, "edited content", 99), "PromptTemplateStore: Set succeeds");
    Check(store.SeedDefaultsIfEmpty(), "PromptTemplateStore: SeedDefaultsIfEmpty is a no-op once non-empty");
    std::string afterReseed;
    store.Get(PromptTemplateNames::kRepoOnboardingAlex, afterReseed);
    Check(afterReseed == "edited content", "PromptTemplateStore: SeedDefaultsIfEmpty does not overwrite an edit");

    Check(store.ListAll().size() == 2, "PromptTemplateStore: ListAll returns both seeded templates");

    const std::string rendered = RenderPromptTemplate(
        "Repo {{repo_name}} at {{repo_url}} lives at {{local_path}}.{{notes}}", "acme/widgets",
        "https://github.com/acme/widgets", "C:\\repos\\widgets", "focus on parsing");
    Check(
        rendered ==
            "Repo acme/widgets at https://github.com/acme/widgets lives at C:\\repos\\widgets.\n"
            "Cardon's notes: focus on parsing\n",
        "RenderPromptTemplate substitutes all four placeholders with notes present");

    const std::string renderedNoNotes = RenderPromptTemplate(
        "{{repo_name}}/{{repo_url}}/{{local_path}}/{{notes}}/end", "n", "u", "p", "");
    Check(
        renderedNoNotes == "n/u/p//end", "RenderPromptTemplate substitutes an empty string when notes is empty");
}

void TestPromptTemplateMcpTools() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);
    promptTemplateStore.SeedDefaultsIfEmpty();
    SeedTestAgent(agentStore, "alex", {"get_prompt_template", "update_prompt_template"});

    ActivityLog activityLog(L"test-logs", "test-prompt-templates");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string getCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "get_prompt_template"}, {"arguments", {{"name", "repo_onboarding_alex"}}}}},
    }.dump();
    const json getResponse = json::parse(server.HandleLine(getCall));
    Check(getResponse["result"]["isError"] == false, "get_prompt_template succeeds for a known name");
    const json getResult = json::parse(getResponse["result"]["content"][0]["text"].get<std::string>());
    Check(!getResult.value("content", "").empty(), "get_prompt_template returns non-empty content");

    const std::string getUnknownCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "get_prompt_template"}, {"arguments", {{"name", "not_a_real_template"}}}}},
    }.dump();
    const json getUnknownResponse = json::parse(server.HandleLine(getUnknownCall));
    Check(getUnknownResponse["result"]["isError"] == true, "get_prompt_template rejects an unknown name");

    const std::string updateCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         {{"name", "update_prompt_template"},
          {"arguments", {{"name", "repo_onboarding_agent"}, {"content", "new content here"}}}}},
    }.dump();
    const json updateResponse = json::parse(server.HandleLine(updateCall));
    Check(updateResponse["result"]["isError"] == false, "update_prompt_template succeeds for a known name");

    std::string persisted;
    Check(
        promptTemplateStore.Get("repo_onboarding_agent", persisted) && persisted == "new content here",
        "update_prompt_template actually persisted the new content");

    const std::string updateUnknownCall = json{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params",
         {{"name", "update_prompt_template"}, {"arguments", {{"name", "made_up"}, {"content", "x"}}}}},
    }.dump();
    const json updateUnknownResponse = json::parse(server.HandleLine(updateUnknownCall));
    Check(updateUnknownResponse["result"]["isError"] == true, "update_prompt_template rejects an unknown name");
}

void TestChatStoreParticipantModes() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore store(db);

    Chat chat;
    chat.id = "chat-modes";
    chat.createdBy = "user";
    chat.status = "active";
    chat.createdAt = 1;
    Check(store.CreateChat(chat), "ChatStore: CreateChat for participant-mode test");

    Check(store.AddParticipant("chat-modes", "agent", "alex"), "ChatStore: AddParticipant alex");
    Check(store.AddParticipant("chat-modes", "agent", "bob"), "ChatStore: AddParticipant bob");

    const std::vector<ParticipantAgent> defaults = store.ListParticipantAgents("chat-modes");
    Check(defaults.size() == 2, "ChatStore: ListParticipantAgents returns both participants");
    for (const ParticipantAgent& pa : defaults) {
        Check(
            pa.mode == ParticipantMode::kAutoRespond,
            "ChatStore: a newly added participant defaults to auto_respond mode");
    }

    Check(
        store.SetParticipantMode("chat-modes", "agent", "bob", ParticipantMode::kListening),
        "ChatStore: SetParticipantMode succeeds");

    const std::vector<ParticipantAgent> afterSet = store.ListParticipantAgents("chat-modes");
    bool foundBobListening = false;
    bool foundAlexAutoRespond = false;
    for (const ParticipantAgent& pa : afterSet) {
        if (pa.agentId == "bob") {
            foundBobListening = pa.mode == ParticipantMode::kListening;
        }
        if (pa.agentId == "alex") {
            foundAlexAutoRespond = pa.mode == ParticipantMode::kAutoRespond;
        }
    }
    Check(foundBobListening, "ChatStore: bob's mode is now listening");
    Check(foundAlexAutoRespond, "ChatStore: alex is untouched (still auto_respond)");

    // Re-adding an existing participant (INSERT OR IGNORE) must not reset
    // an already-set mode back to the default.
    Check(store.AddParticipant("chat-modes", "agent", "bob"), "ChatStore: re-adding bob is a no-op success");
    const std::vector<ParticipantAgent> afterReAdd = store.ListParticipantAgents("chat-modes");
    bool stillListening = false;
    for (const ParticipantAgent& pa : afterReAdd) {
        if (pa.agentId == "bob") {
            stillListening = pa.mode == ParticipantMode::kListening;
        }
    }
    Check(stillListening, "ChatStore: re-adding an existing participant does not reset its mode");

    Check(store.RemoveParticipant("chat-modes", "agent", "bob"), "ChatStore: RemoveParticipant succeeds");
    Check(
        !store.IsParticipant("chat-modes", "agent", "bob"), "ChatStore: bob is no longer a participant");
    Check(store.IsParticipant("chat-modes", "agent", "alex"), "ChatStore: alex is still a participant");
}

void TestChatStoreArchiving() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore store(db);

    Check(store.CreateChat(Chat{"chat-a", "", "user", "active", "", 1}), "ChatStore: create chat-a");
    Check(store.CreateChat(Chat{"chat-b", "", "user", "active", "", 2}), "ChatStore: create chat-b");
    store.AddParticipant("chat-a", "agent", "alex");
    store.AddParticipant("chat-b", "agent", "alex");

    Check(store.SetChatStatus("chat-a", "archived"), "ChatStore: SetChatStatus succeeds");
    Chat fetched;
    Check(
        store.GetChat("chat-a", fetched) && fetched.status == "archived",
        "ChatStore: SetChatStatus persists the new status");

    const std::vector<Chat> defaultList = store.ListChats();
    Check(defaultList.size() == 1 && defaultList[0].id == "chat-b", "ChatStore: ListChats excludes archived by default");
    const std::vector<Chat> allList = store.ListChats(/*includeArchived=*/true);
    Check(allList.size() == 2, "ChatStore: ListChats(includeArchived=true) returns everything");

    const std::vector<Chat> defaultParticipantList = store.ListChatsForParticipant("alex");
    Check(
        defaultParticipantList.size() == 1 && defaultParticipantList[0].id == "chat-b",
        "ChatStore: ListChatsForParticipant excludes archived by default");
    const std::vector<Chat> allParticipantList = store.ListChatsForParticipant("alex", /*includeArchived=*/true);
    Check(
        allParticipantList.size() == 2,
        "ChatStore: ListChatsForParticipant(includeArchived=true) returns everything");
}

void TestChatStoreGetActiveDmChatForAgent() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore store(db);

    Chat noDm;
    Check(!store.GetActiveDmChatForAgent("dax", noDm), "ChatStore: GetActiveDmChatForAgent finds nothing before any DM exists");

    Check(store.CreateChat(Chat{"dm-dax", "dax (DM)", "user", "active", "", 1}), "ChatStore: create original dm-dax");
    store.AddParticipant("dm-dax", "agent", "dax");

    Chat active1;
    Check(
        store.GetActiveDmChatForAgent("dax", active1) && active1.id == "dm-dax",
        "ChatStore: GetActiveDmChatForAgent finds the original dm-<agentId> chat");

    // Simulate /create-dm's "retire the old one, start a new one" flow: the
    // recreated DM gets a distinct id since "dm-dax" is now taken by the
    // archived chat.
    Check(store.SetChatStatus("dm-dax", "archived"), "ChatStore: archive the original DM");
    Check(
        store.CreateChat(Chat{"dm-dax-999", "dax (DM)", "user", "active", "", 2}),
        "ChatStore: create the recreated dm-dax-999 chat");
    store.AddParticipant("dm-dax-999", "agent", "dax");

    Chat active2;
    Check(
        store.GetActiveDmChatForAgent("dax", active2) && active2.id == "dm-dax-999",
        "ChatStore: GetActiveDmChatForAgent follows to the new chat once the old one is archived, ignoring it "
        "even though it still has 'dax' as a participant");

    Chat otherAgent;
    Check(
        !store.GetActiveDmChatForAgent("alex", otherAgent),
        "ChatStore: GetActiveDmChatForAgent doesn't match a DM chat belonging to a different agent");
}

void TestRequestAddAgentToChatTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    SeedTestAgent(agentStore, "alex", {"request_add_agent_to_chat"});
    // Give alex permission to message bob (can_message defaults to [] in
    // SeedTestAgent, which would fail-closed IsAllowedToMessage below).
    Agent alex;
    agentStore.Get("alex", alex);
    alex.canMessageJson = json::array({"*"}).dump();
    agentStore.Upsert(alex);
    chatStore.AddParticipant("chat-1", "agent", "alex");

    SeedTestAgent(agentStore, "bob", {});

    ActivityLog activityLog(L"test-logs", "test-request-add-agent");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "alex", "chat-1");

    const std::string requestCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_add_agent_to_chat"},
          {"arguments", {{"target_agent_id", "bob"}, {"reason", "needs bob's expertise"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(requestCall));
    Check(response["result"]["isError"] == false, "request_add_agent_to_chat succeeds");

    const std::vector<Approval> unposted = approvalStore.ListUnposted();
    Check(unposted.size() == 1, "request_add_agent_to_chat leaves exactly one unposted approval");
    Check(
        !unposted.empty() && unposted[0].kind == "add_agent_to_chat" && unposted[0].status == "pending",
        "the unposted approval is a pending add_agent_to_chat request");

    Check(
        !chatStore.IsParticipant("chat-1", "agent", "bob"),
        "bob is not yet a participant before the approval is resolved");

    // Requesting the same agent again (still pending) is rejected outright
    // since bob isn't a participant yet, but a request for an agent already
    // IN the chat must be rejected regardless of approval state.
    const std::string requestAlexCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_add_agent_to_chat"},
          {"arguments", {{"target_agent_id", "alex"}, {"reason", "n/a"}}}}},
    }.dump();
    const json alexResponse = json::parse(server.HandleLine(requestAlexCall));
    Check(
        alexResponse["result"]["isError"] == true,
        "request_add_agent_to_chat rejects a target already in the chat");

    // Simulates what Orchestrator::HandleReaction does once Cardon approves.
    Check(
        approvalStore.Resolve(unposted[0].id, "approved", 2), "ApprovalStore: Resolve(approved) succeeds");
    chatStore.AddParticipant("chat-1", "agent", "bob");
    Check(chatStore.IsParticipant("chat-1", "agent", "bob"), "bob is a participant after the approval resolves");
}

void TestAddAgentToChatTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    Chat otherChat;
    otherChat.id = "chat-2";
    otherChat.createdBy = "user";
    otherChat.status = "active";
    otherChat.discordChannelId = "2";
    otherChat.createdAt = 1;
    chatStore.CreateChat(otherChat);

    SeedTestAgent(agentStore, "tyrell", {"add_agent_to_chat"});
    Agent tyrell;
    agentStore.Get("tyrell", tyrell);
    tyrell.canMessageJson = json::array({"*"}).dump();
    agentStore.Upsert(tyrell);
    chatStore.AddParticipant("chat-1", "agent", "tyrell");

    SeedTestAgent(agentStore, "sable", {});

    ActivityLog activityLog(L"test-logs", "test-add-agent-to-chat");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "tyrell", "chat-1");

    const std::string call = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "add_agent_to_chat"}, {"arguments", {{"target_agent_id", "sable"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(call));
    Check(response["result"]["isError"] == false, "add_agent_to_chat succeeds with no approval step");
    Check(
        chatStore.IsParticipant("chat-1", "agent", "sable"),
        "add_agent_to_chat adds the target as a chat participant immediately");

    const std::vector<ChatStore::PendingAgentGrant> pendingGrants = chatStore.ListPendingAgentGrants();
    Check(
        pendingGrants.size() == 1 && pendingGrants[0].chatId == "chat-1" && pendingGrants[0].agentId == "sable",
        "add_agent_to_chat leaves a pending Discord-access grant for Orchestrator to pick up");

    // Already a participant — rejected on a repeat call.
    const json repeatResponse = json::parse(server.HandleLine(call));
    Check(
        repeatResponse["result"]["isError"] == true,
        "add_agent_to_chat rejects a target already in the chat");

    // Unknown target agent is rejected.
    const std::string unknownCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "add_agent_to_chat"}, {"arguments", {{"target_agent_id", "no-such-agent"}}}}},
    }.dump();
    const json unknownResponse = json::parse(server.HandleLine(unknownCall));
    Check(unknownResponse["result"]["isError"] == true, "add_agent_to_chat rejects an unknown target agent");

    // An explicit chat_id the caller isn't a participant of is rejected.
    SeedTestAgent(agentStore, "wren", {});
    const std::string otherChatCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         {{"name", "add_agent_to_chat"}, {"arguments", {{"target_agent_id", "wren"}, {"chat_id", "chat-2"}}}}},
    }.dump();
    const json otherChatResponse = json::parse(server.HandleLine(otherChatCall));
    Check(
        otherChatResponse["result"]["isError"] == true,
        "add_agent_to_chat rejects an explicit chat_id the caller isn't a participant of");

    // can_message gates the target the same way request_add_agent_to_chat does.
    SeedTestAgent(agentStore, "restricted", {"add_agent_to_chat"});
    Agent restricted;
    agentStore.Get("restricted", restricted);
    restricted.canMessageJson = json::array().dump(); // allowed to message no one
    agentStore.Upsert(restricted);
    chatStore.AddParticipant("chat-1", "agent", "restricted");

    McpServer restrictedServer(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore, activityLog,
        "restricted", "chat-1");
    SeedTestAgent(agentStore, "unreachable", {});
    const std::string blockedCall = json{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", {{"name", "add_agent_to_chat"}, {"arguments", {{"target_agent_id", "unreachable"}}}}},
    }.dump();
    const json blockedResponse = json::parse(restrictedServer.HandleLine(blockedCall));
    Check(
        blockedResponse["result"]["isError"] == true,
        "add_agent_to_chat rejects a target the caller's can_message doesn't permit messaging");
}

void TestAddAgentToWorkspaceTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "workspace-chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    Workspace workspace;
    workspace.id = "workspace-1";
    workspace.repoIdsJson = "[]";
    workspace.chatId = "workspace-chat-1";
    workspace.createdBy = "user";
    workspace.status = "active";
    workspace.createdAt = 1;
    workspace.updatedAt = 1;
    workspaceStore.Create(workspace);

    SeedTestAgent(agentStore, "tyrell", {"add_agent_to_workspace"});
    Agent tyrell;
    agentStore.Get("tyrell", tyrell);
    tyrell.canMessageJson = json::array({"*"}).dump();
    agentStore.Upsert(tyrell);

    SeedTestAgent(agentStore, "dax", {});

    ActivityLog activityLog(L"test-add-agent-to-workspace", "test-add-agent-to-workspace");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "tyrell", "workspace-chat-1");

    const std::string call = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "add_agent_to_workspace"}, {"arguments", {{"workspace_id", "workspace-1"}, {"agent_id", "dax"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(call));
    Check(response["result"]["isError"] == false, "add_agent_to_workspace succeeds");

    Check(
        chatStore.IsParticipant("workspace-chat-1", "agent", "dax"),
        "add_agent_to_workspace adds the target as a chat participant");
    const std::vector<ParticipantAgent> participants = chatStore.ListParticipantAgents("workspace-chat-1");
    ParticipantAgent daxParticipant;
    for (const ParticipantAgent& p : participants) {
        if (p.agentId == "dax") {
            daxParticipant = p;
        }
    }
    Check(
        daxParticipant.mode == ParticipantMode::kListening,
        "add_agent_to_workspace defaults the new participant to listening mode");

    const std::vector<WorkspaceStore::PendingAgentGrant> pendingGrants = workspaceStore.ListPendingAgentGrants();
    Check(
        pendingGrants.size() == 1 && pendingGrants[0].workspaceId == "workspace-1" &&
            pendingGrants[0].agentId == "dax",
        "add_agent_to_workspace leaves a pending Discord-access grant for Orchestrator to pick up");

    // Adding the same agent again is rejected — already a participant.
    const json repeatResponse = json::parse(server.HandleLine(call));
    Check(
        repeatResponse["result"]["isError"] == true,
        "add_agent_to_workspace rejects an agent that's already part of the workspace");

    // Unknown workspace id is rejected.
    const std::string unknownWorkspaceCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "add_agent_to_workspace"},
          {"arguments", {{"workspace_id", "no-such-workspace"}, {"agent_id", "dax"}}}}},
    }.dump();
    const json unknownWorkspaceResponse = json::parse(server.HandleLine(unknownWorkspaceCall));
    Check(
        unknownWorkspaceResponse["result"]["isError"] == true,
        "add_agent_to_workspace rejects an unknown workspace id");
}

void TestRequestTemporaryPermissionTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    // request_temporary_permission is always allowed — deliberately NOT
    // granted here, to prove that.
    SeedTestAgent(agentStore, "tyrell", {});
    chatStore.AddParticipant("chat-1", "agent", "tyrell");

    ActivityLog activityLog(L"test-request-temp-permission", "test-request-temp-permission");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "tyrell", "chat-1");

    const std::string call = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_temporary_permission"},
          {"arguments", {{"tool_name", "create_workspace"}, {"reason", "one-off fix"}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(call));
    Check(
        response["result"]["isError"] == false,
        "request_temporary_permission itself succeeds even though tyrell holds no tool_permissions at all");

    const std::vector<Approval> unposted = approvalStore.ListUnposted();
    Check(unposted.size() == 1, "request_temporary_permission leaves exactly one unposted approval");
    Check(
        !unposted.empty() && unposted[0].kind == "temp_tool_permission" && unposted[0].status == "pending",
        "the unposted approval is a pending temp_tool_permission request");

    // An unknown tool name is rejected outright.
    const std::string unknownToolCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_temporary_permission"},
          {"arguments", {{"tool_name", "not_a_real_tool"}, {"reason", "n/a"}}}}},
    }.dump();
    const json unknownToolResponse = json::parse(server.HandleLine(unknownToolCall));
    Check(
        unknownToolResponse["result"]["isError"] == true,
        "request_temporary_permission rejects a tool name that doesn't exist");

    // Requesting a tool already permanently held short-circuits with no
    // approval created.
    Agent tyrell;
    agentStore.Get("tyrell", tyrell);
    tyrell.toolPermissionsJson = json::array({"read_chat"}).dump();
    agentStore.Upsert(tyrell);
    const std::string alreadyHeldCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_temporary_permission"},
          {"arguments", {{"tool_name", "read_chat"}, {"reason", "n/a"}}}}},
    }.dump();
    const json alreadyHeldResponse = json::parse(server.HandleLine(alreadyHeldCall));
    Check(alreadyHeldResponse["result"]["isError"] == false, "request_temporary_permission (already held) succeeds");
    const json alreadyHeldResult =
        json::parse(alreadyHeldResponse["result"]["content"][0]["text"].get<std::string>());
    Check(
        alreadyHeldResult["status"] == "already_permitted",
        "request_temporary_permission short-circuits when the tool is already permanently held");
    Check(
        approvalStore.ListUnposted().size() == 1,
        "the already-permitted short-circuit doesn't create a second approval");
}

void TestRequestNewToolTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    // request_new_tool is always allowed — deliberately NOT granted here, to
    // prove that.
    SeedTestAgent(agentStore, "tyrell", {});
    chatStore.AddParticipant("chat-1", "agent", "tyrell");

    ActivityLog activityLog(L"test-request-new-tool", "test-request-new-tool");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "tyrell", "chat-1");

    const std::string call = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_new_tool"},
          {"arguments",
           {{"tool_name", "read_metrics"}, {"description", "I need to check server CPU usage."}}}}},
    }.dump();
    const json response = json::parse(server.HandleLine(call));
    Check(
        response["result"]["isError"] == false,
        "request_new_tool succeeds even though tyrell holds no tool_permissions at all");
    const json result = json::parse(response["result"]["content"][0]["text"].get<std::string>());
    Check(result["status"] == "requested", "request_new_tool reports status 'requested'");
    const std::string newChatId = result["chat_id"].get<std::string>();
    Check(newChatId.rfind("toolreq-tyrell-", 0) == 0, "request_new_tool creates a toolreq-prefixed chat");

    Chat createdChat;
    Check(chatStore.GetChat(newChatId, createdChat), "the tool-request chat was actually created");
    const std::vector<ParticipantAgent> participants = chatStore.ListParticipantAgents(newChatId);
    Check(
        participants.size() == 1 && participants[0].agentId == "tyrell" &&
            participants[0].mode == ParticipantMode::kAutoRespond,
        "tyrell is the sole auto_respond participant of the new tool-request chat");

    const std::vector<Message> messages = chatStore.RecentMessages(newChatId, 10);
    Check(
        messages.size() == 1 && messages[0].content.find("read_metrics") != std::string::npos &&
            messages[0].content.find("CPU usage") != std::string::npos,
        "the request message names the proposed tool and carries the description");

    // A second call creates a distinct chat, not a reused one.
    const std::string secondCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "request_new_tool"}, {"arguments", {{"description", "Something else entirely."}}}}},
    }.dump();
    const json secondResponse = json::parse(server.HandleLine(secondCall));
    Check(secondResponse["result"]["isError"] == false, "a second request_new_tool call also succeeds");
    const json secondResult = json::parse(secondResponse["result"]["content"][0]["text"].get<std::string>());
    Check(
        secondResult["chat_id"].get<std::string>() != newChatId,
        "a second request_new_tool call creates a brand-new chat, not the same one");
}

void TestAlwaysAllowedToolsAndTempGrantFallback() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    // Empty tool_permissions on purpose — remember must still work.
    SeedTestAgent(agentStore, "tyrell", {});
    chatStore.AddParticipant("chat-1", "agent", "tyrell");

    ActivityLog activityLog(L"test-always-allowed-and-temp-grant", "test-always-allowed-and-temp-grant");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore, taskStore, reminderStore,
        activityLog, "tyrell", "chat-1");

    const std::string rememberCall = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "remember"}, {"arguments", {{"key", "note"}, {"value", "test value"}}}}},
    }.dump();
    const json rememberResponse = json::parse(server.HandleLine(rememberCall));
    Check(
        rememberResponse["result"]["isError"] == false,
        "remember succeeds for an agent with empty tool_permissions — it's always allowed");

    // list_my_chats and read_chat (default, no explicit chat_id) are also
    // baseline tools every agent needs just to function in a chat — must
    // succeed even with empty tool_permissions and no temp grant.
    const std::string listMyChatsCall = json{
        {"jsonrpc", "2.0"},
        {"id", 10},
        {"method", "tools/call"},
        {"params", {{"name", "list_my_chats"}, {"arguments", json::object()}}},
    }.dump();
    const json listMyChatsResponse = json::parse(server.HandleLine(listMyChatsCall));
    Check(
        listMyChatsResponse["result"]["isError"] == false,
        "list_my_chats succeeds for an agent with empty tool_permissions — it's always allowed");

    const std::string readChatDefaultCall = json{
        {"jsonrpc", "2.0"},
        {"id", 11},
        {"method", "tools/call"},
        {"params", {{"name", "read_chat"}, {"arguments", json::object()}}},
    }.dump();
    const json readChatDefaultResponse = json::parse(server.HandleLine(readChatDefaultCall));
    Check(
        readChatDefaultResponse["result"]["isError"] == false,
        "read_chat succeeds for an agent with empty tool_permissions — it's always allowed");

    // message_user too — every agent needs a way to reach Cardon directly
    // regardless of its own tool_permissions (confirmed live: an agent
    // finished real work and had no way to report it back).
    const std::string messageUserCall = json{
        {"jsonrpc", "2.0"},
        {"id", 12},
        {"method", "tools/call"},
        {"params", {{"name", "message_user"}, {"arguments", {{"content", "hi"}}}}},
    }.dump();
    const json messageUserResponse = json::parse(server.HandleLine(messageUserCall));
    Check(
        messageUserResponse["result"]["isError"] == false,
        "message_user succeeds for an agent with empty tool_permissions — it's always allowed");

    // post_message is NOT always-allowed and tyrell has no permanent grant
    // for it — must fail closed before any temp grant exists.
    const std::string postMessageCall = json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"content", "hello"}}}}},
    }.dump();
    const json deniedResponse = json::parse(server.HandleLine(postMessageCall));
    Check(deniedResponse["result"]["isError"] == true, "post_message is denied with no permanent or temp grant");

    // Grant a one-time use of post_message (simulates HandleReaction's
    // approval handling for a temp_tool_permission approval) — the very
    // next call succeeds, and the grant is then consumed.
    Check(
        tempPermissionStore.Grant("tyrell", "post_message", 1),
        "TempPermissionStore: Grant succeeds ahead of a call");
    const json firstAllowedResponse = json::parse(server.HandleLine(postMessageCall));
    Check(
        firstAllowedResponse["result"]["isError"] == false,
        "post_message succeeds once an active temp grant covers it");
    Check(
        !tempPermissionStore.HasActiveGrant("tyrell", "post_message"),
        "the temp grant is consumed after the call that used it succeeds");

    const json secondDeniedResponse = json::parse(server.HandleLine(postMessageCall));
    Check(
        secondDeniedResponse["result"]["isError"] == true,
        "a second post_message call after the grant is consumed is denied again");
}

void TestTaskStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    TaskStore store(db);

    Check(TaskStatus::IsValid("not_started"), "TaskStatus: 'not_started' is valid");
    Check(TaskStatus::IsValid("in_progress"), "TaskStatus: 'in_progress' is valid");
    Check(TaskStatus::IsValid("blocked"), "TaskStatus: 'blocked' is valid");
    Check(TaskStatus::IsValid("in_review"), "TaskStatus: 'in_review' is valid");
    Check(TaskStatus::IsValid("done"), "TaskStatus: 'done' is valid");
    Check(TaskStatus::IsValid("cancelled"), "TaskStatus: 'cancelled' is valid");
    Check(!TaskStatus::IsValid("done_done"), "TaskStatus: an unknown status string is invalid");
    Check(!TaskStatus::IsValid(""), "TaskStatus: an empty status string is invalid");

    Task missing;
    Check(!store.Get("no-such-task", missing), "TaskStore: Get returns false for an unknown id");

    Task task;
    task.id = "task-1";
    task.chatId = "chat-1";
    task.title = "Fix the parser";
    task.description = "It chokes on trailing commas";
    task.status = TaskStatus::kNotStarted;
    task.createdBy = "alex";
    task.createdAt = 1;
    task.updatedAt = 1;
    Check(store.Create(task), "TaskStore: Create succeeds");

    Task fetched;
    Check(store.Get("task-1", fetched), "TaskStore: Get finds the created task");
    Check(
        fetched.title == task.title && fetched.description == task.description &&
            fetched.status == TaskStatus::kNotStarted && fetched.workspaceId.empty() &&
            fetched.assigneeAgentId.empty(),
        "TaskStore: round-tripped fields match, workspace_id/assignee_agent_id empty by default");

    Check(
        store.UpdateStatus("task-1", TaskStatus::kInProgress, /*setAssignee=*/true, "dax", 2),
        "TaskStore: UpdateStatus(setAssignee=true) succeeds");
    Task afterAssign;
    store.Get("task-1", afterAssign);
    Check(
        afterAssign.status == TaskStatus::kInProgress && afterAssign.assigneeAgentId == "dax" &&
            afterAssign.updatedAt == 2,
        "TaskStore: UpdateStatus updates status, assignee, and updated_at together");

    Check(
        store.UpdateStatus("task-1", TaskStatus::kBlocked, /*setAssignee=*/false, "", 3),
        "TaskStore: UpdateStatus(setAssignee=false) succeeds");
    Task afterStatusOnly;
    store.Get("task-1", afterStatusOnly);
    Check(
        afterStatusOnly.status == TaskStatus::kBlocked && afterStatusOnly.assigneeAgentId == "dax",
        "TaskStore: UpdateStatus with setAssignee=false leaves the existing assignee untouched");

    Task task2;
    task2.id = "task-2";
    task2.workspaceId = "workspace-1";
    task2.title = "Ship the release";
    task2.status = TaskStatus::kDone;
    task2.assigneeAgentId = "tyrell";
    task2.createdBy = "tyrell";
    task2.createdAt = 4;
    task2.updatedAt = 4;
    store.Create(task2);

    Check(store.List("", "", "").size() == 2, "TaskStore: List with no filters returns every task");
    Check(
        store.List(TaskStatus::kDone, "", "").size() == 1, "TaskStore: List filters by status");
    Check(
        store.List("", "tyrell", "").size() == 1, "TaskStore: List filters by assignee_agent_id");
    Check(
        store.List("", "", "workspace-1").size() == 1, "TaskStore: List filters by workspace_id");
    Check(
        store.List(TaskStatus::kBlocked, "dax", "").size() == 1,
        "TaskStore: List filters correctly combine status and assignee");
}

void TestReminderStore() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ReminderStore store(db);

    Reminder missing;
    Check(!store.Get("no-such-reminder", missing), "ReminderStore: Get returns false for an unknown id");

    Reminder reminder;
    reminder.id = "reminder-1";
    reminder.chatId = "chat-1";
    reminder.message = "stand up in 5";
    reminder.fireAt = 100;
    reminder.createdBy = "alex";
    reminder.status = ReminderStatus::kPending;
    reminder.createdAt = 1;
    Check(store.Create(reminder), "ReminderStore: Create succeeds");

    Reminder fetched;
    Check(store.Get("reminder-1", fetched), "ReminderStore: Get finds the created reminder");
    Check(
        fetched.message == reminder.message && fetched.fireAt == 100 && fetched.status == ReminderStatus::kPending,
        "ReminderStore: round-tripped fields match");

    Reminder futureReminder;
    futureReminder.id = "reminder-2";
    futureReminder.chatId = "chat-1";
    futureReminder.message = "still far off";
    futureReminder.fireAt = 500;
    futureReminder.createdBy = "alex";
    futureReminder.status = ReminderStatus::kPending;
    futureReminder.createdAt = 1;
    store.Create(futureReminder);

    Reminder otherAgentReminder;
    otherAgentReminder.id = "reminder-3";
    otherAgentReminder.chatId = "chat-1";
    otherAgentReminder.message = "not alex's";
    otherAgentReminder.fireAt = 50;
    otherAgentReminder.createdBy = "tyrell";
    otherAgentReminder.status = ReminderStatus::kPending;
    otherAgentReminder.createdAt = 1;
    store.Create(otherAgentReminder);

    const std::vector<Reminder> due = store.ListDue(100);
    Check(due.size() == 2, "ReminderStore: ListDue returns every pending reminder with fire_at <= now");
    bool sawReminder1 = false, sawReminder3 = false;
    for (const Reminder& r : due) {
        if (r.id == "reminder-1") sawReminder1 = true;
        if (r.id == "reminder-3") sawReminder3 = true;
        Check(r.id != "reminder-2", "ReminderStore: ListDue excludes a reminder whose fire_at is still in the future");
    }
    Check(sawReminder1 && sawReminder3, "ReminderStore: ListDue includes both due reminders regardless of creator");

    Check(
        store.ListPendingByCreator("alex").size() == 2,
        "ReminderStore: ListPendingByCreator scopes to the given creator");

    Check(store.SetStatus("reminder-1", ReminderStatus::kFired), "ReminderStore: SetStatus succeeds");
    Check(
        store.ListPendingByCreator("alex").size() == 1,
        "ReminderStore: a fired reminder no longer shows up in ListPendingByCreator");
    const std::vector<Reminder> dueAfterFire = store.ListDue(1000);
    Check(
        dueAfterFire.size() == 2,
        "ReminderStore: ListDue still returns the other two still-pending reminders");
    for (const Reminder& r : dueAfterFire) {
        Check(r.id != "reminder-1", "ReminderStore: a fired reminder no longer shows up in ListDue");
    }
}

void TestTaskToolsRoundTripAndValidation() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    SeedTestAgent(agentStore, "alex", {"create_task", "update_task_status", "list_tasks"});

    ActivityLog activityLog(L"test-task-tools", "test-task-tools");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore,
        taskStore, reminderStore, activityLog, "alex", "chat-1");

    int nextId = 1;
    auto call = [&](const std::string& name, const json& arguments) -> json {
        const std::string line = json{
            {"jsonrpc", "2.0"},
            {"id", nextId++},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", arguments}}},
        }.dump();
        return json::parse(server.HandleLine(line));
    };
    auto resultOf = [](const json& response) {
        return json::parse(response["result"]["content"][0]["text"].get<std::string>());
    };

    const json createResponse = call("create_task", {{"title", "Write the docs"}});
    Check(createResponse["result"]["isError"] == false, "create_task with no chat_id/workspace_id succeeds");
    const json created = resultOf(createResponse);
    Check(created["status"] == "not_started", "create_task defaults status to not_started");
    const std::string taskId = created["task_id"].get<std::string>();

    Task stored;
    Check(taskStore.Get(taskId, stored), "create_task actually wrote a tasks row");
    Check(
        stored.chatId == "chat-1" && stored.workspaceId.empty(),
        "create_task with no workspace_id defaults chat_id to the current chat");

    const json listResponse = call("list_tasks", json::object());
    Check(listResponse["result"]["isError"] == false, "list_tasks succeeds");
    const json listed = resultOf(listResponse);
    Check(
        listed["tasks"].is_array() && listed["tasks"].size() == 1 && listed["tasks"][0]["id"] == taskId,
        "create_task -> list_tasks round-trip returns the created task");

    const json badStatusResponse = call("update_task_status", {{"task_id", taskId}, {"status", "not_a_status"}});
    Check(badStatusResponse["result"]["isError"] == true, "update_task_status rejects an invalid status string");

    const json updateResponse =
        call("update_task_status", {{"task_id", taskId}, {"status", "in_progress"}, {"assignee_agent_id", "dax"}});
    Check(updateResponse["result"]["isError"] == false, "update_task_status with a valid status succeeds");

    Task afterUpdate;
    taskStore.Get(taskId, afterUpdate);
    Check(
        afterUpdate.status == "in_progress" && afterUpdate.assigneeAgentId == "dax",
        "update_task_status actually updated status and assignee together");

    const json filteredList = call("list_tasks", {{"assignee_agent_id", "dax"}});
    const json filtered = resultOf(filteredList);
    Check(
        filtered["tasks"].size() == 1, "list_tasks filters by assignee_agent_id after update_task_status reassigned it");

    const json missingTaskResponse = call("update_task_status", {{"task_id", "no-such-task"}, {"status", "done"}});
    Check(missingTaskResponse["result"]["isError"] == true, "update_task_status rejects an unknown task_id");
}

void TestScheduleReminderToolValidation() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);
    chatStore.AddParticipant("chat-1", "agent", "alex");

    SeedTestAgent(agentStore, "alex", {"schedule_reminder", "list_reminders", "cancel_reminder"});

    ActivityLog activityLog(L"test-schedule-reminder", "test-schedule-reminder");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore,
        taskStore, reminderStore, activityLog, "alex", "chat-1");

    int nextId = 1;
    auto call = [&](const std::string& name, const json& arguments) -> json {
        const std::string line = json{
            {"jsonrpc", "2.0"},
            {"id", nextId++},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", arguments}}},
        }.dump();
        return json::parse(server.HandleLine(line));
    };
    auto resultOf = [](const json& response) {
        return json::parse(response["result"]["content"][0]["text"].get<std::string>());
    };

    // Neither delay_seconds nor fire_at given.
    const json neitherResponse = call("schedule_reminder", {{"chat_id", "chat-1"}, {"message", "hi"}});
    Check(
        neitherResponse["result"]["isError"] == true,
        "schedule_reminder rejects a call with neither delay_seconds nor fire_at");

    // Both given at once — ambiguous, also rejected.
    const json bothResponse = call(
        "schedule_reminder",
        {{"chat_id", "chat-1"}, {"message", "hi"}, {"delay_seconds", 10}, {"fire_at", 200}});
    Check(bothResponse["result"]["isError"] == true, "schedule_reminder rejects a call with both delay_seconds and fire_at");

    const json delayResponse =
        call("schedule_reminder", {{"chat_id", "chat-1"}, {"message", "stand up"}, {"delay_seconds", 60}});
    Check(delayResponse["result"]["isError"] == false, "schedule_reminder succeeds with delay_seconds alone");
    const json delayResult = resultOf(delayResponse);
    const std::string reminderId = delayResult["reminder_id"].get<std::string>();

    Reminder stored;
    Check(reminderStore.Get(reminderId, stored), "schedule_reminder actually wrote a reminders row");
    Check(stored.status == ReminderStatus::kPending, "a freshly scheduled reminder starts out pending");

    const json fireAtResponse =
        call("schedule_reminder", {{"chat_id", "chat-1"}, {"message", "later"}, {"fire_at", 9999999999}});
    Check(fireAtResponse["result"]["isError"] == false, "schedule_reminder succeeds with fire_at alone");

    const json listResponse = call("list_reminders", json::object());
    const json listed = resultOf(listResponse);
    Check(listed["reminders"].size() == 2, "list_reminders returns both of the caller's pending reminders");

    const json cancelResponse = call("cancel_reminder", {{"reminder_id", reminderId}});
    Check(cancelResponse["result"]["isError"] == false, "cancel_reminder succeeds for the caller's own pending reminder");

    Reminder afterCancel;
    reminderStore.Get(reminderId, afterCancel);
    Check(afterCancel.status == ReminderStatus::kCancelled, "cancel_reminder actually marks the reminder cancelled");

    const json listAfterCancel = resultOf(call("list_reminders", json::object()));
    Check(
        listAfterCancel["reminders"].size() == 1,
        "a cancelled reminder no longer shows up in list_reminders");

    const json cancelAgainResponse = call("cancel_reminder", {{"reminder_id", reminderId}});
    Check(
        cancelAgainResponse["result"]["isError"] == true,
        "cancel_reminder rejects cancelling an already-cancelled reminder");

    const json cancelMissingResponse = call("cancel_reminder", {{"reminder_id", "no-such-reminder"}});
    Check(cancelMissingResponse["result"]["isError"] == true, "cancel_reminder rejects an unknown reminder_id");
}

// Regression coverage mirroring TestChatParticipationEnforcement, for the
// new schedule_reminder tool: chat_id is REQUIRED (never defaulted, unlike
// post_message/read_chat), so it must always be checked against
// chat_participants, not just when explicitly passed.
void TestScheduleReminderChatParticipationEnforcement() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);
    PromptTemplateStore promptTemplateStore(db);
    RepoStore repoStore(db);
    WorkspaceStore workspaceStore(db);
    TempPermissionStore tempPermissionStore(db);
    TaskStore taskStore(db);
    ReminderStore reminderStore(db);

    Chat ownChat;
    ownChat.id = "dm-alex";
    ownChat.createdBy = "user";
    ownChat.status = "active";
    ownChat.createdAt = 1;
    chatStore.CreateChat(ownChat);
    chatStore.AddParticipant("dm-alex", "agent", "alex");

    Chat otherChat;
    otherChat.id = "dm-tyrell";
    otherChat.createdBy = "user";
    otherChat.status = "active";
    otherChat.createdAt = 1;
    chatStore.CreateChat(otherChat);
    chatStore.AddParticipant("dm-tyrell", "agent", "tyrell");

    SeedTestAgent(agentStore, "alex", {"schedule_reminder"});

    ActivityLog activityLog(L"test-schedule-reminder-participation", "test-schedule-reminder-participation");
    McpServer server(
        chatStore, agentStore, approvalStore, promptTemplateStore, repoStore, workspaceStore, tempPermissionStore,
        taskStore, reminderStore, activityLog, "alex", "dm-alex");

    const json deniedResponse = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "schedule_reminder"},
          {"arguments", {{"chat_id", "dm-tyrell"}, {"message", "sneaking in"}, {"delay_seconds", 10}}}}},
    }.dump()));
    Check(
        deniedResponse["result"]["isError"] == true,
        "schedule_reminder denies an agent with no chat_participants row in the target chat");
    Check(
        reminderStore.ListPendingByCreator("alex").empty(),
        "schedule_reminder did not write a reminder targeting a chat alex isn't part of");

    const json allowedResponse = json::parse(server.HandleLine(json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params",
         {{"name", "schedule_reminder"},
          {"arguments", {{"chat_id", "dm-alex"}, {"message", "hi from alex"}, {"delay_seconds", 10}}}}},
    }.dump()));
    Check(
        allowedResponse["result"]["isError"] == false,
        "schedule_reminder still succeeds for a chat the caller actually participates in");
}

} // namespace

int main() {
    // Each test runs through RunTest so an unexpected exception in one test
    // (e.g. a bad assumption about a response shape) is caught and reported
    // as a named failure, rather than aborting the whole binary before any
    // later test — or the final pass/fail summary — gets a chance to run.
#define RUN(fn) RunTest(#fn, fn)
    RUN(TestGreeting);
    RUN(TestSchema);
    RUN(TestDmParticipantCleanupMigration);
    RUN(TestAgentStore);
    RUN(TestAgentStoreBotToken);
    RUN(TestAgentSessionStore);
    RUN(TestAgentSessionStoreGetIfFresh);
    RUN(TestChatSummaryStore);
    RUN(TestChatStore);
    RUN(TestChatStoreParticipantModes);
    RUN(TestChatStoreArchiving);
    RUN(TestChatStoreGetActiveDmChatForAgent);
    RUN(TestMcpServer);
    RUN(TestChatParticipationEnforcement);
    RUN(TestApprovalWorkflowTool);
    RUN(TestMessageUserTool);
    RUN(TestPostMessageAttachments);
    RUN(TestMessageUserToolPicksFreshIdWhenBareOneIsArchived);
    RUN(TestUpdateAgentTool);
    RUN(TestToolPermissionEnforcement);
    RUN(TestRememberTool);
    RUN(TestListAgentsTool);
    RUN(TestListAgentsToolExposesPermissionsToUpdateAgentHolder);
    RUN(TestListAgentsToolFiltersByAgentIdAndSections);
    RUN(TestStartChatAndListMyChatsTools);
    RUN(TestStartChatWithNoOtherParticipants);
    RUN(TestRequestAddAgentToChatTool);
    RUN(TestAddAgentToChatTool);
    RUN(TestAddAgentToWorkspaceTool);
    RUN(TestRequestTemporaryPermissionTool);
    RUN(TestRequestNewToolTool);
    RUN(TestAlwaysAllowedToolsAndTempGrantFallback);
    RUN(TestMentions);
    RUN(TestChunkForDiscord);
    RUN(TestGitHubRepoParseGitHubUrl);
    RUN(TestRepoStore);
    RUN(TestWorkspaceStore);
    RUN(TestTempPermissionStore);
    RUN(TestWorkspaceCreatorValidation);
    RUN(TestCreateWorkspaceToolValidation);
    RUN(TestGitHubOpsValidation);
    RUN(TestGitHubPrIssueToolsValidation);
    RUN(TestPromptTemplateStore);
    RUN(TestPromptTemplateMcpTools);
    RUN(TestTaskStore);
    RUN(TestReminderStore);
    RUN(TestTaskToolsRoundTripAndValidation);
    RUN(TestScheduleReminderToolValidation);
    RUN(TestScheduleReminderChatParticipationEnforcement);
#undef RUN

    if (failures > 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "All tests passed." << std::endl;
    return 0;
}

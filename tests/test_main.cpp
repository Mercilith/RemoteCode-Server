#include <iostream>
#include <string>

#include "../src/db/AgentSessionStore.h"
#include "../src/db/AgentStore.h"
#include "../src/db/ApprovalStore.h"
#include "../src/db/ChatStore.h"
#include "../src/db/Database.h"
#include "../src/db/Schema.h"
#include "../src/greeting.h"
#include "../src/mcp/McpServer.h"
#include "../src/third_party/json.hpp"
#include "../src/util/ActivityLog.h"
#include "../src/util/Mentions.h"

using nlohmann::json;

namespace {

int failures = 0;

void Check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << std::endl;
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
}

void TestMcpServer() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    ActivityLog activityLog(L"test-logs", "test-mcp");
    McpServer server(chatStore, agentStore, approvalStore, activityLog, "alex", "chat-1");

    // Notification (no "id") must produce no response.
    Check(
        server.HandleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),
        "McpServer: notification produces no response");

    const json initResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    Check(initResponse["id"] == 1, "McpServer: initialize echoes request id");
    Check(initResponse["result"].contains("protocolVersion"), "McpServer: initialize returns a protocolVersion");

    const json listResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    const json& tools = listResponse["result"]["tools"];
    Check(tools.is_array() && tools.size() == 7, "McpServer: tools/list returns exactly 7 tools");

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

void TestApprovalWorkflowTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    ActivityLog activityLog(L"test-logs", "test-approval");
    McpServer server(chatStore, agentStore, approvalStore, activityLog, "alex", "chat-1");

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

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    ActivityLog activityLog(L"test-logs", "test-message-user");
    McpServer server(chatStore, agentStore, approvalStore, activityLog, "alex", "chat-1");

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

void TestUpdateAgentTool() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);

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

    ActivityLog activityLog(L"test-logs", "test-update");
    McpServer server(chatStore, agentStore, approvalStore, activityLog, "alex", "chat-1");

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

void TestStartChatAndListMyChatsTools() {
    Database db;
    db.Open(L":memory:");
    Schema::EnsureCreated(db);
    ChatStore chatStore(db);
    AgentStore agentStore(db);
    ApprovalStore approvalStore(db);

    // Seed the calling agent (alex) plus two targets, one alex is allowed to
    // message and one it isn't — start_chat's can_message check needs real
    // rows to check against, like TestMessageUserTool notes.
    Agent alex;
    alex.id = "alex";
    alex.name = "Alex";
    alex.description = "fixture";
    alex.systemPrompt = "test";
    alex.status = "active";
    alex.toolPermissionsJson = "[]";
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
    McpServer server(chatStore, agentStore, approvalStore, activityLog, "alex", "chat-1");

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

} // namespace

int main() {
    TestGreeting();
    TestSchema();
    TestAgentStore();
    TestAgentStoreBotToken();
    TestAgentSessionStore();
    TestChatStore();
    TestMcpServer();
    TestApprovalWorkflowTool();
    TestMessageUserTool();
    TestUpdateAgentTool();
    TestStartChatAndListMyChatsTools();
    TestMentions();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "All tests passed." << std::endl;
    return 0;
}

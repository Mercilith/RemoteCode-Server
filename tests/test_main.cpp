#include <iostream>
#include <string>

#include "../src/db/AgentStore.h"
#include "../src/db/ApprovalStore.h"
#include "../src/db/ChatStore.h"
#include "../src/db/Database.h"
#include "../src/db/Schema.h"
#include "../src/greeting.h"
#include "../src/mcp/McpServer.h"
#include "../src/third_party/json.hpp"

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

    for (int i = 0; i < 3; ++i) {
        Message m;
        m.chatId = "chat-1";
        m.senderType = "user";
        m.senderId = "user-1";
        m.type = "text";
        m.content = "message " + std::to_string(i);
        m.createdAt = i;
        Check(store.InsertMessage(m) >= 0, "ChatStore: InsertMessage succeeds for message " + std::to_string(i));
    }

    const std::vector<Message> recent = store.RecentMessages("chat-1", 10);
    Check(recent.size() == 3, "ChatStore: RecentMessages returns all 3 messages");
    Check(
        recent.size() == 3 && recent[0].content == "message 0" && recent[2].content == "message 2",
        "ChatStore: RecentMessages is in chronological (oldest-first) order");

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

    McpServer server(chatStore, agentStore, approvalStore, "alex", "chat-1");

    // Notification (no "id") must produce no response.
    Check(
        server.HandleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),
        "McpServer: notification produces no response");

    const json initResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    Check(initResponse["id"] == 1, "McpServer: initialize echoes request id");
    Check(initResponse["result"].contains("protocolVersion"), "McpServer: initialize returns a protocolVersion");

    const json listResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    const json& tools = listResponse["result"]["tools"];
    Check(tools.is_array() && tools.size() == 4, "McpServer: tools/list returns exactly 4 tools");

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

    McpServer server(chatStore, agentStore, approvalStore, "alex", "chat-1");

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

    McpServer server(chatStore, agentStore, approvalStore, "alex", "chat-1");

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

} // namespace

int main() {
    TestGreeting();
    TestSchema();
    TestAgentStore();
    TestAgentStoreBotToken();
    TestChatStore();
    TestMcpServer();
    TestApprovalWorkflowTool();
    TestUpdateAgentTool();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "All tests passed." << std::endl;
    return 0;
}

#include <iostream>
#include <string>

#include "../src/db/AgentStore.h"
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

    Chat chat;
    chat.id = "chat-1";
    chat.createdBy = "user";
    chat.status = "active";
    chat.discordChannelId = "1";
    chat.createdAt = 1;
    chatStore.CreateChat(chat);

    McpServer server(chatStore, "alex");

    // Notification (no "id") must produce no response.
    Check(
        server.HandleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),
        "McpServer: notification produces no response");

    const json initResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    Check(initResponse["id"] == 1, "McpServer: initialize echoes request id");
    Check(initResponse["result"].contains("protocolVersion"), "McpServer: initialize returns a protocolVersion");

    const json listResponse = json::parse(server.HandleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    const json& tools = listResponse["result"]["tools"];
    Check(tools.is_array() && tools.size() == 2, "McpServer: tools/list returns exactly 2 tools");

    const std::string postCall = json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "post_message"}, {"arguments", {{"chat_id", "chat-1"}, {"content", "hi there"}}}}},
    }.dump();
    const json postResponse = json::parse(server.HandleLine(postCall));
    Check(postResponse["result"]["isError"] == false, "McpServer: post_message call succeeds");

    const std::vector<Message> stored = chatStore.RecentMessages("chat-1", 10);
    Check(
        stored.size() == 1 && stored[0].content == "hi there" && stored[0].senderId == "alex",
        "McpServer: post_message actually wrote the message as the scoped agent");

    const std::string readCall = json{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", {{"name", "read_chat"}, {"arguments", {{"chat_id", "chat-1"}}}}},
    }.dump();
    const json readResponse = json::parse(server.HandleLine(readCall));
    const json readResult = json::parse(readResponse["result"]["content"][0]["text"].get<std::string>());
    Check(
        readResult["messages"].size() == 1 && readResult["messages"][0]["content"] == "hi there",
        "McpServer: read_chat returns the message post_message wrote");

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

} // namespace

int main() {
    TestGreeting();
    TestSchema();
    TestAgentStore();
    TestChatStore();
    TestMcpServer();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "All tests passed." << std::endl;
    return 0;
}

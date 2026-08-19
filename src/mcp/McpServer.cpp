#include "McpServer.h"

#include <iostream>

#include "../third_party/json.hpp"
#include "Tools.h"

using nlohmann::json;

namespace {

json MakeError(const json& id, int code, const std::string& message) {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = {{"code", code}, {"message", message}};
    return response;
}

json MakeResult(const json& id, const json& result) {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response;
}

} // namespace

McpServer::McpServer(
    ChatStore& chatStore, AgentStore& agentStore, ApprovalStore& approvalStore,
    PromptTemplateStore& promptTemplateStore, RepoStore& repoStore, WorkspaceStore& workspaceStore,
    TempPermissionStore& tempPermissionStore, ActivityLog& activityLog, std::string agentId, std::string chatId)
    : chatStore_(chatStore),
      agentStore_(agentStore),
      approvalStore_(approvalStore),
      promptTemplateStore_(promptTemplateStore),
      repoStore_(repoStore),
      workspaceStore_(workspaceStore),
      tempPermissionStore_(tempPermissionStore),
      activityLog_(activityLog),
      agentId_(std::move(agentId)),
      chatId_(std::move(chatId)) {}

std::string McpServer::HandleLine(const std::string& line) {
    json request;
    try {
        request = json::parse(line);
    } catch (const json::parse_error&) {
        return MakeError(nullptr, -32700, "Parse error").dump();
    }

    const bool isNotification = !request.contains("id");
    const json id = isNotification ? json() : request["id"];
    const std::string method = request.value("method", "");

    if (method == "notifications/initialized") {
        return "";
    }

    if (method == "initialize") {
        json result;
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"] = {{"tools", json::object()}};
        result["serverInfo"] = {{"name", "remotecode-orchestrator"}, {"version", "0.1.0"}};
        return MakeResult(id, result).dump();
    }

    if (method == "tools/list") {
        return MakeResult(id, json{{"tools", Tools::Definitions()}}).dump();
    }

    if (method == "tools/call") {
        const json params = request.value("params", json::object());
        const std::string toolName = params.value("name", "");
        const json arguments = params.value("arguments", json::object());

        ToolContext ctx{
            chatStore_, agentStore_,       approvalStore_,      promptTemplateStore_, repoStore_,
            workspaceStore_, tempPermissionStore_, activityLog_, agentId_,             chatId_};
        std::string errorMessage;
        const json result = Tools::Call(ctx, toolName, arguments, errorMessage);

        json content = json::array();
        if (!errorMessage.empty()) {
            content.push_back({{"type", "text"}, {"text", errorMessage}});
            return MakeResult(id, json{{"content", content}, {"isError", true}}).dump();
        }
        content.push_back({{"type", "text"}, {"text", result.dump()}});
        return MakeResult(id, json{{"content", content}, {"isError", false}}).dump();
    }

    if (isNotification) {
        return "";
    }
    return MakeError(id, -32601, "Method not found: " + method).dump();
}

void McpServer::RunStdio() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        const std::string response = HandleLine(line);
        if (!response.empty()) {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }
}

#pragma once

#include <string>

#include "../db/AgentStore.h"
#include "../db/ApprovalStore.h"
#include "../db/ChatStore.h"
#include "../util/ActivityLog.h"

// Hand-rolled MCP server (stdio transport, JSON-RPC 2.0, one message per
// line — https://modelcontextprotocol.io stdio framing). Spawned as the
// worker's MCP subprocess for a single agent turn, so it's scoped to one
// agent id and chat id, and opens its own Database handle onto the same
// SQLite file the rest of the server uses.
class McpServer {
public:
    McpServer(
        ChatStore& chatStore, AgentStore& agentStore, ApprovalStore& approvalStore, ActivityLog& activityLog,
        std::string agentId, std::string chatId);

    // Processes one JSON-RPC message (a single line, no trailing newline)
    // and returns the response line to write back, or an empty string if
    // no response is needed (the message was a notification).
    std::string HandleLine(const std::string& line);

    // Reads newline-delimited JSON-RPC messages from stdin and writes
    // responses to stdout until stdin hits EOF.
    void RunStdio();

private:
    ChatStore& chatStore_;
    AgentStore& agentStore_;
    ApprovalStore& approvalStore_;
    ActivityLog& activityLog_;
    std::string agentId_;
    std::string chatId_;
};

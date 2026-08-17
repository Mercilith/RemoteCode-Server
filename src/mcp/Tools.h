#pragma once

#include <string>

#include "../db/ChatStore.h"
#include "../third_party/json.hpp"

// Tool definitions and dispatch for the hand-rolled MCP server. Only
// post_message/read_chat exist this pass — the rest of the design doc's
// tool surface (start_chat, remember, draft_agent, etc.) comes later.
class Tools {
public:
    static nlohmann::json Definitions();

    // Dispatches a tool call. On success returns the tool's JSON result and
    // leaves outError empty. On failure returns an empty json and sets
    // outError to a human-readable message.
    static nlohmann::json Call(
        ChatStore& chatStore, const std::string& agentId, const std::string& toolName,
        const nlohmann::json& arguments, std::string& outError);
};

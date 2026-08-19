#pragma once

#include <string>

#include "../db/AgentStore.h"
#include "../db/ApprovalStore.h"
#include "../db/ChatStore.h"
#include "../db/PromptTemplateStore.h"
#include "../db/RepoStore.h"
#include "../db/WorkspaceStore.h"
#include "../third_party/json.hpp"
#include "../util/ActivityLog.h"

// Everything a tool call needs, bundled up so Tools::Call doesn't grow an
// ever-longer parameter list as more tools land.
struct ToolContext {
    ChatStore& chatStore;
    AgentStore& agentStore;
    ApprovalStore& approvalStore;
    PromptTemplateStore& promptTemplateStore;
    // Read-only from a tool's point of view (repo import/cloning itself is
    // never delegated to an LLM — see Orchestrator::AddRepo) except for
    // create_workspace's git-worktree-add step, which is a plain programmatic
    // subprocess call, same rule.
    RepoStore& repoStore;
    WorkspaceStore& workspaceStore;
    ActivityLog& activityLog;
    std::string agentId; // the agent this MCP server instance is scoped to
    std::string chatId;  // the chat this turn is happening in — the default
                          // target for post_message/read_chat/approvals when
                          // the caller doesn't pass an explicit chat_id
};

// Tool definitions and dispatch for the hand-rolled MCP server. The rest of
// the design doc's tool surface (remember, list_agents, etc.) comes later.
class Tools {
public:
    static nlohmann::json Definitions();

    // Dispatches a tool call. On success returns the tool's JSON result and
    // leaves outError empty. On failure returns an empty json and sets
    // outError to a human-readable message.
    static nlohmann::json Call(
        ToolContext& ctx, const std::string& toolName, const nlohmann::json& arguments,
        std::string& outError);
};

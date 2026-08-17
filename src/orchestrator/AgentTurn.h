#pragma once

#include <string>
#include <vector>

#include "../db/AgentStore.h"
#include "../db/ChatStore.h"

struct AgentTurnResult {
    bool ok = false;
    std::string response; // agent's reply text, valid when ok
    std::string error;    // human-readable failure reason, valid when !ok
};

// Spawns the Node/TypeScript worker subprocess (worker/src/index.ts) for a
// single agent turn: writes {systemPrompt, messages, mcpServerCommand,
// mcpServerArgs} as JSON to its stdin, and reads its {response}/{error}
// JSON reply from stdout. The worker in turn spawns this same exe in
// `--mcp-server` mode (see main.cpp) as its MCP stdio subprocess, scoped to
// `agent.id` and `dbPath`.
class AgentTurn {
public:
    static AgentTurnResult Run(
        const Agent& agent, const std::vector<Message>& recentMessages, const std::wstring& dbPath);
};

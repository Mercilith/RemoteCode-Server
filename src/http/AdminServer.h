#pragma once

#include <memory>
#include <string>

#include "../db/AgentStore.h"

namespace httplib {
class Server;
} // namespace httplib

// Local-only (127.0.0.1) HTTP admin API for RemoteCode-Desktop: list/
// create/update agents, assign or clear a per-agent Discord bot token, and
// ask Alex to revise an agent. Started by Orchestrator on its own thread.
// Never exposed beyond loopback — no auth beyond that, matching this
// project's personal-single-user threat model (same as the Discord bot
// token's own trust boundary).
class AdminServer {
public:
    // dbPath/claudeConfigDir are only needed for the /revise endpoint,
    // which spawns a real Alex turn the same way Orchestrator does.
    AdminServer(AgentStore& agentStore, std::wstring dbPath, std::string claudeConfigDir);
    ~AdminServer();

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    // Binds 127.0.0.1:<port> and blocks serving requests until Stop().
    void Run(int port);
    void Stop();

private:
    AgentStore& agentStore_;
    std::wstring dbPath_;
    std::string claudeConfigDir_;
    std::unique_ptr<httplib::Server> server_;
};

// Fixed local port for the admin API — RemoteCode-Desktop's HTTP client is
// hardcoded to the same value (same-machine assumption for this pass).
constexpr int kAdminServerPort = 47291;

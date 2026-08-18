#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../db/AgentSessionStore.h"
#include "../db/AgentStore.h"
#include "../db/ChatStore.h"
#include "../third_party/json.hpp"

namespace httplib {
class Server;
} // namespace httplib

// Callback into Orchestrator::InjectTestMessage — see AdminServer's
// constructor comment and Orchestrator.h for why this is a callback rather
// than a back-reference to Orchestrator.
using InjectMessageFn = std::function<nlohmann::json(
    const std::string& chatId, const std::string& content, const std::string& senderId)>;

// Local-only (127.0.0.1) HTTP admin API for RemoteCode-Desktop: list/
// create/update agents, assign or clear a per-agent Discord bot token, and
// ask Alex to revise an agent. Also hosts a DEBUG/DEV-ONLY surface
// (`/debug/*`) for exercising the message-dispatch pipeline without a real
// Discord round-trip — see AdminServer.cpp. Started by Orchestrator on its
// own thread. Never exposed beyond loopback — no auth beyond that, matching
// this project's personal-single-user threat model (same as the Discord bot
// token's own trust boundary); the debug endpoints are no more sensitive
// than the rest of this API under that model.
class AdminServer {
public:
    // dbPath/claudeConfigDir/agentSessionStore are only needed for the
    // /revise endpoint, which spawns a real Alex turn the same way
    // Orchestrator does (including resuming a prior revise session for the
    // same agent, if one exists). chatStore is needed for the debug
    // endpoints (listing chats, injecting a test message). injectMessage is
    // Orchestrator::InjectTestMessage bound as a callback — see
    // Orchestrator.h; AdminServer deliberately doesn't hold an
    // Orchestrator& to avoid a circular/back-reference (Orchestrator owns
    // AdminServer as a member).
    AdminServer(
        AgentStore& agentStore, AgentSessionStore& agentSessionStore, ChatStore& chatStore,
        std::wstring dbPath, std::string claudeConfigDir, std::wstring logDir,
        InjectMessageFn injectMessage);
    ~AdminServer();

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    // Binds 127.0.0.1:<port> and blocks serving requests until Stop().
    void Run(int port);
    void Stop();

private:
    AgentStore& agentStore_;
    AgentSessionStore& agentSessionStore_;
    ChatStore& chatStore_;
    std::wstring dbPath_;
    std::string claudeConfigDir_;
    std::wstring logDir_;
    InjectMessageFn injectMessage_;
    std::unique_ptr<httplib::Server> server_;
};

// Fixed local port for the admin API — RemoteCode-Desktop's HTTP client is
// hardcoded to the same value (same-machine assumption for this pass).
constexpr int kAdminServerPort = 47291;

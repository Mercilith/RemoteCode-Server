#include <string>

#include "db/ChatStore.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "mcp/McpServer.h"
#include "service/ServiceMain.h"

namespace {

// Agent ids are always ASCII (generated identifiers) — a byte-for-byte
// narrow is exact.
std::string NarrowAscii(const std::wstring& wide) {
    std::string result;
    result.reserve(wide.size());
    for (const wchar_t c : wide) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

// Minimal argv scanner — this mode only ever receives the exact flags
// AgentTurn::Run constructs (see orchestrator/AgentTurn.cpp), so no need
// for a general-purpose parser. Uses wmain so paths/ids never round-trip
// through the ambiguous narrow argv encoding.
bool TryRunMcpServer(int argc, wchar_t* argv[]) {
    bool isMcpServer = false;
    std::wstring agentId;
    std::wstring dbPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--mcp-server") {
            isMcpServer = true;
        } else if (arg == L"--agent-id" && i + 1 < argc) {
            agentId = argv[++i];
        } else if (arg == L"--db-path" && i + 1 < argc) {
            dbPath = argv[++i];
        }
    }

    if (!isMcpServer) {
        return false;
    }

    Database db;
    if (!db.Open(dbPath) || !Schema::EnsureCreated(db)) {
        return true; // handled (albeit by failing) — do not fall through to service mode
    }

    ChatStore chatStore(db);
    McpServer server(chatStore, NarrowAscii(agentId));
    server.RunStdio();
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (TryRunMcpServer(argc, argv)) {
        return 0;
    }
    return ServiceMain::Run();
}

#pragma once

#include <cstdint>
#include <string>

#include "Database.h"

// One-time tool-permission grants — see the "request_temporary_permission"
// MCP tool (src/mcp/Tools.cpp), which any agent can call regardless of its
// own tool_permissions (see IsAlwaysAllowedTool). On Cardon's approval a row
// is inserted here; Tools::Call checks HasActiveGrant as a fallback when an
// agent's permanent tool_permissions don't include the requested tool, and
// consumes the grant (Consume) after the call actually succeeds — one grant
// is good for exactly one successful call, never reused.
class TempPermissionStore {
public:
    explicit TempPermissionStore(Database& db) : db_(db) {}

    bool Grant(const std::string& agentId, const std::string& toolName, int64_t grantedAt);
    bool HasActiveGrant(const std::string& agentId, const std::string& toolName);
    // Marks the oldest still-active grant for this agent/tool as consumed.
    // No-op (returns true) if there's nothing active to consume — a caller
    // that already checked HasActiveGrant before dispatching shouldn't see
    // this fail, but it's not itself the source of truth for "was this call
    // permitted," so it stays forgiving rather than erroring.
    bool Consume(const std::string& agentId, const std::string& toolName, int64_t consumedAt);

private:
    Database& db_;
};

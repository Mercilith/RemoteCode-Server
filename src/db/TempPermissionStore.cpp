#include "TempPermissionStore.h"

bool TempPermissionStore::Grant(const std::string& agentId, const std::string& toolName, int64_t grantedAt) {
    Statement stmt(
        db_, "INSERT INTO temp_tool_grants (agent_id, tool_name, granted_at, consumed_at) VALUES (?1,?2,?3,NULL);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, toolName);
    stmt.BindInt64(3, grantedAt);
    stmt.Step();
    return stmt.Ok();
}

bool TempPermissionStore::HasActiveGrant(const std::string& agentId, const std::string& toolName) {
    Statement stmt(
        db_,
        "SELECT 1 FROM temp_tool_grants WHERE agent_id = ?1 AND tool_name = ?2 AND consumed_at IS NULL LIMIT 1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, agentId);
    stmt.BindText(2, toolName);
    return stmt.Step();
}

bool TempPermissionStore::Consume(const std::string& agentId, const std::string& toolName, int64_t consumedAt) {
    Statement stmt(
        db_,
        "UPDATE temp_tool_grants SET consumed_at = ?1 WHERE id = ("
        "SELECT id FROM temp_tool_grants WHERE agent_id = ?2 AND tool_name = ?3 AND consumed_at IS NULL "
        "ORDER BY granted_at ASC LIMIT 1);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindInt64(1, consumedAt);
    stmt.BindText(2, agentId);
    stmt.BindText(3, toolName);
    stmt.Step();
    return stmt.Ok();
}

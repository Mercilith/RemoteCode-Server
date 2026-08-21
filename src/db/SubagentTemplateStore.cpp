#include "SubagentTemplateStore.h"

namespace {

constexpr const char* kColumns =
    "id, name, description, task_template, tool_permissions, scope, requires_review, created_by, "
    "created_at, updated_at";

void ReadRow(const Statement& stmt, SubagentTemplate& out) {
    out.id = stmt.ColumnText(0);
    out.name = stmt.ColumnText(1);
    out.description = stmt.ColumnText(2);
    out.taskTemplate = stmt.ColumnText(3);
    out.toolPermissionsJson = stmt.ColumnText(4);
    out.scope = stmt.ColumnIsNull(5) ? "" : stmt.ColumnText(5);
    out.requiresReview = stmt.ColumnInt64(6) != 0;
    out.createdBy = stmt.ColumnText(7);
    out.createdAt = stmt.ColumnInt64(8);
    out.updatedAt = stmt.ColumnInt64(9);
}

} // namespace

bool SubagentTemplateStore::Get(const std::string& id, SubagentTemplate& outTemplate) {
    Statement stmt(db_, std::string("SELECT ") + kColumns + " FROM subagent_templates WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadRow(stmt, outTemplate);
    return true;
}

bool SubagentTemplateStore::Upsert(const SubagentTemplate& tmpl) {
    Statement stmt(
        db_,
        "INSERT INTO subagent_templates (id, name, description, task_template, tool_permissions, scope, "
        "requires_review, created_by, created_at, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, description=excluded.description, "
        "task_template=excluded.task_template, tool_permissions=excluded.tool_permissions, "
        "scope=excluded.scope, requires_review=excluded.requires_review, updated_at=excluded.updated_at;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, tmpl.id);
    stmt.BindText(2, tmpl.name);
    stmt.BindText(3, tmpl.description);
    stmt.BindText(4, tmpl.taskTemplate);
    stmt.BindText(5, tmpl.toolPermissionsJson);
    if (tmpl.scope.empty()) {
        stmt.BindNull(6);
    } else {
        stmt.BindText(6, tmpl.scope);
    }
    stmt.BindInt64(7, tmpl.requiresReview ? 1 : 0);
    stmt.BindText(8, tmpl.createdBy);
    stmt.BindInt64(9, tmpl.createdAt);
    stmt.BindInt64(10, tmpl.updatedAt);
    stmt.Step();
    return stmt.Ok();
}

std::vector<SubagentTemplate> SubagentTemplateStore::ListAll() {
    std::vector<SubagentTemplate> templates;
    Statement stmt(db_, std::string("SELECT ") + kColumns + " FROM subagent_templates ORDER BY name;");
    if (!stmt.Valid()) {
        return templates;
    }
    while (stmt.Step()) {
        SubagentTemplate tmpl;
        ReadRow(stmt, tmpl);
        templates.push_back(std::move(tmpl));
    }
    return templates;
}

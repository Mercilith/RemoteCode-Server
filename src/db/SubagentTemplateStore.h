#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

// A reusable preset for spawn_subagent — saved once via
// create_subagent_template, then referenced by id from any later
// spawn_subagent(template_id=...) call instead of re-specifying
// task/tool_permissions/scope from scratch each time. See mcp/Tools.cpp's
// SpawnSubagent/CreateSubagentTemplate for how this gets rendered/applied.
struct SubagentTemplate {
    std::string id;
    std::string name;
    std::string description;
    // May contain {{key}} placeholders, filled in at spawn time from the
    // caller's `variables` object — see RenderTemplate in Tools.cpp.
    std::string taskTemplate;
    std::string toolPermissionsJson;  // JSON array default for spawned subagents
    std::string scope;                // may be empty
    bool requiresReview = false;
    std::string createdBy;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

class SubagentTemplateStore {
public:
    explicit SubagentTemplateStore(Database& db) : db_(db) {}

    bool Get(const std::string& id, SubagentTemplate& outTemplate);
    // Upsert, keyed by id.
    bool Upsert(const SubagentTemplate& tmpl);
    // Every stored template, ordered by name — used by list_subagent_templates.
    std::vector<SubagentTemplate> ListAll();

private:
    Database& db_;
};

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

struct Approval {
    std::string id;
    std::string chatId;
    int64_t messageId = 0; // messages.id this approval's draft is attached to
    std::string requestedBy; // agent id that requested it
    std::string kind;        // "create_agent" this pass
    std::string payloadJson; // e.g. {"agent_id": "..."} for create_agent
    std::string status;      // "pending" | "approved" | "rejected"
    int64_t createdAt = 0;
    int64_t resolvedAt = 0; // 0 if unresolved
};

class ApprovalStore {
public:
    explicit ApprovalStore(Database& db) : db_(db) {}

    bool Create(const Approval& approval);

    // Pending approvals whose draft message hasn't been posted to Discord
    // yet (messages.discord_message_id IS NULL) — Orchestrator polls this
    // after each turn to post the draft and seed the approve/reject
    // reactions.
    std::vector<Approval> ListUnposted();

    // Looks up the approval attached to a given messages.id — used to map
    // a reaction event (via the message it landed on) back to its approval.
    bool GetByMessageId(int64_t messageId, Approval& outApproval);

    bool Resolve(const std::string& id, const std::string& status, int64_t resolvedAt);

private:
    Database& db_;
};

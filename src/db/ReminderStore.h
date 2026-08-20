#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Database.h"

// Fixed status enum for the `reminders` table — see the schedule_reminder /
// list_reminders / cancel_reminder MCP tools (mcp/Tools.cpp) and
// Orchestrator::FireDueReminders, the periodic thread that actually posts a
// reminder once its fire_at has passed (see Orchestrator.h's header comment
// on the reminder thread for why this can't happen inside the MCP
// subprocess itself — same "DB-only from the subprocess, real work on a
// later pass in the main process" split create_workspace/add_agent_to_workspace
// use).
namespace ReminderStatus {
constexpr const char* kPending = "pending";
constexpr const char* kFired = "fired";
constexpr const char* kCancelled = "cancelled";
} // namespace ReminderStatus

struct Reminder {
    std::string id;
    std::string chatId;
    std::string message;
    int64_t fireAt = 0;      // unix timestamp
    std::string createdBy;   // the agent id that scheduled it
    std::string status;      // one of ReminderStatus's constants
    int64_t createdAt = 0;
};

class ReminderStore {
public:
    explicit ReminderStore(Database& db) : db_(db) {}

    bool Create(const Reminder& reminder);
    bool Get(const std::string& id, Reminder& outReminder);
    bool SetStatus(const std::string& id, const std::string& status);

    // Every still-pending reminder `createdBy` scheduled — backs
    // list_reminders, which only ever shows an agent its own reminders.
    std::vector<Reminder> ListPendingByCreator(const std::string& createdBy);

    // Every reminder with status='pending' and fire_at <= nowTs — the
    // periodic firing thread's poll query (see Orchestrator::FireDueReminders).
    std::vector<Reminder> ListDue(int64_t nowTs);

private:
    Database& db_;
};

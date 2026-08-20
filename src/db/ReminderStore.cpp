#include "ReminderStore.h"

namespace {

constexpr const char* kReminderColumns = "id, chat_id, message, fire_at, created_by, status, created_at";

void ReadReminderRow(const Statement& stmt, Reminder& out) {
    out.id = stmt.ColumnText(0);
    out.chatId = stmt.ColumnText(1);
    out.message = stmt.ColumnText(2);
    out.fireAt = stmt.ColumnInt64(3);
    out.createdBy = stmt.ColumnText(4);
    out.status = stmt.ColumnText(5);
    out.createdAt = stmt.ColumnInt64(6);
}

} // namespace

bool ReminderStore::Create(const Reminder& reminder) {
    Statement stmt(
        db_,
        "INSERT INTO reminders (id, chat_id, message, fire_at, created_by, status, created_at) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, reminder.id);
    stmt.BindText(2, reminder.chatId);
    stmt.BindText(3, reminder.message);
    stmt.BindInt64(4, reminder.fireAt);
    stmt.BindText(5, reminder.createdBy);
    stmt.BindText(6, reminder.status);
    stmt.BindInt64(7, reminder.createdAt);
    stmt.Step();
    return stmt.Ok();
}

bool ReminderStore::Get(const std::string& id, Reminder& outReminder) {
    Statement stmt(db_, std::string("SELECT ") + kReminderColumns + " FROM reminders WHERE id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, id);
    if (!stmt.Step()) {
        return false;
    }
    ReadReminderRow(stmt, outReminder);
    return true;
}

bool ReminderStore::SetStatus(const std::string& id, const std::string& status) {
    Statement stmt(db_, "UPDATE reminders SET status = ?1 WHERE id = ?2;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, status);
    stmt.BindText(2, id);
    stmt.Step();
    return stmt.Ok();
}

std::vector<Reminder> ReminderStore::ListPendingByCreator(const std::string& createdBy) {
    std::vector<Reminder> reminders;
    Statement stmt(
        db_, std::string("SELECT ") + kReminderColumns +
                 " FROM reminders WHERE created_by = ?1 AND status = 'pending' ORDER BY fire_at ASC;");
    if (!stmt.Valid()) {
        return reminders;
    }
    stmt.BindText(1, createdBy);
    while (stmt.Step()) {
        Reminder reminder;
        ReadReminderRow(stmt, reminder);
        reminders.push_back(std::move(reminder));
    }
    return reminders;
}

std::vector<Reminder> ReminderStore::ListDue(int64_t nowTs) {
    std::vector<Reminder> reminders;
    Statement stmt(
        db_, std::string("SELECT ") + kReminderColumns +
                 " FROM reminders WHERE status = 'pending' AND fire_at <= ?1 ORDER BY fire_at ASC;");
    if (!stmt.Valid()) {
        return reminders;
    }
    stmt.BindInt64(1, nowTs);
    while (stmt.Step()) {
        Reminder reminder;
        ReadReminderRow(stmt, reminder);
        reminders.push_back(std::move(reminder));
    }
    return reminders;
}

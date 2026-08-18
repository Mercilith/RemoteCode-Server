#include "ApprovalStore.h"

namespace {

void ReadRow(const Statement& stmt, Approval& out) {
    out.id = stmt.ColumnText(0);
    out.chatId = stmt.ColumnText(1);
    out.messageId = stmt.ColumnInt64(2);
    out.requestedBy = stmt.ColumnText(3);
    out.kind = stmt.ColumnText(4);
    out.payloadJson = stmt.ColumnText(5);
    out.status = stmt.ColumnText(6);
    out.createdAt = stmt.ColumnInt64(7);
    out.resolvedAt = stmt.ColumnIsNull(8) ? 0 : stmt.ColumnInt64(8);
}

} // namespace

bool ApprovalStore::Create(const Approval& approval) {
    Statement stmt(
        db_,
        "INSERT INTO approvals (id, chat_id, message_id, requested_by, kind, payload, status, "
        "created_at, resolved_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,NULL);");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, approval.id);
    stmt.BindText(2, approval.chatId);
    stmt.BindInt64(3, approval.messageId);
    stmt.BindText(4, approval.requestedBy);
    stmt.BindText(5, approval.kind);
    stmt.BindText(6, approval.payloadJson);
    stmt.BindText(7, approval.status);
    stmt.BindInt64(8, approval.createdAt);
    stmt.Step();
    return stmt.Ok();
}

std::vector<Approval> ApprovalStore::ListUnposted() {
    std::vector<Approval> results;
    Statement stmt(
        db_,
        "SELECT a.id, a.chat_id, a.message_id, a.requested_by, a.kind, a.payload, a.status, "
        "a.created_at, a.resolved_at FROM approvals a JOIN messages m ON m.id = a.message_id "
        "WHERE a.status = 'pending' AND m.discord_message_id IS NULL;");
    if (!stmt.Valid()) {
        return results;
    }
    while (stmt.Step()) {
        Approval a;
        ReadRow(stmt, a);
        results.push_back(std::move(a));
    }
    return results;
}

bool ApprovalStore::GetByMessageId(int64_t messageId, Approval& outApproval) {
    Statement stmt(
        db_,
        "SELECT id, chat_id, message_id, requested_by, kind, payload, status, created_at, "
        "resolved_at FROM approvals WHERE message_id = ?1;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindInt64(1, messageId);
    if (!stmt.Step()) {
        return false;
    }
    ReadRow(stmt, outApproval);
    return true;
}

bool ApprovalStore::Resolve(const std::string& id, const std::string& status, int64_t resolvedAt) {
    Statement stmt(db_, "UPDATE approvals SET status = ?1, resolved_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) {
        return false;
    }
    stmt.BindText(1, status);
    stmt.BindInt64(2, resolvedAt);
    stmt.BindText(3, id);
    stmt.Step();
    return stmt.Ok();
}

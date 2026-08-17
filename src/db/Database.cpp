#include "Database.h"

Database::~Database() {
    Close();
}

bool Database::Open(const std::wstring& path) {
    Close();
    const int result = sqlite3_open16(path.c_str(), &db_);
    if (result != SQLITE_OK) {
        Close();
        return false;
    }
    // Enforce foreign keys and wait a bit under contention rather than
    // failing immediately with SQLITE_BUSY — this is a single-process,
    // low-concurrency database (the orchestrator is the only writer), but
    // the Discord gateway thread and turn-spawning logic can still touch
    // it from different threads.
    Exec("PRAGMA foreign_keys = ON;");
    sqlite3_busy_timeout(db_, 5000);
    return true;
}

void Database::Close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::Exec(const std::string& sql) {
    if (db_ == nullptr) {
        return false;
    }
    char* errMsg = nullptr;
    const int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (errMsg != nullptr) {
        sqlite3_free(errMsg);
    }
    return result == SQLITE_OK;
}

Statement::Statement(Database& db, const std::string& sql) : db_(db.Handle()) {
    if (db_ == nullptr) {
        ok_ = false;
        return;
    }
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        stmt_ = nullptr;
        ok_ = false;
    }
}

Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

void Statement::BindText(int index, const std::string& value) {
    if (stmt_ != nullptr) {
        sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
}

void Statement::BindInt64(int index, int64_t value) {
    if (stmt_ != nullptr) {
        sqlite3_bind_int64(stmt_, index, value);
    }
}

void Statement::BindNull(int index) {
    if (stmt_ != nullptr) {
        sqlite3_bind_null(stmt_, index);
    }
}

bool Statement::Step() {
    if (stmt_ == nullptr) {
        ok_ = false;
        return false;
    }
    const int result = sqlite3_step(stmt_);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result != SQLITE_DONE) {
        ok_ = false;
    }
    return false;
}

std::string Statement::ColumnText(int index) const {
    if (stmt_ == nullptr) {
        return "";
    }
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

int64_t Statement::ColumnInt64(int index) const {
    return stmt_ != nullptr ? sqlite3_column_int64(stmt_, index) : 0;
}

bool Statement::ColumnIsNull(int index) const {
    return stmt_ == nullptr || sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

int64_t Statement::LastInsertRowId() const {
    return db_ != nullptr ? sqlite3_last_insert_rowid(db_) : 0;
}

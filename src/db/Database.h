#pragma once

#include <cstdint>
#include <string>

#include "../third_party/sqlite3.h"

// Thin RAII wrapper over the vendored sqlite3 C API. Opens with
// sqlite3_open16 (accepts a UTF-16 path directly) since every path in this
// codebase is a std::wstring already.
class Database {
public:
    ~Database();

    bool Open(const std::wstring& path);
    void Close();

    // For DDL and statements with no bound parameters/result rows.
    bool Exec(const std::string& sql);

    sqlite3* Handle() const { return db_; }

    // Row count affected by the most recently completed INSERT/UPDATE/DELETE
    // on this connection — used for atomic "claim" UPDATEs (e.g. RepoStore::
    // TryClaimOnboarding), where a WHERE-guarded UPDATE's own affected-row
    // count is what tells the caller whether *this* call won the claim vs.
    // someone else already had.
    int Changes() const;

private:
    sqlite3* db_ = nullptr;
};

// RAII prepared-statement wrapper. Construct, bind parameters (1-indexed,
// matching sqlite3's own convention), then Step() in a loop for SELECTs or
// once for INSERT/UPDATE/DELETE.
class Statement {
public:
    Statement(Database& db, const std::string& sql);
    ~Statement();

    bool Valid() const { return stmt_ != nullptr; }

    void BindText(int index, const std::string& value);
    void BindInt64(int index, int64_t value);
    void BindNull(int index);

    // Advances to the next row. Returns true if a row is now available
    // (caller should read columns), false once done or on error — check
    // Ok() to distinguish "done" from "error".
    bool Step();
    bool Ok() const { return ok_; }

    std::string ColumnText(int index) const;
    int64_t ColumnInt64(int index) const;
    bool ColumnIsNull(int index) const;

    int64_t LastInsertRowId() const;

private:
    sqlite3_stmt* stmt_ = nullptr;
    sqlite3* db_ = nullptr;
    bool ok_ = true;
};

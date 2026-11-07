#pragma once

#include "px/core/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

// A small SQLite wrapper for the platform service. Errors inside a request
// throw DatabaseError, which the HTTP layer turns into "500 internal error"
// (and logs); they are never shown to users in detail.
namespace px::platform {

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Statement {
public:
    Statement(sqlite3* db, sqlite3_stmt* stmt) : m_db(db), m_stmt(stmt) {}
    ~Statement();
    Statement(Statement&& other) noexcept : m_db(other.m_db), m_stmt(other.m_stmt) { other.m_stmt = nullptr; }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& operator=(Statement&&) = delete;

    // Parameters are numbered from 1, as in SQL ("?1").
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value) { return bind(index, static_cast<std::int64_t>(value)); }
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, const char* value) { return bind(index, std::string_view(value)); }
    Statement& bind(int index, const std::string& value) { return bind(index, std::string_view(value)); }
    Statement& bindNull(int index);

    // Advances to the next row; false when there are no more.
    bool step();
    // Runs a statement that returns no rows.
    void run();

    [[nodiscard]] std::int64_t integer(int column) const;
    [[nodiscard]] double real(int column) const;
    [[nodiscard]] std::string text(int column) const;
    [[nodiscard]] bool isNull(int column) const;

private:
    void check(int rc, const char* what) const;
    sqlite3* m_db;
    sqlite3_stmt* m_stmt;
};

class Database {
public:
    [[nodiscard]] static Result<std::unique_ptr<Database>> open(const std::filesystem::path& file);
    // For looking only (a backup): nothing about the file changes.
    [[nodiscard]] static Result<std::unique_ptr<Database>> openReadOnly(const std::filesystem::path& file);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(std::string_view sql);
    [[nodiscard]] Statement prepare(std::string_view sql);
    [[nodiscard]] std::int64_t lastInsertId() const;
    [[nodiscard]] int changes() const;

    // A consistent copy in `file` (which must not exist) while the database
    // stays in use: a few pages at a time, holding `lock` (the one that
    // guards this connection) only while copying them. The copy has no
    // write-ahead log and passes SQLite's quick check.
    [[nodiscard]] Status backupTo(const std::filesystem::path& file, std::mutex& lock);
    // PRAGMA integrity_check: "ok", or what is wrong.
    [[nodiscard]] std::string integrityCheck();

    // BEGIN IMMEDIATE ... COMMIT, rolled back if not committed.
    class Transaction {
    public:
        explicit Transaction(Database& db);
        ~Transaction();
        void commit();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

    private:
        Database& m_db;
        bool m_done = false;
    };

private:
    explicit Database(sqlite3* db) : m_db(db) {}
    sqlite3* m_db;
};

} // namespace px::platform

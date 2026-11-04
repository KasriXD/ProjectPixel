#include "Database.hpp"

#include <sqlite3.h>

#include <format>

namespace px::platform {

Statement::~Statement() {
    if (m_stmt != nullptr) sqlite3_finalize(m_stmt);
}

void Statement::check(int rc, const char* what) const {
    if (rc != SQLITE_OK) throw DatabaseError(std::format("{}: {}", what, sqlite3_errmsg(m_db)));
}

Statement& Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(m_stmt, index, value), "bind");
    return *this;
}
Statement& Statement::bind(int index, double value) {
    check(sqlite3_bind_double(m_stmt, index, value), "bind");
    return *this;
}
Statement& Statement::bind(int index, std::string_view value) {
    check(sqlite3_bind_text64(m_stmt, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind");
    return *this;
}
Statement& Statement::bindNull(int index) {
    check(sqlite3_bind_null(m_stmt, index), "bind");
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DatabaseError(std::format("step: {} ({})", sqlite3_errmsg(m_db), sqlite3_sql(m_stmt)));
}

void Statement::run() {
    while (step()) {
    }
}

std::int64_t Statement::integer(int column) const { return sqlite3_column_int64(m_stmt, column); }
double Statement::real(int column) const { return sqlite3_column_double(m_stmt, column); }
std::string Statement::text(int column) const {
    const auto* p = sqlite3_column_text(m_stmt, column);
    const int n = sqlite3_column_bytes(m_stmt, column);
    return p == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
}
bool Statement::isNull(int column) const { return sqlite3_column_type(m_stmt, column) == SQLITE_NULL; }

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(file.string().c_str(), &db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db != nullptr ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        return makeError(ErrorCode::IoError, "Cannot open the database {}: {}", file.string(), message);
    }
    sqlite3_busy_timeout(db, 5000);
    auto out = std::unique_ptr<Database>(new Database(db));
    try {
        out->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
    } catch (const DatabaseError& e) {
        return makeError(ErrorCode::IoError, "Cannot set up the database: {}", e.what());
    }
    return out;
}

Database::~Database() { sqlite3_close(m_db); }

void Database::exec(std::string_view sql) {
    char* error = nullptr;
    const std::string text(sql);
    if (sqlite3_exec(m_db, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw DatabaseError(message);
    }
}

Statement Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseError(std::format("prepare: {} ({})", sqlite3_errmsg(m_db), sql));
    }
    return Statement(m_db, stmt);
}

std::int64_t Database::lastInsertId() const { return sqlite3_last_insert_rowid(m_db); }
int Database::changes() const { return sqlite3_changes(m_db); }

Database::Transaction::Transaction(Database& db) : m_db(db) { m_db.exec("BEGIN IMMEDIATE"); }
Database::Transaction::~Transaction() {
    if (!m_done) {
        try {
            m_db.exec("ROLLBACK");
        } catch (...) {
        }
    }
}
void Database::Transaction::commit() {
    m_db.exec("COMMIT");
    m_done = true;
}

} // namespace px::platform

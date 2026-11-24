#include "linux_store.h"
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <limits>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
namespace fsengine {
namespace {
void Check(sqlite3* db, int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void Exec(sqlite3* db, const char* sql) { Check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    Check(db, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
    return Statement(statement, sqlite3_finalize);
}
void Bind(sqlite3* db, sqlite3_stmt* statement, int column, const std::string& bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("catalog field too large");
    Check(db, sqlite3_bind_blob(statement, column, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT));
}
std::string Bytes(sqlite3_stmt* statement, int column) {
    const int size = sqlite3_column_bytes(statement, column);
    return size ? std::string(static_cast<const char*>(sqlite3_column_blob(statement, column)), size) : std::string{};
}
LinuxScanEntry ReadEntry(sqlite3_stmt* statement) {
    return {Bytes(statement, 0), Bytes(statement, 1),
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2)),
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3)),
        sqlite3_column_int(statement, 4) != 0, sqlite3_column_int(statement, 5) != 0,
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 6)), Bytes(statement, 7)};
}
}
LinuxStoreSnapshot::~LinuxStoreSnapshot() {
    if (statement_) sqlite3_finalize(statement_);
    if (db_) sqlite3_close(db_);
}
void LinuxStoreSnapshot::Advance() {
    const int code = sqlite3_step(statement_);
    Check(db_, code);
    if (code == SQLITE_DONE) pending_.reset();
    else pending_ = ReadEntry(statement_);
}
LinuxSnapshotPage LinuxStoreSnapshot::NextPage(std::size_t limit, std::size_t byte_budget) {
    if (!limit || limit > 1024 || !byte_budget) throw std::runtime_error("invalid snapshot page size");
    LinuxSnapshotPage page;
    std::size_t bytes = 0;
    while (pending_ && page.entries.size() < limit) {
        // Conservative JSON bound: display escaping plus raw-byte hex fields.
        const auto cost = 512 + 8 * (pending_->path.size() + pending_->name.size());
        if (cost > byte_budget - bytes) {
            if (page.entries.empty()) throw std::runtime_error("snapshot entry exceeds frame budget");
            break;
        }
        bytes += cost;
        page.entries.push_back(std::move(*pending_));
        Advance();
    }
    page.done = !pending_;
    return page;
}
std::unique_ptr<LinuxStoreSnapshot> LinuxStore::Snapshot(const std::string& scope, bool files,
    bool directories, std::string& error) const {
    if (!db_) { error = "catalog store not open"; return {}; }
    auto snapshot = std::unique_ptr<LinuxStoreSnapshot>(new LinuxStoreSnapshot);
    try {
        const char* path = sqlite3_db_filename(db_, "main");
        if (!path || !*path) throw std::runtime_error("snapshot requires file-backed catalog");
        const int opened = sqlite3_open_v2(path, &snapshot->db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        Check(snapshot->db_, opened);
        sqlite3_busy_timeout(snapshot->db_, 2000);
        Exec(snapshot->db_, "BEGIN");
        std::string sql = "SELECT path,name,inode,device,directory,symlink,entry_id,object_key FROM entries WHERE ";
        const bool scoped = !scope.empty() && scope != "/";
        sql += scoped ? "(path>=?1 AND path<?3 AND (path=?1 OR path>=?2))" : "1";
        if (!files && !directories) sql += " AND 0";
        else if (!files) sql += " AND directory=1";
        else if (!directories) sql += " AND directory=0";
        sql += " ORDER BY path";
        auto statement = Prepare(snapshot->db_, sql.c_str());
        if (scoped) {
            Bind(snapshot->db_, statement.get(), 1, scope);
            Bind(snapshot->db_, statement.get(), 2, scope + "/");
            Bind(snapshot->db_, statement.get(), 3, scope + "0");
        }
        snapshot->statement_ = statement.release();
        // The first read pins the MVCC snapshot while the catalog mutex still
        // excludes commits/event publication. BEGIN alone would not pin it.
        snapshot->Advance();
        return snapshot;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}
LinuxStore::~LinuxStore() { if (db_) sqlite3_close(db_); }
bool LinuxStore::Open(const std::string& path, std::string& error) {
    if (db_) { error = "catalog store already open"; return false; }
    corrupt_ = false;
    try {
        const int opened = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        Check(db_, opened);
        sqlite3_busy_timeout(db_, 2000);
        Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
            "CREATE TABLE IF NOT EXISTS entries(path BLOB PRIMARY KEY, name BLOB NOT NULL, inode INTEGER NOT NULL, device INTEGER NOT NULL, directory INTEGER NOT NULL, symlink INTEGER NOT NULL) WITHOUT ROWID;");
        bool has_id = false, has_key = false;
        {
            auto columns = Prepare(db_, "PRAGMA table_info(entries)");
            for (int code; (code = sqlite3_step(columns.get())) != SQLITE_DONE;) {
                Check(db_, code);
                has_id |= Bytes(columns.get(), 1) == "entry_id";
                has_key |= Bytes(columns.get(), 1) == "object_key";
            }
        }
        if (!has_id) Exec(db_, "ALTER TABLE entries ADD COLUMN entry_id INTEGER NOT NULL DEFAULT 0");
        if (!has_key) Exec(db_, "ALTER TABLE entries ADD COLUMN object_key BLOB NOT NULL DEFAULT X''");
        Exec(db_, "CREATE TABLE IF NOT EXISTS catalog_metadata(key TEXT PRIMARY KEY, value BLOB NOT NULL);"
            "CREATE TABLE IF NOT EXISTS allocator(key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
            "INSERT OR IGNORE INTO allocator SELECT 'next_id', COALESCE(MAX(entry_id),0)+1 FROM entries;"
            "CREATE UNIQUE INDEX IF NOT EXISTS unique_entry_id ON entries(entry_id) WHERE entry_id<>0;");
        auto check = Prepare(db_, "PRAGMA quick_check");
        Check(db_, sqlite3_step(check.get()));
        if (Bytes(check.get(), 0) != "ok") {
            corrupt_ = true;
            throw std::runtime_error("catalog integrity check failed");
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        if (db_) {
            const int code = sqlite3_errcode(db_) & 0xff;
            corrupt_ |= code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
        }
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
}
bool LinuxStore::OpenRecovering(const std::string& path, std::string& quarantine, std::string& error) {
    quarantine.clear();
    if (Open(path, error)) return true;
    if (!corrupt_) return false;
    std::vector<std::string> files;
    for (const auto& suffix : {"", "-wal", "-shm"}) {
        const auto file = path + suffix;
        struct stat state{};
        if (lstat(file.c_str(), &state) < 0) {
            if (errno == ENOENT) continue;
            error = std::strerror(errno); return false;
        }
        if (!S_ISREG(state.st_mode) || state.st_uid != geteuid() || state.st_nlink != 1) {
            error = "refusing to quarantine unsafe catalog file: " + file; return false;
        }
        files.push_back(file);
    }
    const auto parent = std::filesystem::path(path).parent_path();
    struct stat directory{};
    if (!parent.is_absolute() || lstat(parent.c_str(), &directory) < 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0022)) {
        error = "unsafe catalog recovery directory"; return false;
    }
    std::string pattern = path + ".corrupt-XXXXXX";
    char* created = mkdtemp(pattern.data());
    if (!created) { error = std::strerror(errno); return false; }
    quarantine = created;
    for (const auto& file : files) {
        const auto destination = std::filesystem::path(quarantine) / std::filesystem::path(file).filename();
        if (rename(file.c_str(), destination.c_str()) < 0) { error = std::strerror(errno); return false; }
    }
    // Persist the recoverable moves before creating a new derived catalog.
    for (const auto& directory_path : {parent.string(), quarantine}) {
        const int fd = open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { error = std::strerror(errno); return false; }
        const int result = fsync(fd), failure = errno;
        close(fd);
        if (result < 0) { error = std::strerror(failure); return false; }
    }
    error.clear();
    return Open(path, error);
}
bool LinuxStore::Replace(const std::vector<LinuxScanEntry>& entries, std::string& error, const std::string& root) { return Commit(entries, {}, true, error, root); }
bool LinuxStore::Apply(const std::vector<LinuxScanEntry>& entries, const std::vector<std::string>& deletes, std::string& error) { return Commit(entries, deletes, false, error); }
bool LinuxStore::Commit(const std::vector<LinuxScanEntry>& entries, const std::vector<std::string>& deletes, bool replace, std::string& error, const std::string& root) {
    if (!db_) { error = "catalog store not open"; return false; }
    try {
        Exec(db_, "BEGIN IMMEDIATE");
        if (replace) Exec(db_, "DELETE FROM entries");
        auto remove = Prepare(db_, "DELETE FROM entries WHERE path=?");
        for (const auto& path : deletes) {
            Bind(db_, remove.get(), 1, path);
            Check(db_, sqlite3_step(remove.get()));
            Check(db_, sqlite3_reset(remove.get()));
        }
        auto insert = Prepare(db_, "INSERT INTO entries(path,name,inode,device,directory,symlink,entry_id,object_key) VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(path) DO UPDATE SET name=excluded.name,inode=excluded.inode,device=excluded.device,"
            "directory=excluded.directory,symlink=excluded.symlink,entry_id=excluded.entry_id,object_key=excluded.object_key");
        std::uint64_t largest_id = 0;
        for (const auto& entry : entries) {
            if (entry.path.empty() || entry.path.front() != '/') throw std::runtime_error("catalog path must be absolute");
            if (entry.id >= static_cast<std::uint64_t>(INT64_MAX)) throw std::runtime_error("invalid catalog ID");
            largest_id = std::max(largest_id, entry.id);
            Bind(db_, insert.get(), 1, entry.path); Bind(db_, insert.get(), 2, entry.name);
            Check(db_, sqlite3_bind_int64(insert.get(), 3, static_cast<sqlite3_int64>(entry.inode)));
            Check(db_, sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(entry.device)));
            Check(db_, sqlite3_bind_int(insert.get(), 5, entry.directory));
            Check(db_, sqlite3_bind_int(insert.get(), 6, entry.symlink));
            Check(db_, sqlite3_bind_int64(insert.get(), 7, static_cast<sqlite3_int64>(entry.id)));
            Bind(db_, insert.get(), 8, entry.object_key);
            Check(db_, sqlite3_step(insert.get())); Check(db_, sqlite3_reset(insert.get()));
        }
        if (largest_id) {
            auto allocator = Prepare(db_, "UPDATE allocator SET value=MAX(value,?) WHERE key='next_id'");
            Check(db_, sqlite3_bind_int64(allocator.get(), 1, static_cast<sqlite3_int64>(largest_id + 1)));
            Check(db_, sqlite3_step(allocator.get()));
        }
        if (replace) {
            // Bind the root and completed catalog in the same transaction.
            // An empty/legacy binding must never authorize cached startup.
            auto metadata = Prepare(db_, "INSERT INTO catalog_metadata(key,value) VALUES('root',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
            Bind(db_, metadata.get(), 1, root);
            Check(db_, sqlite3_step(metadata.get()));
        }
        Exec(db_, "COMMIT"); return true;
    } catch (const std::exception& exception) {
        error = exception.what(); sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false;
    }
}
bool LinuxStore::CachedRoot(std::string& root, std::string& error) const {
    if (!db_) { error = "catalog store not open"; return false; }
    try {
        auto statement = Prepare(db_, "SELECT value FROM catalog_metadata WHERE key='root'");
        const int code = sqlite3_step(statement.get()); Check(db_, code);
        root = code == SQLITE_ROW ? Bytes(statement.get(), 0) : std::string{};
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool LinuxStore::Load(std::vector<LinuxScanEntry>& entries, std::string& error) const {
    if (!db_) { error = "catalog store not open"; return false; }
    try {
        std::vector<LinuxScanEntry> next;
        auto statement = Prepare(db_, "SELECT path,name,inode,device,directory,symlink,entry_id,object_key FROM entries ORDER BY path");
        for (;;) {
            int code = sqlite3_step(statement.get()); Check(db_, code);
            if (code == SQLITE_DONE) break;
            next.push_back(ReadEntry(statement.get()));
        }
        entries = std::move(next); return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool LinuxStore::ReserveIds(std::uint64_t count, std::uint64_t& first, std::string& error) {
    if (!db_) { error = "catalog store not open"; return false; }
    try {
        Exec(db_, "BEGIN IMMEDIATE");
        auto read = Prepare(db_, "SELECT value FROM allocator WHERE key='next_id'");
        if (sqlite3_step(read.get()) != SQLITE_ROW) throw std::runtime_error("missing ID allocator");
        const auto next = sqlite3_column_int64(read.get(), 0);
        if (next <= 0 || !count || count > static_cast<std::uint64_t>(INT64_MAX - next))
            throw std::runtime_error("catalog ID space exhausted");
        auto update = Prepare(db_, "UPDATE allocator SET value=? WHERE key='next_id'");
        Check(db_, sqlite3_bind_int64(update.get(), 1, next + static_cast<sqlite3_int64>(count)));
        Check(db_, sqlite3_step(update.get()));
        Exec(db_, "COMMIT"); first = static_cast<std::uint64_t>(next); return true;
    } catch (const std::exception& exception) {
        error = exception.what(); sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false;
    }
}
}

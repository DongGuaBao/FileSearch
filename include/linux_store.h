#pragma once
#include "linux_scanner.h"
#include <memory>
#include <optional>
struct sqlite3;
struct sqlite3_stmt;
namespace fsengine {
struct LinuxSnapshotPage {
    std::vector<LinuxScanEntry> entries;
    bool done = false;
};
// Independent SQLite read transaction: no full-catalog copy per subscriber.
class LinuxStoreSnapshot {
public:
    ~LinuxStoreSnapshot();
    LinuxStoreSnapshot(const LinuxStoreSnapshot&) = delete;
    LinuxStoreSnapshot& operator=(const LinuxStoreSnapshot&) = delete;
    LinuxSnapshotPage NextPage(std::size_t limit, std::size_t byte_budget = 512 * 1024);
private:
    friend class LinuxStore;
    LinuxStoreSnapshot() = default;
    void Advance();
    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::optional<LinuxScanEntry> pending_;
};
// Linux-only transactional catalog; Windows cache/WAL formats are untouched.
class LinuxStore {
public:
    ~LinuxStore();
    LinuxStore() = default;
    LinuxStore(const LinuxStore&) = delete;
    LinuxStore& operator=(const LinuxStore&) = delete;
    bool Open(const std::string& path, std::string& error);
    bool OpenRecovering(const std::string& path, std::string& quarantine, std::string& error);
    bool Replace(const std::vector<LinuxScanEntry>& entries, std::string& error, const std::string& root = {});
    bool CachedRoot(std::string& root, std::string& error) const;
    bool Apply(const std::vector<LinuxScanEntry>& upserts, const std::vector<std::string>& deletes, std::string& error);
    bool Load(std::vector<LinuxScanEntry>& entries, std::string& error) const;
    bool ReserveIds(std::uint64_t count, std::uint64_t& first, std::string& error);
    std::unique_ptr<LinuxStoreSnapshot> Snapshot(const std::string& scope, bool files,
        bool directories, std::string& error) const;
private:
    bool Commit(const std::vector<LinuxScanEntry>& entries, const std::vector<std::string>& deletes, bool replace, std::string& error, const std::string& root = {});
    sqlite3* db_ = nullptr;
    bool corrupt_ = false;
};
}

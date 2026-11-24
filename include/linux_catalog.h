#pragma once
#include "linux_fanotify.h"
#include "linux_scanner.h"
#include "linux_name_index.h"
#include "linux_store.h"
#include "notifier.h"
#include <map>
#include <set>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace fsengine {
enum class LinuxSearchPhase { ExactOnly, FuzzyOnly, ExactThenFuzzy };
class LinuxCatalogNotReady : public std::runtime_error {
public:
    LinuxCatalogNotReady() : std::runtime_error("catalog reconciliation in progress") {}
};
class LinuxCatalog {
public:
    // Deferred mode lets the service accept cache searches while its own
    // collector thread runs Pump. Default stays synchronous for embedders.
    bool Start(const std::string& root, std::string& error, const std::string& database = {}, bool deferred = false);
    bool Pump(std::string& error);
    bool Reconcile(std::string& error);
    std::unique_ptr<LinuxStoreSnapshot> Snapshot(const std::string& scope, bool files,
        bool directories, ChangePosition& position) const;
    std::vector<LinuxScanEntry> Search(const std::string& query, const std::string& scope, std::size_t limit,
        LinuxSearchPhase phase = LinuxSearchPhase::ExactOnly, bool* stale = nullptr) const;
    ChangePosition Position() const { return events_.CurrentPosition(); }
    FileChangePullResult Changes(ChangePosition cursor) const { return events_.PullSince(cursor); }
    bool CanResume(ChangePosition cursor) const { return Ready() && !events_.PullSince(cursor, 0).expired; }
    std::uint64_t OverflowCount() const { return overflow_count_.load(); }
    bool Ready() const { return !recovering_.load(); }
    bool SearchReady() const { return Ready() || startup_cache_.load(); }
    std::uint64_t ScanRetryCount() const { return scan_retry_count_.load(); }
    const std::string& RecoveredDatabase() const { return recovered_database_; }
    int EventDescriptor() const { return Ready() && watcher_ ? watcher_->Descriptor() : -1; }
    ChangePosition WaitForChanges(ChangePosition position, std::uint32_t timeout_ms) const {
        return events_.WaitForChange(position, timeout_ms);
    }
private:
    void Emit(const LinuxScanEntry& entry, bool remove);
    void EmitRename(const LinuxScanEntry& entry, const std::string& old_path, const std::string& new_path,
        const std::string& old_name, const std::string& new_name);
    bool ConfigureMounts(std::vector<LinuxMount> mounts, std::string& error);
    bool AssignIds(std::vector<LinuxScanEntry>& entries, std::string& error);
    void RequestReconcile();
    std::string root_;
    std::string root_object_key_;
    std::vector<LinuxMount> mounts_;
    std::vector<std::string> excluded_;
    std::unique_ptr<LinuxFanotify> watcher_;
    std::chrono::steady_clock::time_point mount_check_;
    std::chrono::steady_clock::time_point retry_at_;
    ChangeEventRing events_;
    std::atomic<std::uint64_t> overflow_count_{0};
    std::atomic<std::uint64_t> scan_retry_count_{0};
    std::atomic<bool> recovering_{true};
    std::atomic<bool> startup_cache_{false};
    mutable std::mutex mutex_;
    std::map<std::string, LinuxScanEntry> entries_;
    std::map<std::string, LinuxScanEntry> transient_;
    std::unordered_set<std::string> transient_directory_handles_;
    LinuxNameIndex name_index_;
    std::unique_ptr<LinuxStore> store_;
    std::string recovered_database_;
    std::uint64_t next_id_ = 1;
    std::uint64_t id_limit_ = 1;
    // A kernel object can be visible through several bind-mount paths.
    std::unordered_map<std::string, std::set<std::string>> directory_paths_;
};
}

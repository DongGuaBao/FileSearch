#include "linux_catalog.h"
#include "utf8.h"
#include <filesystem>
#include <fstream>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace fsengine {
namespace {
bool Within(const std::string& path, const std::string& root) {
    return root == "/" || path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
std::string MountSignature(const std::vector<LinuxMount>& mounts, const std::string& root) {
    std::string signature;
    for (const auto& mount : mounts) {
        if (!Within(mount.path, root) && !Within(root, mount.path)) continue;
        signature += std::to_string(mount.id) + ":" + mount.device + ":" + mount.root + ":" + mount.path + ":" + mount.filesystem + "\n";
    }
    return signature;
}
std::vector<std::string> EntryPaths(const std::map<std::string, LinuxScanEntry>& entries,
    const std::string& path, bool descendants) {
    std::vector<std::string> result;
    if (entries.count(path)) result.push_back(path);
    if (descendants) {
        const auto prefix = path == "/" ? path : path + "/";
        for (auto it = entries.lower_bound(prefix); it != entries.end() && it->first.starts_with(prefix); ++it)
            result.push_back(it->first);
    }
    return result;
}
std::uint64_t ViewMount(const std::vector<LinuxMount>& mounts, const std::string& path) {
    const LinuxMount* selected = nullptr;
    for (const auto& mount : mounts)
        if (Within(path, mount.path) && (!selected || mount.path.size() > selected->path.size())) selected = &mount;
    return selected ? selected->id : 0;
}
void MoveDirectoryPaths(std::unordered_map<std::string, std::set<std::string>>& directories,
    const std::string& old_path, const std::string& new_path = {}) {
    for (auto it = directories.begin(); it != directories.end();) {
        std::vector<std::string> moved;
        for (auto path = it->second.begin(); path != it->second.end();) {
            if (Within(*path, old_path)) {
                if (!new_path.empty()) moved.push_back(new_path + path->substr(old_path.size()));
                path = it->second.erase(path);
            } else if (!new_path.empty() && Within(*path, new_path)) path = it->second.erase(path);
            else ++path;
        }
        it->second.insert(moved.begin(), moved.end());
        if (it->second.empty()) it = directories.erase(it); else ++it;
    }
}
}
bool LinuxCatalog::ConfigureMounts(std::vector<LinuxMount> mounts, std::string& error) {
    if (!IsLinuxCatalogPath(mounts, root_)) { error = "root is excluded by mount policy"; return false; }
    mounts_ = std::move(mounts);
    return Reconcile(error);
}
bool LinuxCatalog::Start(const std::string& root, std::string& error, const std::string& database, bool deferred) {
    root_ = std::filesystem::path(root).lexically_normal().string();
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    std::string cached_root;
    if (!database.empty()) {
        const auto state_directory = std::filesystem::path(database).parent_path().lexically_normal().string();
        if (state_directory.empty() || state_directory.front() != '/' || Within(root_, state_directory)) {
            error = "state directory must be absolute and must not contain the search root"; return false;
        }
        excluded_.push_back(state_directory);
        store_ = std::make_unique<LinuxStore>();
        if (!store_->OpenRecovering(database, recovered_database_, error)) return false;
        if (!store_->CachedRoot(cached_root, error)) return false;
        std::vector<LinuxScanEntry> cached;
        if (!store_->Load(cached, error)) return false;
        for (auto& entry : cached) entries_[entry.path] = std::move(entry);
    }
    std::ifstream input("/proc/self/mountinfo");
    if (!input) { error = "cannot read mountinfo"; return false; }
    if (deferred) {
        mounts_ = ReadLinuxMounts(input);
        if (!IsLinuxCatalogPath(mounts_, root_)) { error = "root is excluded by mount policy"; return false; }
        // Never expose another root's cached entries or newly excluded mounts.
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!Within(it->first, root_) || !IsLinuxCatalogPath(mounts_, it->first) || IsLinuxExcludedPath(it->first, excluded_))
                it = entries_.erase(it);
            else { if (cached_root == root_) name_index_.Upsert(it->first, it->second.name); ++it; }
        }
        startup_cache_ = cached_root == root_;
        mount_check_ = std::chrono::steady_clock::now();
        retry_at_ = mount_check_;
        return true;
    }
    if (!ConfigureMounts(ReadLinuxMounts(input), error)) return false;
    mount_check_ = std::chrono::steady_clock::now();
    return Pump(error);
}
void LinuxCatalog::Emit(const LinuxScanEntry& entry, bool remove) {
    FileChangeEvent event;
    event.type = remove ? (entry.directory ? FsEventType::DirDeleted : FsEventType::FileDeleted)
                        : (entry.directory ? FsEventType::DirCreated : FsEventType::FileCreated);
    event.path = Utf8ToWide(entry.path);
    event.name = Utf8ToWide(entry.name);
    event.path_bytes = entry.path;
    event.name_bytes = entry.name;
    event.frn = entry.id;
    event.is_directory = entry.directory;
    events_.Push(std::move(event));
}
bool LinuxCatalog::Reconcile(std::string& error) {
    {
        std::lock_guard lock(mutex_);
        recovering_ = true;
        events_.Reset();
    }
    // A configured root may disappear between generations. Keep the durable
    // catalog unavailable until that path is a real directory again; never
    // silently switch to a symlink target or terminate on a transient absence.
    const int root_fd = open(root_.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR) { error = std::strerror(errno); return false; }
        ++scan_retry_count_;
        retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        error.clear();
        return true;
    }
    struct stat root_state{};
    const int root_stat_result = fstat(root_fd, &root_state);
    const int root_stat_error = errno;
    close(root_fd);
    if (root_stat_result < 0) { error = std::strerror(root_stat_error); return false; }
    if (!S_ISDIR(root_state.st_mode)) {
        ++scan_retry_count_;
        retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        error.clear();
        return true;
    }
    // Start a fresh queue BEFORE enumeration. Old queued records must not be
    // replayed against a new snapshot; changes during this scan remain queued.
    auto watcher = std::make_unique<LinuxFanotify>();
    if (!watcher->Open(root_, error)) {
        struct stat current{};
        if (lstat(root_.c_str(), &current) < 0 && (errno == ENOENT || errno == ENOTDIR)) {
            ++scan_retry_count_;
            retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            error.clear();
            return true;
        }
        return false;
    }
    for (const auto& mount : SelectLinuxCatalogMounts(mounts_)) {
        if (Within(mount.path, root_) && IsLinuxCatalogPath(mounts_, mount.path) &&
            !watcher->AddFilesystem(mount.path, error)) return false;
    }
    watcher_ = std::move(watcher);
    auto snapshot = ScanLinuxTree(root_, mounts_, excluded_, true);
    if (!snapshot.errors.empty()) { error = snapshot.errors.front(); return false; }
    if (snapshot.raced) {
        ++scan_retry_count_;
        retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        error.clear();
        return true;
    }
    if (!AssignIds(snapshot.entries, error)) return false;
    // Snapshot creation is gated by recovering_; already-open snapshots own
    // independent SQLite read transactions. Keep this expensive write off the
    // search mutex so startup-cache queries continue during persistence.
    if (store_ && !store_->Replace(snapshot.entries, error, root_)) return false;
    std::map<std::string, LinuxScanEntry> next;
    std::unordered_map<std::string, std::set<std::string>> directories;
    LinuxNameIndex index;
    directories[snapshot.root_object_key].insert(root_);
    for (auto& entry : snapshot.entries) {
        index.Upsert(entry.path, entry.name);
        if (entry.directory && !entry.object_key.empty()) directories[entry.object_key].insert(entry.path);
        next.emplace(entry.path, std::move(entry));
    }
    directories.erase("");
    {
        std::lock_guard lock(mutex_);
        entries_.swap(next);
        transient_.clear();
        transient_directory_handles_.clear();
        std::swap(name_index_, index);
        directory_paths_.swap(directories);
        root_object_key_ = std::move(snapshot.root_object_key);
        events_.Reset();
        startup_cache_ = false;
        recovering_ = false;
    }
    // Destroy the old index outside the search lock as well.
    return true;
}
void LinuxCatalog::RequestReconcile() {
    recovering_ = true;
    ++scan_retry_count_;
    retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    events_.Reset();
}
bool LinuxCatalog::AssignIds(std::vector<LinuxScanEntry>& entries, std::string& error) {
    for (auto& entry : entries) {
        const auto previous = entries_.find(entry.path);
        if (previous != entries_.end() && previous->second.inode == entry.inode &&
            previous->second.device == entry.device && previous->second.directory == entry.directory &&
            !entry.object_key.empty() && previous->second.object_key == entry.object_key)
            entry.id = previous->second.id;
        const auto transient = transient_.find(entry.path);
        if (!entry.id && transient != transient_.end() && !entry.object_key.empty() &&
            transient->second.object_key == entry.object_key) entry.id = transient->second.id;
        if (entry.id) continue;
        if (store_ && next_id_ == id_limit_) {
            if (!store_->ReserveIds(1024, next_id_, error)) return false;
            id_limit_ = next_id_ + 1024;
        }
        entry.id = next_id_++;
    }
    return true;
}
void LinuxCatalog::EmitRename(const LinuxScanEntry& entry, const std::string& old_path, const std::string& new_path,
    const std::string& old_name, const std::string& new_name) {
    FileChangeEvent event;
    event.type = entry.directory ? FsEventType::DirRenamed : FsEventType::FileRenamed;
    event.is_directory = entry.directory;
    event.old_path = Utf8ToWide(old_path); event.path = Utf8ToWide(new_path);
    event.old_name = Utf8ToWide(old_name); event.name = Utf8ToWide(new_name);
    event.old_path_bytes = old_path; event.path_bytes = new_path;
    event.old_name_bytes = old_name; event.name_bytes = new_name;
    event.frn = entry.id;
    events_.Push(std::move(event));
}
bool LinuxCatalog::Pump(std::string& error) {
    const auto now = std::chrono::steady_clock::now();
    if (now - mount_check_ >= std::chrono::seconds(1)) {
        mount_check_ = now;
        std::ifstream input("/proc/self/mountinfo");
        if (!input) { error = "cannot read mountinfo"; return false; }
        auto mounts = ReadLinuxMounts(input);
        if (MountSignature(mounts, root_) != MountSignature(mounts_, root_) &&
            !ConfigureMounts(std::move(mounts), error)) return false;
        // FAN_REPORT_DFID_NAME describes the root rename through its parent,
        // which is intentionally outside our indexed subtree. Periodically
        // fence the root's generation, not merely its pathname or inode.
        if (!recovering_ && LinuxFanotify::DirectoryHandle(root_) != root_object_key_) {
            std::lock_guard lock(mutex_);
            RequestReconcile();
        }
    }
    if (recovering_) {
        if (now >= retry_at_) return Reconcile(error);
        return true;
    }
    std::vector<LinuxNotification> notifications;
    if (!watcher_ || !watcher_->Read(notifications, error)) return false;
    for (const auto& received : notifications) {
        if (received.overflow) {
            ++overflow_count_;
            return Reconcile(error);
        }
        // Pair rename parents in the same visible mount, never a Cartesian
        // product of aliases. A view seeing only one endpoint gets move in/out.
        std::map<std::uint64_t, std::pair<std::string, std::string>> views;
        {
            std::lock_guard lock(mutex_);
            auto add = [&](const std::string& handle, bool old) {
                auto found = directory_paths_.find(handle);
                if (found == directory_paths_.end()) return;
                for (const auto& path : found->second) {
                    auto& view = views[ViewMount(mounts_, path)];
                    (old ? view.first : view.second) = path;
                }
            };
            add(received.directory_handle, false);
            if (received.mask & FAN_RENAME) add(received.old_directory_handle, true);
        }
        for (const auto& [mount, parents] : views) {
        auto notification = received;
        std::string parent_path = parents.second;
        if (notification.mask & FAN_RENAME) {
            std::lock_guard lock(mutex_);
            auto join = [](const std::string& parent, const std::string& name) {
                return parent == "/" ? parent + name : parent + "/" + name;
            };
            const std::string old_path = parents.first.empty() ? "" : join(parents.first, notification.old_name);
            const std::string new_path = parents.second.empty() ? "" : join(parents.second, notification.name);
            const bool old_inside = !old_path.empty() && Within(old_path, root_) && IsLinuxCatalogPath(mounts_, old_path) && !IsLinuxExcludedPath(old_path, excluded_);
            const bool new_inside = !new_path.empty() && Within(new_path, root_) && IsLinuxCatalogPath(mounts_, new_path) && !IsLinuxExcludedPath(new_path, excluded_);
            if (old_inside && new_inside) {
                auto source = entries_.find(old_path);
                if (source != entries_.end()) {
                if (!notification.object_handle.empty() && source->second.object_key != notification.object_handle) {
                    RequestReconcile(); return true;
                }
                const auto original = source->second;
                // rename(2) may replace an existing destination, including an empty directory.
                std::vector<LinuxScanEntry> moved;
                auto deletes = EntryPaths(entries_, old_path, original.directory);
                for (const auto& path : deletes) {
                    auto entry = entries_.at(path);
                    entry.path = new_path + path.substr(old_path.size());
                    if (path == old_path) entry.name = notification.name;
                    moved.push_back(std::move(entry));
                }
                auto destination = EntryPaths(entries_, new_path, original.directory);
                deletes.insert(deletes.end(), destination.begin(), destination.end());
                if (store_ && !store_->Apply(moved, deletes, error)) return false;
                for (const auto& path : deletes) {
                    name_index_.Erase(path);
                    entries_.erase(path);
                }
                if (original.directory) MoveDirectoryPaths(directory_paths_, old_path, new_path);
                for (auto& entry : moved) {
                    name_index_.Upsert(entry.path, entry.name);
                    entries_[entry.path] = std::move(entry);
                }
                EmitRename(original, old_path, new_path, notification.old_name, notification.name);
                continue;
                }
                auto transient = transient_.find(old_path);
                if (transient != transient_.end() && transient->second.object_key == notification.object_handle) {
                    auto entry = transient->second;
                    EmitRename(entry, old_path, new_path, notification.old_name, notification.name);
                    auto paths = EntryPaths(transient_, old_path, entry.directory);
                    std::vector<LinuxScanEntry> moved;
                    for (const auto& path : paths) {
                        auto child = transient_.at(path);
                        child.path = new_path + path.substr(old_path.size());
                        if (path == old_path) child.name = notification.name;
                        moved.push_back(std::move(child));
                        transient_.erase(path);
                    }
                    for (auto& child : moved) transient_[child.path] = std::move(child);
                    if (entry.directory) MoveDirectoryPaths(directory_paths_, old_path, new_path);
                }
                // The create may already have moved before lstat observed its
                // old name (especially during reconciliation). Materialize the
                // destination instead of silently dropping the entry.
            }
            if (old_inside && !new_inside) {
                notification.directory_handle = notification.old_directory_handle;
                parent_path = parents.first;
                notification.name = notification.old_name;
                notification.mask = FAN_MOVED_FROM | (notification.mask & FAN_ONDIR);
            } else if (new_inside) notification.mask = FAN_MOVED_TO | (notification.mask & FAN_ONDIR);
            else continue;
        }
        if (parent_path.empty()) continue;
        const std::string path = parent_path == "/" ? "/" + notification.name : parent_path + "/" + notification.name;
        if (!Within(path, root_) || !IsLinuxCatalogPath(mounts_, path) || IsLinuxExcludedPath(path, excluded_)) continue;
        if (notification.mask & (FAN_DELETE | FAN_MOVED_FROM)) {
            std::lock_guard lock(mutex_);
            auto existing = entries_.find(path);
            if (existing != entries_.end()) {
                if (!notification.object_handle.empty() && existing->second.object_key != notification.object_handle) {
                    RequestReconcile(); return true;
                }
                const bool directory = existing->second.directory;
                auto deletes = EntryPaths(entries_, path, directory);
                if (store_ && !store_->Apply({}, deletes, error)) return false;
                Emit(existing->second, true);
                for (const auto& removed : deletes) {
                    name_index_.Erase(removed);
                    entries_.erase(removed);
                }
                if (directory) {
                    MoveDirectoryPaths(directory_paths_, path);
                }
            } else {
                auto transient = transient_.find(path);
                if (transient != transient_.end()) {
                    Emit(transient->second, true);
                    if (transient->second.directory) transient_directory_handles_.insert(transient->second.object_key);
                    transient_.erase(transient);
                }
            }
        }
        if (notification.mask & (FAN_CREATE | FAN_MOVED_TO)) {
            const int object = open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
            if (object < 0) {
                if (errno == ENOTDIR || errno == ELOOP) { RequestReconcile(); return true; }
                if (errno != ENOENT) { error = path + ": " + std::strerror(errno); return false; }
                if (notification.object_handle.empty()) continue;
                std::lock_guard lock(mutex_);
                if (!transient_.count(path)) {
                    std::vector<LinuxScanEntry> temporary{{path, notification.name, 0, 0,
                        (notification.mask & FAN_ONDIR) != 0, false, 0, notification.object_handle}};
                    if (!AssignIds(temporary, error)) return false;
                    Emit(temporary.front(), false);
                    if (temporary.front().directory) directory_paths_[notification.object_handle].insert(path);
                    transient_[path] = std::move(temporary.front());
                }
                if (notification.mask & FAN_DELETE) {
                    // fanotify can merge a create and delete for the same
                    // entry. Preserve both logical transitions even when the
                    // entry has vanished before userspace handles the record.
                    Emit(transient_.at(path), true);
                    if (transient_.at(path).directory) transient_directory_handles_.insert(transient_.at(path).object_key);
                    transient_.erase(path);
                }
                continue;
            }
            struct CloseObject { int fd; ~CloseObject() { close(fd); } } object_guard{object};
            struct stat state{};
            if (fstat(object, &state) < 0) { error = path + ": " + std::strerror(errno); return false; }
            LinuxScanEntry entry{path, notification.name, static_cast<std::uint64_t>(state.st_ino),
                static_cast<std::uint64_t>(state.st_dev), S_ISDIR(state.st_mode), S_ISLNK(state.st_mode),
                0, LinuxFanotify::ObjectHandle(object)};
            if (!notification.object_handle.empty() && entry.object_key != notification.object_handle) {
                RequestReconcile(); return true;
            }
            std::vector<LinuxScanEntry> descendants;
            if (entry.directory) {
                auto scan = ScanLinuxTree(path, mounts_, excluded_, true);
                if (!scan.errors.empty()) { error = scan.errors.front(); return false; }
                if (scan.raced) { RequestReconcile(); return true; }
                descendants = std::move(scan.entries);
            }
            std::lock_guard lock(mutex_);
            auto upserts = descendants;
            upserts.push_back(entry);
            if (!AssignIds(upserts, error)) return false;
            entry = upserts.back();
            upserts.pop_back();
            descendants = std::move(upserts);
            if (store_) {
                auto durable = descendants;
                durable.push_back(entry);
                if (!store_->Apply(durable, {}, error)) return false;
            }
            entries_[path] = entry;
            name_index_.Upsert(path, entry.name);
            if (!transient_.count(path)) Emit(entry, false);
            transient_.erase(path);
            if (entry.directory) directory_paths_[entry.object_key].insert(path);
            for (auto& child : descendants) {
                name_index_.Upsert(child.path, child.name);
                if (child.directory) directory_paths_[child.object_key].insert(child.path);
                entries_[child.path] = std::move(child);
            }
        }
        if (notification.mask & (FAN_CLOSE_WRITE | FAN_ATTRIB)) {
            std::lock_guard lock(mutex_);
            auto existing = entries_.find(path);
            if (existing == entries_.end()) continue;
            FileChangeEvent event;
            event.type = FsEventType::FileModified;
            event.path = Utf8ToWide(path);
            event.name = Utf8ToWide(notification.name);
            event.path_bytes = path;
            event.name_bytes = notification.name;
            event.frn = existing->second.id;
            event.is_directory = existing->second.directory;
            events_.Push(std::move(event));
        }
        }
    }
    if (transient_.size() + transient_directory_handles_.size() > 65536) return Reconcile(error);
    return true;
}
std::vector<LinuxScanEntry> LinuxCatalog::Search(const std::string& query, const std::string& scope, std::size_t limit, LinuxSearchPhase phase, bool* stale) const {
    std::vector<LinuxScanEntry> result;
    std::string normalized_scope = scope.empty() ? root_ : std::filesystem::path(scope).lexically_normal().string();
    while (normalized_scope.size() > 1 && normalized_scope.back() == '/') normalized_scope.pop_back();
    std::lock_guard lock(mutex_);
    if (recovering_ && !startup_cache_) throw LinuxCatalogNotReady();
    if (stale) *stale = recovering_.load();
    if (query.empty() || !limit) return result;
    std::set<std::string> selected;
    if (phase != LinuxSearchPhase::FuzzyOnly) for (const auto& path : name_index_.Candidates(query, normalized_scope, limit)) {
        const auto& entry = entries_.at(path);
        if (Within(path, normalized_scope)) {
            result.push_back(entry);
            selected.insert(path);
            if (result.size() == limit) break;
        }
    }
    if (phase != LinuxSearchPhase::ExactOnly && result.size() < limit) {
        const auto distance = WideToLowerUtf8(Utf8ToWide(query)).size() <= 5 ? 1 : 2;
        for (const auto& [path, score] : name_index_.FuzzyCandidates(query, distance, normalized_scope, limit - result.size(), selected)) {
            if (selected.count(path) || !Within(path, normalized_scope)) continue;
            result.push_back(entries_.at(path));
            if (result.size() == limit) break;
        }
    }
    return result;
}
std::unique_ptr<LinuxStoreSnapshot> LinuxCatalog::Snapshot(const std::string& scope, bool files,
    bool directories, ChangePosition& position) const {
    std::lock_guard lock(mutex_);
    if (recovering_) throw LinuxCatalogNotReady();
    if (!store_) throw std::runtime_error("snapshot requires persistent catalog");
    std::string normalized_scope = scope.empty() ? root_ : std::filesystem::path(scope).lexically_normal().string();
    if (normalized_scope.empty() || normalized_scope.front() != '/') throw std::runtime_error("snapshot scope must be absolute");
    while (normalized_scope.size() > 1 && normalized_scope.back() == '/') normalized_scope.pop_back();
    std::string error;
    auto snapshot = store_->Snapshot(normalized_scope, files, directories, error);
    if (!snapshot) throw std::runtime_error(error);
    position = events_.CurrentPosition();
    return snapshot;
}
}

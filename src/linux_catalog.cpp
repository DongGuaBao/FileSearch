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
#include <algorithm>
#include <iostream>

namespace fsengine {
namespace {
bool Within(const std::string& path, const std::string& root) {
    return root == "/" || path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
int OpenIndexPath(const std::string& path) {
    int fd=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    for(const auto& part:std::filesystem::path(path).relative_path()) {
        const int next=openat(fd,part.c_str(),O_PATH|O_CLOEXEC|O_NOFOLLOW);
        const int failure=errno;close(fd);
        if(next<0){errno=failure;return -1;}
        fd=next;
    }
    return fd;
}
std::string SizeIdentity(const LinuxScanEntry& entry) {
    return entry.object_key.empty() ? std::to_string(entry.device) + ":" + std::to_string(entry.inode) : entry.object_key;
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
    { std::lock_guard lock(mutex_); mounts_ = std::move(mounts); }
    return Reconcile(error);
}
bool LinuxCatalog::Start(const std::string& root, std::string& error, const std::string& database, bool deferred) {
    root_ = std::filesystem::path(root).lexically_normal().string();
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    std::ifstream mount_input("/proc/self/mountinfo");
    if (!mount_input) { error="cannot read mountinfo"; return false; }
    mounts_=ReadLinuxMounts(mount_input);
    if (!IsLinuxCatalogPath(mounts_,root_)) { error="root is excluded by mount policy"; return false; }
    directory_sizes_.Reset(root_);
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
        // Page the durable catalog, publishing directory totals before the
        // expensive name index. No second all-files vector at startup.
        directory_sizes_.Reset(root_);
        auto cached = store_->Snapshot(root_, true, true, error);
        if (!cached) return false;
        const bool publish_partial = query_cache_ && !query_cache_->Read({root_})[0].value;
        std::size_t loaded = 0;
        for (;;) {
            if (scan_options_.cancelled && scan_options_.cancelled()) { error = "initialization cancelled"; return false; }
            auto page = cached->NextPage(1024);
            for (auto& entry : page.entries) {
                if (cached_root != root_ || !Within(entry.path,root_) || !IsLinuxCatalogPath(mounts_,entry.path) || IsLinuxExcludedPath(entry.path,excluded_)) continue;
                directory_sizes_.Add(entry);
                if (!entry.directory && !entry.symlink) size_aliases_[SizeIdentity(entry)].insert(entry.path);
                entries_.emplace(entry.path, std::move(entry));
            }
            loaded += page.entries.size();
            if (loaded / 100000 != (loaded - page.entries.size()) / 100000) std::cerr << "loading cached entries: " << loaded << std::endl;
            if (publish_partial) query_cache_->Write(directory_sizes_.TakeChanges(), false);
            if (page.done) break;
        }
        if (query_cache_ && cached_root == root_) {
            query_cache_->Write(directory_sizes_.All(), true, true);
        }
        std::cerr << "cached directory totals loaded: " << entries_.size() << " entries; building search index" << std::endl;
    }
    if (deferred) {
        for (const auto& [path, entry] : entries_) {
            if (scan_options_.cancelled && scan_options_.cancelled()) { error="initialization cancelled"; return false; }
            name_index_.Upsert(path,entry.name);
        }
        directory_sizes_.TakeChanges();
        size_cache_ready_ = cached_root == root_;
        startup_cache_ = cached_root == root_;
        mount_check_ = std::chrono::steady_clock::now();
        retry_at_ = mount_check_;
        return true;
    }
    if (!ConfigureMounts(mounts_, error)) return false;
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
    const auto reconcile_started=std::chrono::steady_clock::now();
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
    watcher->SetIgnoredProcess(ignored_process_);
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
    // Publish a bounded batch of provisional totals while the first full
    // namespace baseline is still scanning. Never label these totals complete.
    auto options = scan_options_;
    bool provisional = false;
    {
        std::lock_guard lock(mutex_);
        provisional = !size_cache_ready_;
        if (provisional) { directory_sizes_.Reset(root_); provisional_sizes_ = true; if (query_cache_) query_cache_->Clear(); }
    }
    std::cerr << "directory scan started: " << root_ << std::endl;
    std::size_t scanned = 0;
    std::vector<LinuxScanEntry> size_batch;
    auto flush_sizes = [&] {
        if (size_batch.empty()) return;
        std::lock_guard lock(mutex_);
        for (const auto& entry : size_batch) directory_sizes_.Add(entry);
        if (query_cache_) query_cache_->Write(directory_sizes_.TakeChanges(), false);
        size_batch.clear();
    };
    if (provisional) options.observed = [&](const LinuxScanEntry& entry) {
        if (++scanned % 100000 == 0) std::cerr << "directory scan entries: " << scanned << std::endl;
        size_batch.push_back(entry);
        if (size_batch.size() >= 128) flush_sizes();
    };
    auto snapshot = ScanLinuxTree(root_, mounts_, excluded_, true, options);
    flush_sizes();
    std::cerr << "directory scan finished: entries=" << snapshot.entries.size() << " raced=" << snapshot.raced << " errors=" << snapshot.errors.size() << std::endl;
    if (snapshot.cancelled) { error.clear(); return true; }
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
    if(query_cache_) {
        const auto count=snapshot.entries.size();
        if(!PublishSnapshot(snapshot.entries,snapshot.root_object_key,error))return false;
        std::cerr << "reconciliation complete: " << count << " entries in " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-reconcile_started).count() << "ms" << std::endl;
        return true;
    }
    const auto reconciled_count=snapshot.entries.size();
    std::map<std::string, LinuxScanEntry> next;
    std::unordered_map<std::string, std::set<std::string>> directories;
    LinuxNameIndex index;
    LinuxDirectorySizes sizes; sizes.Reset(root_);
    std::unordered_map<std::string, std::set<std::string>> size_aliases;
    directories[snapshot.root_object_key].insert(root_);
    for (auto& entry : snapshot.entries) {
        sizes.Add(entry);
        if (!entry.directory && !entry.symlink) size_aliases[SizeIdentity(entry)].insert(entry.path);
        index.Upsert(entry.path, entry.name);
        if (entry.directory && !entry.object_key.empty()) directories[entry.object_key].insert(entry.path);
        next.emplace(entry.path, std::move(entry));
    }
    std::vector<LinuxScanEntry>().swap(snapshot.entries);
    directories.erase("");
    if (query_cache_) { query_cache_->Write(sizes.All(), true, true); }
    sizes.TakeChanges();
    {
        std::lock_guard lock(mutex_);
        entries_.swap(next);
        std::swap(directory_sizes_, sizes);
        size_cache_ready_ = true;
        size_aliases_.swap(size_aliases);
        dirty_size_paths_.clear();
        repairs_.clear(); scanning_size_paths_.clear();
        size_cursor_.clear();
        last_reconcile_ = std::chrono::steady_clock::now();
        transient_.clear();
        transient_directory_handles_.clear();
        std::swap(name_index_, index);
        directory_paths_.swap(directories);
        root_object_key_ = std::move(snapshot.root_object_key);
        events_.Reset();
        startup_cache_ = false;
        recovering_ = false;
        rebuilding_index_ = false;
    }
    std::cerr << "reconciliation complete: " << reconciled_count << " entries in " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-reconcile_started).count() << "ms" << std::endl;
    // Destroy the old index outside the search lock as well.
    return true;
}
bool LinuxCatalog::PublishSnapshot(std::vector<LinuxScanEntry>& fresh,const std::string& root_key,std::string& error) {
    (void)error;
    std::sort(fresh.begin(),fresh.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    rebuilding_index_=true;
    std::vector<std::pair<std::string,std::optional<LinuxDirectorySize>>> cached;
    std::size_t added=0,removed=0,changed=0;
    {
        std::lock_guard lock(mutex_);
        startup_cache_=false;
        if(!size_cache_ready_)directory_sizes_.Reset(root_);
        auto erase=[&](const std::string& path){
            const auto& entry=entries_.at(path);
            if(entry.directory&&!entry.object_key.empty()){
                auto binding=directory_paths_.find(entry.object_key);
                if(binding!=directory_paths_.end()){binding->second.erase(path);if(binding->second.empty())directory_paths_.erase(binding);}
            }
            name_index_.Erase(path);EraseSizeEntry(path);++removed;
        };
        auto old=entries_.begin();
        for(auto& entry:fresh){
            while(old!=entries_.end()&&old->first<entry.path){auto path=old->first;++old;erase(path);}
            if(old==entries_.end()||entry.path<old->first){
                name_index_.Upsert(entry.path,entry.name);
                if(entry.directory&&!entry.object_key.empty())directory_paths_[entry.object_key].insert(entry.path);
                PutSizeEntry(std::move(entry));++added;continue;
            }
            const auto& before=old->second;
            if(before.name!=entry.name)name_index_.Upsert(entry.path,entry.name);
            const bool identity=before.id==entry.id&&SizeIdentity(before)==SizeIdentity(entry)&&before.directory==entry.directory&&before.symlink==entry.symlink;
            if(before.directory&&(!entry.directory||before.object_key!=entry.object_key)){
                auto binding=directory_paths_.find(before.object_key);
                if(binding!=directory_paths_.end()){binding->second.erase(entry.path);if(binding->second.empty())directory_paths_.erase(binding);}
            }
            if(entry.directory&&!entry.object_key.empty())directory_paths_[entry.object_key].insert(entry.path);
            const bool size_changed=before.logical_size!=entry.logical_size||before.allocated_size!=entry.allocated_size||before.metadata_known!=entry.metadata_known||before.regular!=entry.regular;
            ++old;
            if(!identity||size_changed){PutSizeEntry(std::move(entry));++changed;}
            else { auto target=entries_.find(entry.path);target->second=std::move(entry); }
        }
        while(old!=entries_.end()){auto path=old->first;++old;erase(path);}
        // Deferred startup did not have a live root handle binding yet.
        if(!root_object_key_.empty()&&root_object_key_!=root_key){
            auto previous=directory_paths_.find(root_object_key_);
            if(previous!=directory_paths_.end()){previous->second.erase(root_);if(previous->second.empty())directory_paths_.erase(previous);}
        }
        if(!root_key.empty())directory_paths_[root_key].insert(root_);
        directory_sizes_.MarkValidated(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        cached=directory_sizes_.All();directory_sizes_.TakeChanges();
    }
    // Free the large scan vector before writing the small directory cache.
    std::vector<LinuxScanEntry>().swap(fresh);
    if(query_cache_)query_cache_->Write(cached,true,true);
    {
        std::lock_guard lock(mutex_);
        size_cache_ready_=true;dirty_size_paths_.clear();repairs_.clear();scanning_size_paths_.clear();size_cursor_.clear();
        transient_.clear();transient_directory_handles_.clear();root_object_key_=root_key;
        last_reconcile_=std::chrono::steady_clock::now();events_.Reset();startup_cache_=false;recovering_=false;rebuilding_index_=false;
    }
    std::cerr<<"namespace delta applied: added="<<added<<" removed="<<removed<<" metadata="<<changed<<std::endl;
    return true;
}
void LinuxCatalog::RequestReconcile(std::source_location caller) {
    std::cerr << "reconciliation requested: " << caller.function_name() << ":" << caller.line() << std::endl;
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
        if (MountSignature(mounts, root_) != MountSignature(mounts_, root_)) std::cerr << "mount topology changed; reconciling" << std::endl;
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
    if (!recovering_ && reconcile_seconds_ && now - last_reconcile_ >= std::chrono::seconds(reconcile_seconds_)) {
        std::lock_guard lock(mutex_); RequestReconcile();
    }
    if (recovering_) {
        if (now >= retry_at_) return Reconcile(error);
        return true;
    }
    std::vector<LinuxNotification> notifications;
    if (!watcher_ || !watcher_->Read(notifications, error)) return false;
    return ApplyNotifications(notifications, error);
}
bool LinuxCatalog::ApplyNotifications(const std::vector<LinuxNotification>& notifications, std::string& error) {
    for (const auto& received : notifications) {
        if (received.overflow) {
            ++overflow_count_;
            std::cerr << "fanotify " << (received.userspace_overflow ? "userspace buffer" : "kernel queue") << " overflow; restarting reconciliation" << std::endl;
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
            if(std::any_of(repairs_.begin(),repairs_.end(),[&](const auto& repair){return (old_inside&&(Within(old_path,repair.first)||Within(repair.first,old_path)))||(new_inside&&(Within(new_path,repair.first)||Within(repair.first,new_path)));})) {
                if(old_inside)QueueRepairLocked(old_path);
                if(new_inside)QueueRepairLocked(new_path);
                continue;
            }
            if (old_inside && new_inside) {
                auto source = entries_.find(old_path);
                if (source != entries_.end()) {
                if (!notification.object_handle.empty() && source->second.object_key != notification.object_handle) {
                    QueueRepairLocked(old_path); QueueRepairLocked(new_path); continue;
                }
                const auto original = source->second;
                // rename(2) may replace an existing destination, including an empty directory.
                std::vector<LinuxScanEntry> moved;
                auto deletes = EntryPaths(entries_, old_path, original.directory);
                std::vector<std::string> moved_dirty;
                for (const auto& path : deletes) {
                    if (dirty_size_paths_.count(path) || !original.directory)
                        moved_dirty.push_back(new_path + path.substr(old_path.size()));
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
                    EraseSizeEntry(path);
                }
                if (original.directory) MoveDirectoryPaths(directory_paths_, old_path, new_path);
                for (auto& entry : moved) {
                    name_index_.Upsert(entry.path, entry.name);
                    PutSizeEntry(std::move(entry));
                }
                dirty_size_paths_.insert(moved_dirty.begin(), moved_dirty.end());
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
        if (notification.mask & (FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO)) {
            std::lock_guard lock(mutex_);
            if(std::any_of(repairs_.begin(),repairs_.end(),[&](const auto& repair){return Within(path,repair.first)||Within(repair.first,path);})) {
                QueueRepairLocked(path);continue;
            }
        }
        if (notification.mask & (FAN_DELETE | FAN_MOVED_FROM)) {
            std::lock_guard lock(mutex_);
            auto existing = entries_.find(path);
            if (existing != entries_.end()) {
                if (!notification.object_handle.empty() && existing->second.object_key != notification.object_handle) {
                    QueueRepairLocked(path); continue;
                }
                const bool directory = existing->second.directory;
                auto deletes = EntryPaths(entries_, path, directory);
                if (store_ && !store_->Apply({}, deletes, error)) return false;
                Emit(existing->second, true);
                for (const auto& removed : deletes) {
                    name_index_.Erase(removed);
                    EraseSizeEntry(removed);
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
            const int object = OpenIndexPath(path);
            if (object < 0) {
                if (errno == ENOTDIR || errno == ELOOP) { std::lock_guard lock(mutex_); QueueRepairLocked(path); continue; }
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
            UpdateLinuxMetadata(entry, state);
            {
                std::lock_guard lock(mutex_);
                const auto previous=entries_.find(path);
                if(previous!=entries_.end() && !entry.object_key.empty() && previous->second.object_key==entry.object_key && previous->second.directory==entry.directory) {
                    const auto& old=previous->second;
                    // Enumeration may already contain this generation. Do not
                    // replay an old create as a redundant subtree scan/write.
                    if(entry.directory || (old.metadata_known==entry.metadata_known && old.logical_size==entry.logical_size && old.allocated_size==entry.allocated_size && old.modified_ns==entry.modified_ns))continue;
                }
            }
            if (!notification.object_handle.empty() && entry.object_key != notification.object_handle) {
                std::lock_guard lock(mutex_); QueueRepairLocked(path); continue;
            }
            std::vector<LinuxScanEntry> descendants;
            if (entry.directory) {
                { std::lock_guard lock(mutex_); scanning_size_paths_.insert(path); }
                auto scan = ScanLinuxTree(path, mounts_, excluded_, true, scan_options_);
                { std::lock_guard lock(mutex_); scanning_size_paths_.erase(path); }
                if (scan.cancelled) { error.clear(); return true; }
                if (!scan.errors.empty()) { error = scan.errors.front(); return false; }
                if (scan.raced) { std::lock_guard lock(mutex_); QueueRepairLocked(path, true); continue; }
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
            PutSizeEntry(entry);
            name_index_.Upsert(path, entry.name);
            if (!transient_.count(path)) Emit(entry, false);
            transient_.erase(path);
            if (entry.directory) directory_paths_[entry.object_key].insert(path);
            for (auto& child : descendants) {
                name_index_.Upsert(child.path, child.name);
                if (child.directory) directory_paths_[child.object_key].insert(child.path);
                PutSizeEntry(std::move(child));
            }
        }
        if (notification.mask & (FAN_MODIFY | FAN_CLOSE_WRITE | FAN_ATTRIB)) {
            std::lock_guard lock(mutex_);
            auto existing = entries_.find(path);
            if (existing == entries_.end()) continue;
            dirty_size_paths_.insert(path);
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
    if (transient_.size() + transient_directory_handles_.size() > 65536) {
        std::cerr << "unresolved namespace history exceeds buffer; reconciling" << std::endl;
        return Reconcile(error);
    }
    if (!RepairPending(error)) return false;
    if (!RefreshSizeMetadata(error)) return false;
    if (query_cache_) {
        std::lock_guard lock(mutex_);
        query_cache_->Write(directory_sizes_.TakeChanges(), true);
    }
    return true;
}
void LinuxCatalog::QueueRepairLocked(const std::string& path, bool subtree) {
    for(auto& [scope,state]:repairs_) {
        if(Within(path,scope)){state.subtree|=subtree||path!=scope;return;}
    }
    for(auto it=repairs_.begin();it!=repairs_.end();) {
        if(Within(it->first,path)){subtree=true;scanning_size_paths_.erase(it->first);it=repairs_.erase(it);}else ++it;
    }
    repairs_.emplace(path,RepairState{{},0,subtree});
    scanning_size_paths_.insert(path);
}
bool LinuxCatalog::RepairPending(std::string& error) {
    for(unsigned turn=0;turn<32;++turn){
        std::string path;bool subtree=false;
        {
            std::lock_guard lock(mutex_);
            const auto now=std::chrono::steady_clock::now();
            auto start=repairs_.upper_bound(repair_cursor_);
            for(std::size_t checked=0;checked<repairs_.size();++checked){
                if(start==repairs_.end())start=repairs_.begin();
                if(start->second.due<=now){path=start->first;subtree=start->second.subtree;break;}++start;
            }
            if(path.empty())return true;
            repair_cursor_=path;
        }
        std::string failure;
        if(!RepairPath(path,subtree,failure)){
            std::lock_guard lock(mutex_);
            auto found=repairs_.find(path);if(found==repairs_.end())continue;
            auto& state=found->second;
            state.due=std::chrono::steady_clock::now()+std::chrono::milliseconds(250U<<std::min(state.attempts++,7U));
            if(state.attempts==1||state.attempts%10==0)std::cerr<<"subtree repair deferred: "<<path<<": "<<failure<<std::endl;
            if(!failure.starts_with("retry:")){error=failure;return false;}
        } else {
            std::lock_guard lock(mutex_);repairs_.erase(path);scanning_size_paths_.erase(path);
        }
    }
    return true;
}
bool LinuxCatalog::RepairPath(const std::string& path, bool subtree, std::string& error) {
    const int object=OpenIndexPath(path);
    const int failure=errno;
    struct Guard{int fd;~Guard(){if(fd>=0)close(fd);}} guard{object};
    if(object<0 && failure!=ENOENT && failure!=ENOTDIR && failure!=ELOOP){error="retry: "+path+": "+std::strerror(failure);return false;}
    std::vector<LinuxScanEntry> current;
    if(object>=0){
        struct stat state{};
        if(fstat(object,&state)<0){error="retry: "+std::string(std::strerror(errno));return false;}
        LinuxScanEntry entry{path,std::filesystem::path(path).filename().string(),static_cast<std::uint64_t>(state.st_ino),static_cast<std::uint64_t>(state.st_dev),S_ISDIR(state.st_mode),S_ISLNK(state.st_mode),0,LinuxFanotify::ObjectHandle(object)};
        UpdateLinuxMetadata(entry,state);
        if(entry.directory){
            {
                std::lock_guard lock(mutex_);
                auto previous=entries_.find(path);
                if(!subtree && previous!=entries_.end() && previous->second.directory && !entry.object_key.empty() && previous->second.object_key==entry.object_key)return true;
            }
            auto scan=ScanLinuxTree(path,mounts_,excluded_,true,scan_options_);
            if(scan.cancelled){error="retry: scan cancelled";return false;}
            if(scan.raced||!scan.errors.empty()||scan.root_object_key!=entry.object_key){error="retry: directory changed during local scan";return false;}
            current=std::move(scan.entries);
        }
        current.push_back(std::move(entry));
    }
    std::lock_guard lock(mutex_);
    if(!AssignIds(current,error))return false;
    auto removed=EntryPaths(entries_,path,true);
    std::unordered_map<std::string_view,std::uint64_t> next_ids;
    std::vector<bool> created;created.reserve(current.size());
    for(const auto& entry:current){
        next_ids.emplace(entry.path,entry.id);
        auto previous=entries_.find(entry.path);
        created.push_back(previous==entries_.end()||previous->second.id!=entry.id);
    }
    if(store_&&!store_->Apply(current,removed,error))return false;
    for(const auto& previous:removed){
        const auto& entry=entries_.at(previous);auto next=next_ids.find(previous);
        if(next==next_ids.end()||next->second!=entry.id)Emit(entry,true);
        name_index_.Erase(previous);EraseSizeEntry(previous);
    }
    MoveDirectoryPaths(directory_paths_,path);
    for(std::size_t index=0;index<current.size();++index){
        auto& entry=current[index];name_index_.Upsert(entry.path,entry.name);
        if(entry.directory&&!entry.object_key.empty())directory_paths_[entry.object_key].insert(entry.path);
        if(created[index])Emit(entry,false);
        transient_.erase(entry.path);
        PutSizeEntry(std::move(entry));
    }
    return true;
}
void LinuxCatalog::EraseSizeEntry(const std::string& path) {
    auto found = entries_.find(path);
    if (found != entries_.end()) {
        directory_sizes_.Remove(found->second);
        if (!found->second.directory && !found->second.symlink) {
            auto aliases = size_aliases_.find(SizeIdentity(found->second));
            if (aliases != size_aliases_.end()) { aliases->second.erase(path); if (aliases->second.empty()) size_aliases_.erase(aliases); }
        }
        dirty_size_paths_.erase(path);
        entries_.erase(found);
    }
    dirty_size_paths_.erase(path);
}
void LinuxCatalog::PutSizeEntry(LinuxScanEntry entry) {
    auto found = entries_.find(entry.path);
    if (found != entries_.end()) EraseSizeEntry(entry.path);
    directory_sizes_.Add(entry);
    if (!entry.directory && !entry.symlink) size_aliases_[SizeIdentity(entry)].insert(entry.path);
    entries_[entry.path] = std::move(entry);
}
bool LinuxCatalog::RefreshSizeMetadata(std::string& error) {
    const auto now = std::chrono::steady_clock::now();
    if (now - size_refresh_ < std::chrono::milliseconds(500)) return true;
    size_refresh_ = now;
    std::map<std::string, LinuxScanEntry> candidates;
    {
        std::lock_guard lock(mutex_);
        const std::size_t dirty_limit=dirty_size_paths_.size()>4096?512:64;
        for (auto it=dirty_size_paths_.begin();it!=dirty_size_paths_.end();) {
            auto found=entries_.find(*it);
            if(found==entries_.end()){it=dirty_size_paths_.erase(it);continue;}
            candidates.emplace(*found);++it;
            if(candidates.size()>=dirty_limit)break;
        }
        for (auto it = priority_sizes_.begin(); it != priority_sizes_.end();) {
            if (now - it->second.second > std::chrono::seconds(20)) it = priority_sizes_.erase(it); else ++it;
        }
        for (unsigned turn = 0; turn < 8 && !priority_sizes_.empty() && candidates.size() < dirty_limit+48; ++turn) {
            auto scope = priority_sizes_.upper_bound(priority_size_cursor_);
            if (scope == priority_sizes_.end()) scope = priority_sizes_.begin();
            priority_size_cursor_ = scope->first;
            const auto prefix = scope->first == "/" ? "/" : scope->first + "/";
            auto& cursor = scope->second.first;
            auto entry = cursor.empty() ? entries_.lower_bound(prefix) : entries_.upper_bound(cursor);
            unsigned count = 0;
            for (unsigned step = 0; entry != entries_.end() && entry->first.starts_with(prefix) && step < 128 && count < 6; ++step, ++entry) {
                cursor = entry->first;
                if (!entry->second.directory && !entry->second.symlink) { candidates.emplace(*entry); ++count; }
            }
            if (entry == entries_.end() || !entry->first.starts_with(prefix)) cursor.clear();
        }
        auto entry = entries_.upper_bound(size_cursor_);
        if (entry == entries_.end()) { size_cursor_.clear(); entry = entries_.begin(); }
        for (unsigned step = 0; entry != entries_.end() && step < 64 && candidates.size() < dirty_limit+64; ++step, ++entry) {
            size_cursor_ = entry->first;
            if (!entry->second.directory && !entry->second.symlink) candidates.emplace(*entry);
        }
    }
    std::vector<LinuxScanEntry> observed, changed;
    for (const auto& [path, previous] : candidates) {
        if (scan_options_.cancelled && scan_options_.cancelled()) return true;
        auto next = previous;
        const int fd = OpenIndexPath(path);
        struct stat state{};
        if (fd >= 0 && fstat(fd, &state) == 0) {
            if (static_cast<std::uint64_t>(state.st_ino) != previous.inode ||
                static_cast<std::uint64_t>(state.st_dev) != previous.device ||
                (!previous.object_key.empty() && LinuxFanotify::ObjectHandle(fd) != previous.object_key)) {
                close(fd); std::lock_guard lock(mutex_); QueueRepairLocked(path); continue;
            }
            UpdateLinuxMetadata(next, state);
        } else {
            if(errno==ENOENT||errno==ENOTDIR||errno==ELOOP){
                if(fd>=0)close(fd);
                std::lock_guard lock(mutex_);QueueRepairLocked(path);continue;
            }
            next.metadata_known = false; next.logical_size = next.allocated_size = 0;
        }
        if (fd >= 0) close(fd);
        observed.push_back(std::move(next));
    }
    std::lock_guard lock(mutex_);
    // A write through one hardlink changes every indexed alias of that inode.
    std::map<std::string, LinuxScanEntry> updates;
    for (const auto& entry : observed) {
        auto aliases = size_aliases_.find(SizeIdentity(entry));
        if (entry.metadata_known && aliases != size_aliases_.end()) {
            for (const auto& path : aliases->second) {
                auto alias = entries_.at(path);
                alias.logical_size = entry.logical_size; alias.allocated_size = entry.allocated_size;
                alias.modified_ns = entry.modified_ns; alias.checked_at_ms = entry.checked_at_ms;
                alias.regular = entry.regular; alias.metadata_known = true;
                updates[path] = std::move(alias);
            }
        } else updates[entry.path] = entry;
    }
    changed.clear();
    for (const auto& [path, entry] : updates) {
        const auto& old = entries_.at(path);
        if (entry.metadata_known != old.metadata_known || entry.logical_size != old.logical_size ||
            entry.allocated_size != old.allocated_size || entry.modified_ns != old.modified_ns || entry.regular != old.regular)
            changed.push_back(entry);
    }
    if (store_ && !changed.empty() && !store_->Apply(changed, {}, error)) return false;
    for (auto& [path, entry] : updates) { dirty_size_paths_.erase(path); PutSizeEntry(std::move(entry)); }
    return true;
}
std::vector<LinuxCatalog::DirectoryResult> LinuxCatalog::DirectorySizes(const std::vector<std::string>& paths) {
    if (paths.size() > 128) throw std::runtime_error("at most 128 directory paths per request");
    std::lock_guard lock(mutex_);
    std::vector<DirectoryResult> result;
    for (const auto& requested : paths) {
        if (requested.empty() || requested.front() != '/' || requested.find('\0') != std::string::npos)
            throw std::runtime_error("directory path must be absolute");
        auto path = std::filesystem::path(requested).lexically_normal().string();
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (!Within(path, root_) || !IsLinuxCatalogPath(mounts_, path) || IsLinuxExcludedPath(path, excluded_)) {
            result.push_back({path, {}, "excluded"}); continue;
        }
        const auto size = size_cache_ready_ || provisional_sizes_ ? directory_sizes_.Get(path) : std::optional<LinuxDirectorySize>{};
        const auto prefix = path == "/" ? "/" : path + "/";
        auto dirty = dirty_size_paths_.lower_bound(prefix);
        const bool scanning = std::any_of(scanning_size_paths_.begin(), scanning_size_paths_.end(), [&](const auto& scope) { return Within(path, scope) || Within(scope, path); });
        const bool pending = scanning || dirty_size_paths_.count(path) || (dirty != dirty_size_paths_.end() && dirty->starts_with(prefix));
        const auto state = recovering_ ? (size ? (size_cache_ready_ ? "stale" : "partial") : "calculating")
            : !size ? (scanning ? "calculating" : "unavailable") : size->unknown ? "partial" : pending ? "updating" : "ready";
        result.push_back({path, size, state});
        if (size && !recovering_) {
            priority_sizes_[path].second = std::chrono::steady_clock::now();
            if (priority_sizes_.size() > 64) {
                auto oldest = std::min_element(priority_sizes_.begin(), priority_sizes_.end(), [](const auto& a, const auto& b) { return a.second.second < b.second.second; });
                if (std::chrono::steady_clock::now() - oldest->second.second < std::chrono::seconds(1)) priority_sizes_.erase(path);
                else priority_sizes_.erase(oldest);
            }
        }
    }
    return result;
}
std::vector<LinuxScanEntry> LinuxCatalog::Search(const std::string& query, const std::string& scope, std::size_t limit, LinuxSearchPhase phase, bool* stale) const {
    std::vector<LinuxScanEntry> result;
    std::string normalized_scope = scope.empty() ? root_ : std::filesystem::path(scope).lexically_normal().string();
    while (normalized_scope.size() > 1 && normalized_scope.back() == '/') normalized_scope.pop_back();
    std::lock_guard lock(mutex_);
    if (recovering_ && !startup_cache_) throw LinuxCatalogNotReady();
    if (stale) *stale = recovering_.load() || std::any_of(repairs_.begin(), repairs_.end(), [&](const auto& repair) { return Within(repair.first, normalized_scope) || Within(normalized_scope, repair.first); });
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

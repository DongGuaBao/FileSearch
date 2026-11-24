#include "mft_service.h"
#include "usn_continuity.h"

#include "file_id_codec.h"
#include "logger.h"
#include "mft_snapshot.h"
#include "usn_utils.h"
#include "utf8.h"
#include "win_file_id.h"

#include <Windows.h>
#include <Psapi.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <mutex>
#include <shared_mutex>
#include "change_snapshot.h"
#include "windows_snapshot_ancestors.h"
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace fsengine {

    namespace {
        using Clock = std::chrono::steady_clock;

        std::uint64_t ProcessPrivateMb()
        {
            PROCESS_MEMORY_COUNTERS_EX counters = {};
            counters.cb = sizeof(counters);
            if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                      sizeof(counters))) {
                return 0;
            }
            return static_cast<std::uint64_t>(counters.PrivateUsage / (1024 * 1024));
        }

        void LogMft(const char* message)
        {
            std::ostringstream out;
            out << "[mft] " << message
                << " private_mb=" << ProcessPrivateMb();
            LogLine(out.str());
        }

        void LogMftDone(const char* message, Clock::time_point start)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            std::ostringstream out;
            out << "[mft] " << message
                << " elapsed_ms=" << ms
                << " private_mb=" << ProcessPrivateMb();
            LogLine(out.str());
        }

        std::size_t CapacityWithSlack(std::size_t count) {
            return count + std::max<std::size_t>(4096, count / 16);
        }

        std::size_t ByteCapacityWithSlack(std::size_t count) {
            return count + std::max<std::size_t>(1024 * 1024, count / 16);
        }

        std::wstring VolumePath(wchar_t drive) {
            return std::wstring(L"\\\\.\\") + drive + L":";
        }

        void TrimCrtHeap() {
            _heapmin();
        }

        bool HasEveryVolumeRoot(const MftSnapshot& snapshot) {
            std::unordered_set<wchar_t> roots;
            roots.reserve(snapshot.volumes.size());
            for (const MftEntryRecord& entry : snapshot.entries) {
                if (!entry.is_directory || entry.parent_frn != kRootParentFrn) {
                    continue;
                }
                const wchar_t drive = DecodeFrnDrive(entry.frn);
                if (drive != L'\0') {
                    roots.insert(drive);
                }
            }
            for (const MftSnapshotVolume& volume : snapshot.volumes) {
                if (roots.find(static_cast<wchar_t>(std::towupper(volume.drive))) == roots.end()) {
                    return false;
                }
            }
            return !snapshot.volumes.empty();
        }

        std::wstring EntryPath(HANDLE volume, const RawEntry& raw_entry) {
            std::wstring parent = PathFromFileId(volume, raw_entry.parent_frn);
            if (parent.empty()) {
                return {};
            }
            if (parent.back() != L'\\' && parent.back() != L'/') {
                parent.push_back(L'\\');
            }
            parent.append(raw_entry.name);
            return parent;
        }

    }  // namespace

    MftService::~MftService() {
        Stop();
        WalClose();
    }

    bool MftService::ValidateAndStoreConfig(MftServiceConfig config, std::wstring* error) {
        for (wchar_t& drive : config.drives) {
            drive = static_cast<wchar_t>(std::towupper(drive));
        }
        std::sort(config.drives.begin(), config.drives.end());
        config.drives.erase(std::unique(config.drives.begin(), config.drives.end()), config.drives.end());
        if (config.drives.empty()) {
            if (error != nullptr) {
                *error = L"MFT drive list is empty.";
            }
            return false;
        }
        for (wchar_t drive : config.drives) {
            if (drive < L'A' || drive > L'Z') {
                if (error != nullptr) {
                    *error = L"Drive letter must be A-Z.";
                }
                return false;
            }
        }
        if (config.snapshot_path.empty()) {
            if (error != nullptr) {
                *error = L"MFT snapshot path is empty.";
            }
            return false;
        }
        if (config.wal_path.empty()) {
            if (error != nullptr) {
                *error = L"MFT change log path is empty.";
            }
            return false;
        }

        config_ = std::move(config);
        volumes_.clear();
        volumes_.reserve(config_.drives.size());
        for (wchar_t drive : config_.drives) {
            VolumeState state;
            state.drive = drive;
            state.volume_root = std::wstring(1, drive) + L":";
            volumes_.push_back(std::move(state));
        }
        configured_ = true;
        facts_released_ = false;
        return true;
    }

    bool MftService::LoadCache(MftServiceConfig config, std::wstring* error) {
        const auto start = Clock::now();
        LogMft("LoadCache begin");
        Stop();
        if (!ValidateAndStoreConfig(std::move(config), error)) {
            LogMftDone("LoadCache config failed", start);
            return false;
        }
        if (!InitVolumes(error)) {
            LogMftDone("LoadCache init volumes failed", start);
            return false;
        }

        MftSnapshot snapshot;
        cache_loaded_ = LoadMftSnapshotFile(config_.snapshot_path, &snapshot, error) &&
            HasEveryVolumeRoot(snapshot) && RestoreStoreFromSnapshot(std::move(snapshot), error);
        if (!cache_loaded_ && error != nullptr && error->empty()) {
            *error = L"MFT snapshot is incomplete and must be rebuilt.";
        }
        LogMftDone(cache_loaded_ ? "LoadCache done" : "LoadCache failed", start);
        return cache_loaded_;
    }

    bool MftService::SaveSnapshotFile(std::wstring* error) const {
        return SaveMftSnapshotFile(BuildSnapshotFromStore(), config_.snapshot_path, error);
    }

    MftSnapshot MftService::BuildSnapshotFromStore() const {
        std::shared_lock lock(store_mutex_);
        return BuildSnapshotFromStoreLocked();
    }
    MftSnapshot MftService::BuildSnapshotFromStoreLocked() const {
        MftSnapshot snapshot;
        snapshot.mutation_version = mutation_version_;
        snapshot.volumes.reserve(volumes_.size());
        for (const VolumeState& state : volumes_) {
            if (!state.ready) {
                continue;
            }
            snapshot.volumes.push_back(MftSnapshotVolume{
                state.drive,
                state.volume_root,
                state.journal_id,
                state.next_usn,
            });
        }
        if (!snapshot.volumes.empty()) {
            snapshot.volume_root = snapshot.volumes.front().volume_root;
            snapshot.usn_journal_id = snapshot.volumes.front().usn_journal_id;
            snapshot.next_usn = snapshot.volumes.front().next_usn;
        }

        snapshot.entries.reserve(frns_.size());
        std::size_t name_bytes = 0;
        for (StringRef ref : names_refs_) {
            name_bytes += ref.length;
        }
        snapshot.names.ReserveAdditional(ByteCapacityWithSlack(name_bytes));
        for (std::size_t i = 0; i < frns_.size(); ++i) {
            const StringRef name_ref = snapshot.names.Add(names_.View(names_refs_[i]));
            snapshot.entries.push_back(MftEntryRecord{
                frns_[i],
                parent_frns_[i],
                name_ref,
                is_directory_[i] != 0,
            });
        }
        return snapshot;
    }

    bool MftService::EncodeChange(wchar_t drive, const IndexChange& raw_change, IndexChange* encoded_change) const {
        if (encoded_change == nullptr) {
            return false;
        }
        Frn encoded_frn = 0;
        Frn encoded_parent = 0;
        if (!EncodeFrn(drive, raw_change.entry.frn, &encoded_frn) ||
            !EncodeFrn(drive, raw_change.entry.parent_frn, &encoded_parent)) {
            return false;
        }
        *encoded_change = raw_change;
        encoded_change->entry.frn = encoded_frn;
        encoded_change->entry.parent_frn = encoded_parent;
        return true;
    }

    bool MftService::EncodeEntry(wchar_t drive, const RawEntry& raw_entry, RawEntry* encoded_entry) const {
        if (encoded_entry == nullptr) {
            return false;
        }
        Frn encoded_frn = 0;
        Frn encoded_parent = 0;
        if (!EncodeFrn(drive, raw_entry.frn, &encoded_frn) ||
            !EncodeFrn(drive, raw_entry.parent_frn, &encoded_parent)) {
            return false;
        }
        *encoded_entry = raw_entry;
        encoded_entry->frn = encoded_frn;
        encoded_entry->parent_frn = encoded_parent;
        return true;
    }

    void MftService::PublishFileEvents(VolumeState& state, const UsnReadBatch& batch) {
        constexpr std::uint32_t kModifiedReasons =
            USN_REASON_BASIC_INFO_CHANGE |
            USN_REASON_DATA_EXTEND |
            USN_REASON_DATA_OVERWRITE |
            USN_REASON_DATA_TRUNCATION |
            USN_REASON_EA_CHANGE |
            USN_REASON_SECURITY_CHANGE |
            USN_REASON_REPARSE_POINT_CHANGE;

        using PublishedTarget = std::pair<Frn, std::wstring>;
        std::unordered_map<Frn, PublishedTarget> published_creates;
        std::unordered_map<Frn, PublishedTarget> published_modifies;
        std::unordered_map<Frn, PublishedTarget> published_renames;
        auto already_published = [](std::unordered_map<Frn, PublishedTarget>& published,
                                    const RawEntry& entry) {
            const PublishedTarget target{entry.parent_frn, entry.name};
            const auto found = published.find(entry.frn);
            if (found != published.end() && found->second == target) {
                return true;
            }
            published[entry.frn] = target;
            return false;
        };

        for (const UsnReadBatch::Record& record : batch.records) {
            RawEntry entry;
            if (!EncodeEntry(state.drive, record.entry, &entry)) {
                continue;
            }
            const std::uint32_t reason = record.reason;
            if ((reason & USN_REASON_RENAME_OLD_NAME) != 0 && (reason & USN_REASON_FILE_DELETE) == 0) {
                state.pending_renames[entry.frn] = std::move(entry);
                continue;
            }

            FileChangeEvent event;
            event.frn = entry.frn;
            event.parent_frn = entry.parent_frn;
            event.name = entry.name;
            event.path = EntryPath(static_cast<HANDLE>(state.monitor_volume), record.entry);
            event.is_directory = entry.is_directory;

            // USN reasons accumulate until CLOSE. A final delete record may
            // still carry an earlier rename reason; deletion takes precedence.
            if ((reason & USN_REASON_FILE_DELETE) != 0) {
                state.pending_renames.erase(entry.frn);
                event.type = entry.is_directory ? FsEventType::DirDeleted : FsEventType::FileDeleted;
                change_events_.Push(std::move(event));
                continue;
            }
            if ((reason & USN_REASON_RENAME_NEW_NAME) != 0) {
                const auto old = state.pending_renames.find(entry.frn);
                // Do not replay an already paired rename from a later CLOSE
                // record whose accumulated reasons still contain NEW_NAME.
                if (old == state.pending_renames.end() && (reason & USN_REASON_CLOSE) != 0) {
                    continue;
                }
                if (old != state.pending_renames.end()) {
                    event.old_parent_frn = old->second.parent_frn;
                    event.old_name = old->second.name;
                    RawEntry old_raw = old->second;
                    old_raw.frn = record.entry.frn;
                    old_raw.parent_frn = DecodeRawFrn(old->second.parent_frn);
                    event.old_path = EntryPath(static_cast<HANDLE>(state.monitor_volume), old_raw);
                    state.pending_renames.erase(old);
                }
                if (already_published(published_renames, entry)) {
                    continue;
                }
                event.type = entry.is_directory ? FsEventType::DirRenamed : FsEventType::FileRenamed;
                change_events_.Push(std::move(event));
                continue;
            }
            if ((reason & USN_REASON_FILE_CREATE) != 0) {
                if (already_published(published_creates, entry)) {
                    continue;
                }
                event.type = entry.is_directory ? FsEventType::DirCreated : FsEventType::FileCreated;
                change_events_.Push(std::move(event));
                continue;
            }
            if ((reason & kModifiedReasons) != 0 && !entry.is_directory) {
                if (already_published(published_modifies, entry)) {
                    continue;
                }
                event.type = FsEventType::FileModified;
                change_events_.Push(std::move(event));
            }
        }

        // Rename pairs are normally adjacent. Bound retained old-name records
        // in case a malformed or truncated journal never supplies the new name.
        if (state.pending_renames.size() > 4096) {
            state.pending_renames.clear();
        }
    }

    bool MftService::EnumerateVolumes(std::wstring* error) {
        const auto total_start = Clock::now();
        LogMft("EnumerateVolumes begin");
        MftSnapshot combined;
        combined.mutation_version = 1;
        std::size_t ready_count = 0;
        for (VolumeState& state : volumes_) {
            const auto drive_start = Clock::now();
            {
                std::ostringstream out;
                out << "[mft] Enumerate drive begin drive=" << static_cast<char>(state.drive)
                    << " private_mb=" << ProcessPrivateMb();
                LogLine(out.str());
            }
            const EnumerateMftResult result = EnumerateMftVolume(state.drive);
            if (!result.ok) {
                LogMftDone("Enumerate drive failed", drive_start);
                state.error = result.error;
                if (error != nullptr && error->empty()) {
                    *error = result.error;
                }
                continue;
            }
            if (result.snapshot.entries.empty() || result.snapshot.volumes.empty()) {
                LogMftDone("Enumerate drive empty", drive_start);
                state.error = L"MFT enumeration returned no entries.";
                continue;
            }
            {
                std::ostringstream out;
                out << "[mft] Enumerate drive done drive=" << static_cast<char>(state.drive)
                    << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - drive_start).count()
                    << " entries=" << result.snapshot.entries.size()
                    << " private_mb=" << ProcessPrivateMb();
                LogLine(out.str());
            }
            const MftSnapshotVolume& volume = result.snapshot.volumes.front();
            state.volume_root = volume.volume_root;
            state.journal_id = volume.usn_journal_id;
            state.next_usn = volume.next_usn;
            state.ready = true;
            state.error.clear();
            combined.volumes.push_back(volume);
            combined.mutation_version += result.snapshot.entries.size();

            combined.names.ReserveAdditional(result.snapshot.names.CharCount());
            for (const MftEntryRecord& entry : result.snapshot.entries) {
                Frn encoded_frn = 0;
                Frn encoded_parent = 0;
                if (!EncodeFrn(state.drive, entry.frn, &encoded_frn) ||
                    !EncodeFrn(state.drive, entry.parent_frn, &encoded_parent)) {
                    continue;
                }
                const StringRef name_ref = combined.names.Add(result.snapshot.Name(entry));
                combined.entries.push_back(MftEntryRecord{
                    encoded_frn,
                    encoded_parent,
                    name_ref,
                    entry.is_directory,
                });
            }
            ++ready_count;
        }

        if (ready_count == 0) {
            LogMftDone("EnumerateVolumes failed", total_start);
            if (error != nullptr && error->empty()) {
                *error = L"No drive MFT cache is ready.";
            }
            return false;
        }
        const bool ok = RestoreStoreFromSnapshot(std::move(combined), error);
        LogMftDone(ok ? "EnumerateVolumes done" : "EnumerateVolumes restore failed", total_start);
        return ok;
    }

    bool MftService::SyncCacheToLatest(std::wstring* error) {
        const auto total_start = Clock::now();
        LogMft("SyncCacheToLatest begin");
        if (!cache_loaded_) {
            if (error != nullptr) {
                *error = L"MFT cache is not loaded.";
            }
            return false;
        }
        if (facts_released_) {
            if (error != nullptr) {
                *error = L"MFT facts were released.";
            }
            return false;
        }
        if (!OpenWal(error)) {
            LogMftDone("SyncCacheToLatest open WAL failed", total_start);
            return false;
        }

        const auto flush_start = Clock::now();
        if (!FlushWalToSnapshot()) {
            LogMftDone("SyncCacheToLatest flush WAL failed", flush_start);
            if (error != nullptr) {
                *error = L"Failed to flush MFT WAL before USN synchronization.";
            }
            return false;
        }
        LogMftDone("SyncCacheToLatest flush WAL done", flush_start);

        for (VolumeState& state : volumes_) {
            if (!state.ready) {
                continue;
            }
            const auto drive_start = Clock::now();
            std::size_t batch_count = 0;
            std::size_t change_count = 0;
            {
                std::ostringstream out;
                out << "[mft] Sync USN drive begin drive=" << static_cast<char>(state.drive)
                    << " next_usn=" << state.next_usn
                    << " private_mb=" << ProcessPrivateMb();
                LogLine(out.str());
            }
            USN_JOURNAL_DATA_V0 journal = {};
            if (state.monitor_volume == nullptr ||
                !EnsureUsnJournal(static_cast<HANDLE>(state.monitor_volume), &journal, error)) {
                return false;
            }
            if (state.journal_id == 0 || state.next_usn <= 0) {
                if (error != nullptr) {
                    *error = L"MFT cache has no USN checkpoint.";
                }
                return false;
            }
            if (state.journal_id != journal.UsnJournalID) {
                if (error != nullptr) {
                    *error = L"USN journal ID changed; MFT cache cannot be synchronized.";
                }
                return false;
            }
            if (journal.FirstUsn > state.next_usn) {
                if (error != nullptr) {
                    *error = L"USN journal was truncated before the MFT cache checkpoint.";
                }
                return false;
            }

            UsnReadCursor cursor{ state.next_usn };
            for (;;) {
                UsnReadBatch batch = ReadUsnChanges(static_cast<HANDLE>(state.monitor_volume),
                                                    journal.UsnJournalID,
                                                    &cursor);
                if (!batch.ok) {
                    if (error != nullptr) {
                        *error = batch.error;
                    }
                    return false;
                }
                if (batch.empty) {
                    break;
                }
                ++batch_count;
                change_count += batch.changes.size();

                std::vector<IndexChange> encoded_changes;
                encoded_changes.reserve(batch.changes.size());
                for (const IndexChange& raw_change : batch.changes) {
                    IndexChange encoded_change;
                    if (EncodeChange(state.drive, raw_change, &encoded_change)) {
                        AppendWalChange(encoded_change);
                        encoded_changes.push_back(std::move(encoded_change));
                        
                    }
                }
                if (!facts_released_) {
                    MergeWalChanges(encoded_changes);
                }
            }
            state.journal_id = journal.UsnJournalID;
            state.next_usn = cursor.next_usn;
            {
                std::ostringstream out;
                out << "[mft] Sync USN drive done drive=" << static_cast<char>(state.drive)
                    << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - drive_start).count()
                    << " batches=" << batch_count
                    << " changes=" << change_count
                    << " next_usn=" << state.next_usn
                    << " private_mb=" << ProcessPrivateMb();
                LogLine(out.str());
            }
        }

        cache_loaded_ = true;
        const auto save_start = Clock::now();
        const bool saved = SaveSnapshot(error);
        LogMftDone(saved ? "SyncCacheToLatest SaveSnapshot done" : "SyncCacheToLatest SaveSnapshot failed", save_start);
        LogMftDone(saved ? "SyncCacheToLatest done" : "SyncCacheToLatest failed", total_start);
        return saved;
    }

    bool MftService::RebuildCache(std::wstring* error) {
        const auto total_start = Clock::now();
        LogMft("RebuildCache begin");
        if (!configured_) {
            if (error != nullptr) {
                *error = L"MFT service is not configured.";
            }
            return false;
        }

        Stop();
        if (!InitVolumes(error)) {
            LogMftDone("RebuildCache init volumes failed", total_start);
            return false;
        }
        if (!OpenWal(error)) {
            LogMftDone("RebuildCache open WAL failed", total_start);
            return false;
        }
        ClearStore();
        if (!EnumerateVolumes(error)) {
            LogMftDone("RebuildCache enumerate failed", total_start);
            return false;
        }
        cache_loaded_ = true;
        const bool saved = SaveSnapshotFile(error);
        if (saved) {
            WalDiscardHistory();
        }
        LogMftDone(saved ? "RebuildCache done" : "RebuildCache save failed", total_start);
        return saved;
    }

    bool MftService::StartMonitor(std::wstring* error) {
        if (!cache_loaded_) {
            if (error != nullptr) {
                *error = L"MFT cache is not ready.";
            }
            return false;
        }
        if (!OpenWal(error)) {
            return false;
        }

        std::lock_guard lock(monitor_mutex_);
        if (running_.load()) {
            return true;
        }

        running_.store(true);
        try {
            monitor_thread_ = std::thread([this]() {
            try {
                MonitorLoop();
            } catch (const std::exception& ex) {
                LogLine("[mft] Monitor worker exception: " + std::string(ex.what()));
                InvalidateChangeStream();
                for (VolumeState& state : volumes_) {
                    state.error = L"MFT monitor worker exception.";
                }
                running_.store(false);
            } catch (...) {
                LogLine("[mft] Monitor worker unknown exception");
                InvalidateChangeStream();
                for (VolumeState& state : volumes_) {
                    state.error = L"MFT monitor worker unknown exception.";
                }
                running_.store(false);
            }
            });
        } catch (const std::system_error& ex) {
            running_.store(false);
            if (error != nullptr) {
                *error = L"Failed to create MFT monitor thread.";
            }
            LogLine("[mft] Failed to create monitor thread: " + std::string(ex.what()));
            return false;
        }
        return true;
    }

    void MftService::Stop() {
        running_.store(false);
        {
            std::lock_guard lock(monitor_mutex_);
            for (VolumeState& state : volumes_) {
                if (state.monitor_volume != nullptr) {
                    CancelIoEx(static_cast<HANDLE>(state.monitor_volume), nullptr);
                }
            }
        }
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }

        if (configured_ && cache_loaded_ && !facts_released_) {
            std::wstring ignored_error;
            SaveSnapshot(&ignored_error);
        }
        CloseVolumeHandles();
        WalClose();
    }

    bool MftService::SaveSnapshot(std::wstring* error) {
        if (!configured_) {
            if (error != nullptr) {
                *error = L"MFT service is not configured.";
            }
            return false;
        }
        std::unique_lock<std::mutex> wal_lock(wal_mutex_, std::defer_lock);
        std::vector<IndexChange> wal_changes;
        if (wal_header_ != nullptr && wal_data_ != nullptr) {
            wal_lock.lock();
            wal_changes = WalReadAllLocked();
        }

        const bool facts_were_released = facts_released_;
        const std::vector<VolumeState> current_volumes = volumes_;
        if (facts_were_released) {
            MftSnapshot snapshot;
            if (!LoadMftSnapshotFile(config_.snapshot_path, &snapshot, error) ||
                !RestoreStoreFromSnapshot(std::move(snapshot), error)) {
                return false;
            }
            for (VolumeState& restored : volumes_) {
                for (const VolumeState& current : current_volumes) {
                    if (restored.drive == current.drive && current.ready) {
                        restored.journal_id = current.journal_id;
                        restored.next_usn = current.next_usn;
                        restored.ready = current.ready;
                        restored.error = current.error;
                        break;
                    }
                }
            }
        }
        if (!wal_changes.empty()) {
            MergeWalChanges(wal_changes);
        }
        const bool saved = SaveSnapshotFile(error);
        if (facts_were_released) {
            ReleaseFactsIfIdle();
        }
        if (!saved) {
            return false;
        }
        return true;
    }

    void MftService::ReleaseFactsIfIdle() {
        std::unique_lock lock(store_mutex_);
        file_count_ = 0;
        directory_count_ = 0;
        names_.ReleaseMemory();
        frn_to_entry_index_.release_memory();
        std::vector<Frn>().swap(frns_);
        std::vector<Frn>().swap(parent_frns_);
        std::vector<StringRef>().swap(names_refs_);
        std::vector<unsigned char>().swap(is_directory_);
        ++mutation_version_;
        facts_released_ = true;
        TrimCrtHeap();
    }
    void MftService::InvalidateChangeStream() {
        std::lock_guard lock(change_snapshot_mutex_);
        if (change_snapshot_healthy_) {
            change_snapshot_healthy_ = false;
            change_events_.Reset();
        }
    }
    bool MftService::AppendStreamWalChangeLocked(const IndexChange& change) {
        const bool appended = TryAppendWalChange(change);
        if (!appended && change_snapshot_healthy_) {
            change_snapshot_healthy_ = false;
            change_events_.Reset();
        }
        return appended;
    }
    void MftService::MonitorLoop() {
        // A failed journal read cannot prove continuity. Fail the additive
        // stream closed without changing the legacy search/cache behavior.
        auto invalidate_changes = [&]() {
            InvalidateChangeStream();
        };
        while (running_.load()) {
            bool saw_change = false;
            for (VolumeState& state : volumes_) {
                if (!running_.load()) {
                    break;
                }
                if (!state.ready) {
                    continue;
                }
                if (state.monitor_volume == nullptr) {
                    state.error = L"MFT volume is not open.";
                    invalidate_changes();
                    continue;
                }
                const std::int64_t before = state.next_usn;
                USN_JOURNAL_DATA_V0 journal = {};
                if (!EnsureUsnJournal(static_cast<HANDLE>(state.monitor_volume), &journal, &state.error)) {
                    invalidate_changes();
                    continue;
                }
                if (state.journal_id != 0 && state.journal_id != journal.UsnJournalID) {
                    state.error = L"USN journal ID changed; cached MFT is stale.";
                    invalidate_changes();
                    continue;
                }
                if (!UsnCheckpointContinuous(state.journal_id, state.next_usn,
                                             journal.UsnJournalID, journal.FirstUsn, journal.NextUsn)) {
                    state.error = L"USN checkpoint is outside retained journal history; cached MFT is stale.";
                    invalidate_changes();
                    continue;
                }

                UsnReadCursor cursor{ state.next_usn };
                UsnReadBatch batch = ReadUsnChanges(static_cast<HANDLE>(state.monitor_volume),
                                                    journal.UsnJournalID,
                                                    &cursor);
                if (!batch.ok) {
                    if (!batch.cancelled || running_.load()) {
                        state.error = batch.error;
                        invalidate_changes();
                    }
                    continue;
                }
                std::lock_guard snapshot_lock(change_snapshot_mutex_);
                if (!batch.empty) {
                    if (change_snapshot_healthy_) PublishFileEvents(state, batch);
                    for (const IndexChange& raw_change : batch.changes) {
                        IndexChange encoded_change;
                        if (EncodeChange(state.drive, raw_change, &encoded_change)) {
                            AppendStreamWalChangeLocked(encoded_change);
                        }
                    }
                    state.journal_id = journal.UsnJournalID;
                    state.next_usn = batch.next_usn;
                }
                state.journal_id = journal.UsnJournalID;
                state.next_usn = cursor.next_usn;
                state.error.clear();
                saw_change = saw_change || state.next_usn != before;
            }
            if (!saw_change) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    bool MftService::InitVolumes(std::wstring* error) {
        for (VolumeState& state : volumes_) {
            if (state.monitor_volume != nullptr) {
                continue;
            }
            HANDLE volume = CreateFileW(VolumePath(state.drive).c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_READONLY,
                                        nullptr);
            if (volume == INVALID_HANDLE_VALUE) {
                state.error = LastWin32ErrorMessage(L"Failed to open volume.");
                if (error != nullptr && error->empty()) {
                    *error = state.error;
                }
                continue;
            }
            USN_JOURNAL_DATA_V0 journal = {};
            if (!EnsureUsnJournal(volume, &journal, &state.error)) {
                CloseHandle(volume);
                if (error != nullptr && error->empty()) {
                    *error = state.error;
                }
                continue;
            }
            state.monitor_volume = volume;
            if (state.journal_id == 0) {
                state.journal_id = journal.UsnJournalID;
            }
            if (state.next_usn <= 0) {
                state.next_usn = journal.NextUsn;
            }
            state.error.clear();
        }
        for (const VolumeState& state : volumes_) {
            if (state.monitor_volume != nullptr) {
                return true;
            }
        }
        if (error != nullptr && error->empty()) {
            *error = L"Failed to open any configured volume.";
        }
        return false;
    }

    void MftService::CloseVolumeHandles() {
        std::lock_guard lock(monitor_mutex_);
        for (VolumeState& state : volumes_) {
            if (state.monitor_volume != nullptr) {
                CloseHandle(static_cast<HANDLE>(state.monitor_volume));
                state.monitor_volume = nullptr;
            }
        }
    }

    MftSnapshot MftService::Snapshot(WalPosition *out_pos) {
        const bool facts_were_released = facts_released_;
        const std::vector<VolumeState> current_volumes = volumes_;
        if (facts_were_released) {
            FlushWalToSnapshot(out_pos);
            std::wstring ignored_error;
            MftSnapshot loaded;
            if (!LoadMftSnapshotFile(config_.snapshot_path, &loaded, &ignored_error) ||
                !RestoreStoreFromSnapshot(std::move(loaded), &ignored_error)) {
                return {};
            }
            for (VolumeState& restored : volumes_) {
                for (const VolumeState& current : current_volumes) {
                    if (restored.drive == current.drive && current.ready) {
                        restored.journal_id = current.journal_id;
                        restored.next_usn = current.next_usn;
                        restored.ready = current.ready;
                        restored.error = current.error;
                        break;
                    }
                }
            }
        } else if (out_pos != nullptr) {
            *out_pos = CurrentWalPosition();
        }
        MftSnapshot snapshot = BuildSnapshotFromStore();
        if (facts_were_released) {
            ReleaseFactsIfIdle();
        }
        return snapshot;
    }

    bool MftService::CaptureChangeSnapshot(MftSnapshot* output, ChangePosition* position, std::wstring* error) {
        if (!output || !position) { if (error) *error = L"Snapshot output is required."; return false; }
        std::lock_guard batch_lock(change_snapshot_mutex_);
        std::lock_guard wal_lock(wal_mutex_);
        if (!change_snapshot_healthy_ || !WalIsOpen()) {
            if (error) *error = L"Change snapshot unavailable after journal continuity or WAL failure; restart/rebuild required.";
            return false;
        }
        try {
            MftSnapshot snapshot;
            {
                std::shared_lock store_lock(store_mutex_);
                if (facts_released_) {
                    if (!LoadMftSnapshotFile(config_.snapshot_path, &snapshot, error)) return false;
                } else snapshot = BuildSnapshotFromStoreLocked();
            }
            ApplyChangeSnapshotDelta(snapshot, WalReadAllLocked());
            std::vector<std::pair<wchar_t, HANDLE>> snapshot_volumes;
            for (const auto& state : volumes_) if (state.ready && state.monitor_volume)
                snapshot_volumes.emplace_back(state.drive, static_cast<HANDLE>(state.monitor_volume));
            CompleteWindowsSnapshotAncestors(snapshot, snapshot_volumes);
            snapshot.volumes.clear();
            for (const auto& state : volumes_) if (state.ready)
                snapshot.volumes.push_back({state.drive, state.volume_root, state.journal_id, state.next_usn});
            if (!snapshot.volumes.empty()) {
                snapshot.volume_root = snapshot.volumes.front().volume_root;
                snapshot.usn_journal_id = snapshot.volumes.front().usn_journal_id;
                snapshot.next_usn = snapshot.volumes.front().next_usn;
            }
            *position = change_events_.CurrentPosition();
            *output = std::move(snapshot);
            return true;
        } catch (const std::exception& exception) {
            if (error) *error = Utf8ToWide(exception.what());
            return false;
        }
    }
    void MftService::ClearStore() {
        std::unique_lock lock(store_mutex_);
        for (VolumeState& state : volumes_) {
            state.ready = false;
            state.journal_id = 0;
            state.next_usn = 0;
        }
        file_count_ = 0;
        directory_count_ = 0;
        names_.Clear();
        frn_to_entry_index_.clear();
        frns_.clear();
        parent_frns_.clear();
        names_refs_.clear();
        is_directory_.clear();
        ++mutation_version_;
    }


    void MftService::MergeWalChanges(const std::vector<IndexChange>& changes) {
        std::unique_lock lock(store_mutex_);
        for (const IndexChange& change : changes) {
            if (change.remove) {
                RemoveEncodedEntry(change.entry.frn);
            }
            else {
                UpsertEncodedEntry(change.entry);
            }
        }
    }

    bool MftService::RestoreStoreFromSnapshot(MftSnapshot snapshot, std::wstring* error) {
        if (snapshot.entries.empty()) {
            if (error != nullptr) {
                *error = L"MFT snapshot is empty.";
            }
            ClearStore();
            facts_released_ = false;
            return false;
        }

        {
            std::unique_lock lock(store_mutex_);
            mutation_version_ = snapshot.mutation_version;
            for (VolumeState& state : volumes_) {
                state.ready = false;
                state.journal_id = 0;
                state.next_usn = 0;
                for (const MftSnapshotVolume& volume : snapshot.volumes) {
                    if (static_cast<wchar_t>(std::towupper(volume.drive)) == state.drive) {
                        state.volume_root = volume.volume_root;
                        state.journal_id = volume.usn_journal_id;
                        state.next_usn = volume.next_usn;
                        state.ready = true;
                        state.error.clear();
                        break;
                    }
                }
            }
            file_count_ = 0;
            directory_count_ = 0;
            names_.Clear();
            frn_to_entry_index_.clear();
            frns_.clear();
            parent_frns_.clear();
            names_refs_.clear();
            is_directory_.clear();

            std::size_t name_bytes = 0;
            for (const MftEntryRecord& entry : snapshot.entries) {
                name_bytes += entry.name_ref.length;
            }
            const std::size_t capacity = CapacityWithSlack(snapshot.entries.size());
            frn_to_entry_index_.reserve(capacity);
            frns_.reserve(capacity);
            parent_frns_.reserve(capacity);
            names_refs_.reserve(capacity);
            is_directory_.reserve(capacity);
            names_.ReserveAdditional(ByteCapacityWithSlack(name_bytes));

            for (const MftEntryRecord& entry : snapshot.entries) {
                const std::string_view name = snapshot.Name(entry);
                if (entry.frn == 0 || name.empty()) {
                    continue;
                }
                const std::size_t index = frns_.size();
                if (index > (std::numeric_limits<std::uint32_t>::max)()) {
                    break;
                }
                frn_to_entry_index_[entry.frn] = static_cast<std::uint32_t>(index);
                frns_.push_back(entry.frn);
                parent_frns_.push_back(entry.parent_frn);
                names_refs_.push_back(names_.Add(name));
                is_directory_.push_back(entry.is_directory ? 1 : 0);
                if (entry.is_directory) {
                    ++directory_count_;
                }
                else {
                    ++file_count_;
                }
            }
        }

        if (file_count_ == 0 && directory_count_ == 0) {
            if (error != nullptr) {
                *error = L"MFT snapshot is empty.";
            }
            ClearStore();
            facts_released_ = false;
            return false;
        }
        facts_released_ = false;
        return true;
    }

    void MftService::UpsertEncodedEntry(const RawEntry& entry) {
        if (entry.frn == 0 || entry.name.empty()) {
            return;
        }
        const std::string lowered_name = WideToLowerUtf8(entry.name);
        const auto found = frn_to_entry_index_.find(entry.frn);
        if (found != frn_to_entry_index_.end()) {
            const std::uint32_t index = found->value;
            const bool was_dir = is_directory_[index] != 0;
            const bool now_dir = entry.is_directory;
            if (was_dir != now_dir) {
                if (was_dir) {
                    --directory_count_;
                    ++file_count_;
                }
                else {
                    --file_count_;
                    ++directory_count_;
                }
            }
            parent_frns_[index] = entry.parent_frn;
            names_refs_[index] = names_.Add(lowered_name);
            is_directory_[index] = now_dir ? 1 : 0;
            ++mutation_version_;
            return;
        }

        const std::size_t index = frns_.size();
        if (index > (std::numeric_limits<std::uint32_t>::max)()) {
            return;
        }
        frn_to_entry_index_[entry.frn] = static_cast<std::uint32_t>(index);
        frns_.push_back(entry.frn);
        parent_frns_.push_back(entry.parent_frn);
        names_refs_.push_back(names_.Add(lowered_name));
        is_directory_.push_back(entry.is_directory ? 1 : 0);
        if (entry.is_directory) {
            ++directory_count_;
        }
        else {
            ++file_count_;
        }
        ++mutation_version_;
    }

    void MftService::RemoveEncodedEntry(Frn frn) {
        const auto found = frn_to_entry_index_.find(frn);
        if (found == frn_to_entry_index_.end()) {
            return;
        }
        const std::size_t index = found->value;
        const bool was_dir = is_directory_[index] != 0;
        const std::size_t last = frns_.size() - 1;
        if (index != last) {
            frns_[index] = frns_[last];
            parent_frns_[index] = parent_frns_[last];
            names_refs_[index] = names_refs_[last];
            is_directory_[index] = is_directory_[last];
            frn_to_entry_index_[frns_[index]] = static_cast<std::uint32_t>(index);
        }
        frns_.pop_back();
        parent_frns_.pop_back();
        names_refs_.pop_back();
        is_directory_.pop_back();
        frn_to_entry_index_.erase(found);
        if (was_dir) {
            --directory_count_;
        }
        else {
            --file_count_;
        }
        ++mutation_version_;
    }

}  // namespace fsengine

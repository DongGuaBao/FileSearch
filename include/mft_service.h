#pragma once

#include "flat_map.h"
#include "mft_snapshot.h"
#include "notifier.h"
#include "string_pool.h"
#include "types.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fsengine
{

    struct UsnReadBatch;

    struct MftServiceConfig
    {
        std::vector<wchar_t> drives;
        std::wstring snapshot_path;
        std::wstring wal_path;
    };

    class FSENGINE_API MftService
    {
      public:
        MftService() = default;
        ~MftService();

        MftService(const MftService &) = delete;
        MftService &operator=(const MftService &) = delete;

        bool LoadCache(MftServiceConfig config, std::wstring *error);
        bool SyncCacheToLatest(std::wstring *error);
        bool RebuildCache(std::wstring *error);
        bool StartMonitor(std::wstring *error);
        void Stop();
        bool SaveSnapshot(std::wstring *error);
        void ReleaseFactsIfIdle();
        bool FlushWalToSnapshot(WalPosition *out_pos = nullptr, bool discard_history = false);
        MftSnapshot Snapshot(WalPosition *out_pos = nullptr);
        bool Running() const
        {
            return running_.load();
        }
        // 获取当前 WAL position
        WalPosition CurrentWalPosition() const;

        // 从指定 position 拉取增量变更，更新 io_pos 为最新位置。
        // expired=true 表示 position 已不可从 WAL 恢复，消费者需从 MFT snapshot 重建。
        std::vector<IndexChange> PullSince(WalPosition &io_pos, bool *expired = nullptr);

        // 等待有新数据写入，返回最新 position
        WalPosition WaitWalNotification(DWORD timeout_ms);

        ChangePosition CurrentChangePosition() const;
        FileChangePullResult PullFileChanges(ChangePosition cursor,
                                             std::size_t max_events = 1024) const;
        ChangePosition WaitFileChanges(ChangePosition cursor, DWORD timeout_ms) const;
        bool CaptureChangeSnapshot(MftSnapshot* snapshot, ChangePosition* position, std::wstring* error);

      private:
        friend struct MftChangeHealthTestAccess;
        void InvalidateChangeStream();
        bool AppendStreamWalChangeLocked(const IndexChange& change);
        struct WalHeader;
        struct WalRecord;

        struct VolumeState
        {
            wchar_t drive = L'\0';
            std::wstring volume_root;
            std::uint64_t journal_id = 0;
            std::int64_t next_usn = 0;
            void *monitor_volume = nullptr;
            bool ready = false;
            std::wstring error;
            std::unordered_map<Frn, RawEntry> pending_renames;
        };

        void MonitorLoop();
        bool InitVolumes(std::wstring *error);
        void CloseVolumeHandles();
        bool ValidateAndStoreConfig(MftServiceConfig config, std::wstring *error);
        bool OpenWal(std::wstring *error);
        bool WalIsOpen() const
        {
            return wal_header_ != nullptr;
        }
        bool WalMapFile(std::uint64_t size, std::wstring *error);
        void WalUnmapFile();
        void WalClose();
        bool WalAppend(const IndexChange &change);
        std::vector<IndexChange> WalReadAll() const;
        std::vector<IndexChange> WalReadAllLocked() const;
        void WalDiscardHistory();
        void WalDiscardHistoryLocked();
        MftSnapshot BuildSnapshotFromStore() const;
        MftSnapshot BuildSnapshotFromStoreLocked() const;
        bool SaveSnapshotFile(std::wstring *error) const;
        bool EnumerateVolumes(std::wstring *error);
        void AppendWalChange(const IndexChange &change);
        bool TryAppendWalChange(const IndexChange &change);
        void ClearStore();
        void MergeWalChanges(const std::vector<IndexChange> &changes);
        bool RestoreStoreFromSnapshot(MftSnapshot snapshot, std::wstring *error);
        void UpsertEncodedEntry(const RawEntry &entry);
        void RemoveEncodedEntry(Frn frn);
        bool EncodeChange(wchar_t drive, const IndexChange &raw_change, IndexChange *encoded_change) const;
        bool EncodeEntry(wchar_t drive, const RawEntry& raw_entry, RawEntry* encoded_entry) const;
        void PublishFileEvents(VolumeState& state, const UsnReadBatch& batch);

        MftServiceConfig config_;
        bool configured_ = false;
        bool cache_loaded_ = false;
        std::vector<VolumeState> volumes_;

        mutable std::mutex monitor_mutex_;
        std::thread monitor_thread_;

        std::atomic<bool> running_{false};
        bool facts_released_ = false;

        WalHeader *wal_header_ = nullptr;
        char *wal_data_ = nullptr;
        HANDLE wal_file_ = INVALID_HANDLE_VALUE;
        HANDLE wal_mapping_ = nullptr;
        std::uint64_t wal_mapped_size_ = 0;
        mutable std::mutex wal_mutex_;

        mutable std::mutex wal_notify_mutex_;
        std::condition_variable wal_notify_cv_;
        WalPosition latest_notify_pos_;

        ChangeEventRing change_events_;
        mutable std::mutex change_snapshot_mutex_;
        bool change_snapshot_healthy_ = true;

        mutable std::shared_mutex store_mutex_;
        std::uint64_t mutation_version_ = 0;
        std::size_t file_count_ = 0;
        std::size_t directory_count_ = 0;
        StringPool names_;
        FlatMap<Frn, std::uint32_t> frn_to_entry_index_;
        std::vector<Frn> frns_;
        std::vector<Frn> parent_frns_;
        std::vector<StringRef> names_refs_;
        std::vector<unsigned char> is_directory_;
    };

} // namespace fsengine

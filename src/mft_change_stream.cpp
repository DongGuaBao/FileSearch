#include "mft_service.h"

#include "mft_snapshot.h"
#include "usn_utils.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace fsengine
{

    struct MftService::WalHeader
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t reserved;
        std::uint64_t capacity_bytes; // 数据区大小 (不含 header)
        std::uint64_t write_pos;      // 下一次写入的环形位置 [0, capacity_bytes)
        std::uint64_t flush_offset;   // 当前保留的最旧记录位置
        std::uint64_t record_count;   // 当前保留的记录数
        std::uint64_t generation;     // write_pos 绕回起点或历史被丢弃的次数
    };

    struct MftService::WalRecord
    {
        std::uint32_t record_bytes;
        std::uint32_t flags;
        Frn frn;
        Frn parent_frn;
        std::uint32_t name_length;
        std::uint32_t reserved;
    };

    namespace
    {

        constexpr char kWalMagic[8] = {'F', 'S', 'E', 'W', 'A', 'L', '5', '\0'};
        constexpr std::uint32_t kWalVersion = 5;
        constexpr std::uint64_t kWalInitialSize = 8ull * 1024ull * 1024ull;

        void SetError(std::wstring *error, const std::wstring &message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
        }

        std::uint64_t Align8(std::uint64_t value)
        {
            return (value + 7ull) & ~7ull;
        }

    } // namespace

    bool MftService::OpenWal(std::wstring *error)
    {
        if (!configured_)
        {
            if (error != nullptr)
            {
                *error = L"MFT service is not configured.";
            }
            return false;
        }
        if (WalIsOpen())
        {
            return true;
        }
        WalClose();
        try
        {
            const std::filesystem::path wal_path(config_.wal_path);
            if (!wal_path.parent_path().empty())
            {
                std::filesystem::create_directories(wal_path.parent_path());
            }
        }
        catch (...)
        {
            SetError(error, L"Failed to create MFT change log directory.");
            return false;
        }

        wal_file_ = CreateFileW(config_.wal_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (wal_file_ == INVALID_HANDLE_VALUE)
        {
            SetError(error, LastWin32ErrorMessage(L"Failed to open MFT change log."));
            return false;
        }

        if (!WalMapFile(kWalInitialSize, error))
        {
            WalClose();
            return false;
        }

        if (std::memcmp(wal_header_->magic, kWalMagic, sizeof(kWalMagic)) != 0 || wal_header_->version != kWalVersion ||
            wal_header_->capacity_bytes != kWalInitialSize - sizeof(WalHeader) || wal_header_->write_pos >= wal_header_->capacity_bytes ||
            wal_header_->flush_offset >= wal_header_->capacity_bytes)
        {
            std::memcpy(wal_header_->magic, kWalMagic, sizeof(kWalMagic));
            wal_header_->version = kWalVersion;
            wal_header_->reserved = 0;
            wal_header_->capacity_bytes = kWalInitialSize - sizeof(WalHeader);
            wal_header_->write_pos = 0;
            wal_header_->flush_offset = 0;
            wal_header_->record_count = 0;
            wal_header_->generation = 0;
            FlushViewOfFile(wal_header_, sizeof(WalHeader));
        }

        return true;
    }

    bool MftService::WalMapFile(std::uint64_t size, std::wstring *error)
    {
        LARGE_INTEGER large_size;
        large_size.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(wal_file_, large_size, nullptr, FILE_BEGIN) || !SetEndOfFile(wal_file_))
        {
            SetError(error, LastWin32ErrorMessage(L"Failed to size MFT change log."));
            return false;
        }

        wal_mapping_ = CreateFileMappingW(wal_file_, nullptr, PAGE_READWRITE, large_size.HighPart, large_size.LowPart, nullptr);
        if (wal_mapping_ == nullptr)
        {
            SetError(error, LastWin32ErrorMessage(L"Failed to map MFT change log."));
            return false;
        }

        void *view = MapViewOfFile(wal_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr)
        {
            SetError(error, LastWin32ErrorMessage(L"Failed to view MFT change log."));
            CloseHandle(wal_mapping_);
            wal_mapping_ = nullptr;
            return false;
        }

        wal_header_ = static_cast<WalHeader *>(view);
        wal_data_ = static_cast<char *>(view);
        wal_mapped_size_ = size;
        return true;
    }

    void MftService::WalUnmapFile()
    {
        if (wal_header_ != nullptr)
        {
            FlushViewOfFile(wal_header_, 0);
            UnmapViewOfFile(wal_header_);
            wal_header_ = nullptr;
            wal_data_ = nullptr;
        }
        if (wal_mapping_ != nullptr)
        {
            CloseHandle(wal_mapping_);
            wal_mapping_ = nullptr;
        }
        wal_mapped_size_ = 0;
    }

    void MftService::WalClose()
    {
        std::lock_guard lock(wal_mutex_);
        WalUnmapFile();
        if (wal_file_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(wal_file_);
            wal_file_ = INVALID_HANDLE_VALUE;
        }
    }

    bool MftService::WalAppend(const IndexChange &change)
    {
        const std::uint64_t header_size = sizeof(WalHeader);
        {
            std::lock_guard state_lock(wal_mutex_);
            if (wal_header_ == nullptr || wal_data_ == nullptr)
            {
                return false;
            }

            const std::uint64_t name_bytes = static_cast<std::uint64_t>(change.entry.name.size() * sizeof(wchar_t));
            const std::uint64_t record_bytes = Align8(sizeof(WalRecord) + name_bytes);
            if (record_bytes > (std::numeric_limits<std::uint32_t>::max)())
            {
                return false;
            }

            const std::uint64_t capacity = wal_header_->capacity_bytes;

            // 环形空闲空间: flush_offset 到 write_pos 之间的空白
            std::uint64_t free_bytes = (wal_header_->flush_offset - wal_header_->write_pos + capacity) % capacity;
            if (wal_header_->record_count == 0)
            {
                free_bytes = capacity; // 空 buffer
            }
            if (free_bytes < record_bytes)
            {
                return false; // 满了, 触发 flush
            }

            const std::uint64_t old_write_pos = wal_header_->write_pos;
            std::uint64_t pos = old_write_pos;
            char *data_region = wal_data_ + header_size;

            // 尾部不足整条记录时填充 0 并 wrap
            if (capacity - pos < record_bytes)
            {
                std::memset(data_region + pos, 0, static_cast<std::size_t>(capacity - pos));
                FlushViewOfFile(data_region + pos, static_cast<SIZE_T>(capacity - pos));
                pos = 0;
            }

            // 写入记录
            auto *record = reinterpret_cast<WalRecord *>(data_region + pos);
            record->record_bytes = static_cast<std::uint32_t>(record_bytes);
            record->flags = (change.remove ? 1u : 0u) | (change.entry.is_directory ? 2u : 0u);
            record->frn = change.entry.frn;
            record->parent_frn = change.entry.parent_frn;
            record->name_length = static_cast<std::uint32_t>(change.entry.name.size());
            record->reserved = 0;
            if (name_bytes != 0)
            {
                std::memcpy(record + 1, change.entry.name.data(), static_cast<std::size_t>(name_bytes));
            }

            pos += record_bytes;
            if (pos >= capacity) pos = 0;
            wal_header_->write_pos = pos;
            // 绕环 (新位置 < 旧位置) 时 generation +1
            if (pos < old_write_pos)
            {
                ++wal_header_->generation;
            }
            ++wal_header_->record_count;

            FlushViewOfFile(record, static_cast<SIZE_T>(record_bytes));
            FlushViewOfFile(wal_header_, sizeof(WalHeader));
        }

        {
            std::lock_guard lock(wal_notify_mutex_);
            latest_notify_pos_.generation = wal_header_->generation;
            latest_notify_pos_.offset = wal_header_->write_pos;
            wal_notify_cv_.notify_all();
        }
        return true;
    }

    std::vector<IndexChange> MftService::WalReadAll() const
    {
        std::lock_guard state_lock(wal_mutex_);
        return WalReadAllLocked();
    }

    std::vector<IndexChange> MftService::WalReadAllLocked() const
    {
        std::vector<IndexChange> changes;
        if (wal_header_ == nullptr || wal_data_ == nullptr || wal_header_->record_count == 0)
        {
            return changes;
        }

        const std::uint64_t header_size = sizeof(WalHeader);
        const std::uint64_t capacity = wal_header_->capacity_bytes;
        char *data_region = wal_data_ + header_size;
        std::uint64_t pos = wal_header_->flush_offset;
        std::uint64_t remaining = wal_header_->record_count;

        changes.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 1024 * 1024)));

        while (remaining > 0)
        {
            const auto *record = reinterpret_cast<const WalRecord *>(data_region + pos);
            if (record->record_bytes == 0)
            {
                pos = 0;
                continue;
            }
            if (record->record_bytes < sizeof(WalRecord))
            {
                break;
            }

            IndexChange change;
            change.entry.frn = record->frn;
            change.entry.parent_frn = record->parent_frn;
            change.entry.is_directory = (record->flags & 2u) != 0;
            const std::uint64_t available_chars = (record->record_bytes - sizeof(WalRecord)) / sizeof(wchar_t);
            const std::size_t name_len = static_cast<std::size_t>(std::min<std::uint64_t>(record->name_length, available_chars));
            const auto *name = reinterpret_cast<const wchar_t *>(record + 1);
            change.entry.name.assign(name, name + name_len);
            change.remove = (record->flags & 1u) != 0;
            if (change.entry.frn != 0)
            {
                changes.push_back(std::move(change));
            }
            pos += record->record_bytes;
            if (pos >= capacity)
            {
                pos = 0;
            }
            --remaining;
        }
        return changes;
    }

    void MftService::WalDiscardHistory()
    {
        std::lock_guard state_lock(wal_mutex_);
        WalDiscardHistoryLocked();
    }

    void MftService::WalDiscardHistoryLocked()
    {
        if (wal_header_ == nullptr)
        {
            return;
        }
        ++wal_header_->generation;
        wal_header_->flush_offset = wal_header_->write_pos;
        wal_header_->record_count = 0;
        FlushViewOfFile(wal_header_, sizeof(WalHeader));
    }

    void MftService::AppendWalChange(const IndexChange &change)
    {
        TryAppendWalChange(change);
    }

    bool MftService::TryAppendWalChange(const IndexChange &change)
    {
        if (!WalIsOpen())
        {
            return false;
        }
        if (WalAppend(change))
        {
            return true;
        }
        if (!FlushWalToSnapshot(nullptr, true))
        {
            return false;
        }
        return WalAppend(change);
    }

    bool MftService::FlushWalToSnapshot(WalPosition *out_pos, bool discard_history)
    {
        std::lock_guard wal_lock(wal_mutex_);
        if (wal_header_ == nullptr || wal_data_ == nullptr)
        {
            if (out_pos != nullptr)
            {
                *out_pos = {};
            }
            return false;
        }
        const std::vector<IndexChange> changes = WalReadAllLocked();
        if (changes.empty())
        {
            if (out_pos != nullptr)
            {
                out_pos->generation = wal_header_->generation;
                out_pos->offset = wal_header_->write_pos;
            }
            return true;
        }

        const bool facts_were_released = facts_released_;
        const std::vector<VolumeState> current_volumes = volumes_;
        if (facts_were_released)
        {
            std::wstring load_error;
            MftSnapshot snapshot;
            if (!LoadMftSnapshotFile(config_.snapshot_path, &snapshot, &load_error) || !RestoreStoreFromSnapshot(std::move(snapshot), &load_error))
            {
                return false;
            }
            for (VolumeState &restored : volumes_)
            {
                for (const VolumeState &current : current_volumes)
                {
                    if (restored.drive == current.drive && current.ready)
                    {
                        restored.journal_id = current.journal_id;
                        restored.next_usn = current.next_usn;
                        restored.ready = current.ready;
                        restored.error = current.error;
                        break;
                    }
                }
            }
            cache_loaded_ = true;
            facts_released_ = false;
        }

        MergeWalChanges(changes);
        std::wstring ignored_error;
        const bool saved = SaveSnapshotFile(&ignored_error);
        if (facts_were_released)
        {
            ReleaseFactsIfIdle();
        }
        if (!saved)
        {
            if (out_pos != nullptr)
            {
                *out_pos = {};
            }
            return false;
        }
        if (discard_history)
        {
            WalDiscardHistoryLocked();
        }
        else
        {
            FlushViewOfFile(wal_header_, sizeof(WalHeader));
        }
        if (out_pos != nullptr)
        {
            out_pos->generation = wal_header_->generation;
            out_pos->offset = wal_header_->write_pos;
        }
        return true;
    }

    WalPosition MftService::CurrentWalPosition() const
    {
        std::lock_guard state_lock(wal_mutex_);
        WalPosition pos;
        if (wal_header_ != nullptr)
        {
            pos.generation = wal_header_->generation;
            pos.offset = wal_header_->write_pos;
        }
        return pos;
    }

    std::vector<IndexChange> MftService::PullSince(WalPosition &io_pos, bool *expired)
    {
        std::lock_guard state_lock(wal_mutex_);
        if (expired != nullptr)
        {
            *expired = false;
        }
        std::vector<IndexChange> changes;
        if (wal_header_ == nullptr || wal_data_ == nullptr || wal_header_->record_count == 0)
        {
            if (wal_header_ != nullptr && expired != nullptr &&
                (io_pos.generation != wal_header_->generation || io_pos.offset != wal_header_->write_pos))
            {
                *expired = true;
            }
            return changes;
        }

        const std::uint64_t header_size = sizeof(WalHeader);
        const std::uint64_t capacity = wal_header_->capacity_bytes;
        char *data_region = wal_data_ + header_size;
        const std::uint64_t write_pos = wal_header_->write_pos;
        std::uint64_t pos = wal_header_->flush_offset;
        std::uint64_t remaining = wal_header_->record_count;

        // io_pos.offset is the next WAL write position observed by the consumer.
        // New records start at that offset, so matching it must not skip a record.
        const std::uint64_t cur_gen = wal_header_->generation;
        if (io_pos.generation > cur_gen)
        {
            if (expired != nullptr)
            {
                *expired = true;
            }
            return changes;
        }

        if (io_pos.generation == cur_gen && io_pos.offset == write_pos)
        {
            return changes;
        }

        bool found_start = false;
        while (!found_start && remaining > 0)
        {
            const auto *record = reinterpret_cast<const WalRecord *>(data_region + pos);
            if (record->record_bytes == 0)
            {
                pos = 0;
                continue;
            }
            if (record->record_bytes < sizeof(WalRecord))
            {
                break;
            }
            if (pos == io_pos.offset)
            {
                found_start = true;
                break;
            }
            pos += record->record_bytes;
            if (pos >= capacity)
            {
                pos = 0;
            }
            --remaining;
        }
        if (!found_start)
        {
            if (expired != nullptr)
            {
                *expired = true;
            }
            return {};
        }
        while (remaining > 0)
        {
            const auto *record = reinterpret_cast<const WalRecord *>(data_region + pos);
            if (record->record_bytes == 0)
            {
                pos = 0;
                continue;
            }
            if (record->record_bytes < sizeof(WalRecord))
            {
                break;
            }

            IndexChange change;
            change.entry.frn = record->frn;
            change.entry.parent_frn = record->parent_frn;
            change.entry.is_directory = (record->flags & 2u) != 0;
            const std::uint64_t available_chars = (record->record_bytes - sizeof(WalRecord)) / sizeof(wchar_t);
            const std::size_t name_len =
                static_cast<std::size_t>(std::min<std::uint64_t>(record->name_length, available_chars));
            const auto *name = reinterpret_cast<const wchar_t *>(record + 1);
            change.entry.name.assign(name, name + name_len);
            change.remove = (record->flags & 1u) != 0;
            if (change.entry.frn != 0)
            {
                changes.push_back(std::move(change));
            }
            pos += record->record_bytes;
            if (pos >= capacity)
            {
                pos = 0;
            }
            --remaining;
        }

        io_pos.generation = cur_gen;
        io_pos.offset = write_pos;
        return changes;
    }

    WalPosition MftService::WaitWalNotification(DWORD timeout_ms)
    {
        std::unique_lock lock(wal_notify_mutex_);
        wal_notify_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
        return latest_notify_pos_;
    }

    ChangePosition MftService::CurrentChangePosition() const
    {
        return change_events_.CurrentPosition();
    }

    FileChangePullResult MftService::PullFileChanges(ChangePosition cursor,
                                                     std::size_t max_events) const
    {
        std::lock_guard lock(change_snapshot_mutex_);
        if (!change_snapshot_healthy_) {
            FileChangePullResult result;
            result.expired = true;
            result.position = change_events_.CurrentPosition();
            return result;
        }
        return change_events_.PullSince(cursor, max_events);
    }

    ChangePosition MftService::WaitFileChanges(ChangePosition cursor, DWORD timeout_ms) const
    {
        return change_events_.WaitForChange(cursor, timeout_ms);
    }

} // namespace fsengine

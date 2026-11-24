#pragma once

#include "types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fsengine {

std::wstring FileNameStorePath(const std::wstring& cache_directory, wchar_t drive);
std::wstring GlobalFileNameStorePath(const std::wstring& cache_directory);
std::wstring GlobalFrnStorePath(const std::wstring& cache_directory);
std::wstring GlobalParentStorePath(const std::wstring& cache_directory);
std::wstring GlobalDirParentStorePath(const std::wstring& cache_directory);

struct DirParentEntry {
    Frn frn = 0;
    Frn parent_frn = 0;
};

bool SaveFileNameStoreFile(const std::wstring& path,
                           const std::vector<std::string>& names,
                           std::wstring* error);
bool SaveFrnStoreFile(const std::wstring& path,
                      const std::vector<Frn>& frns,
                      std::uint64_t mutation_version,
                      WalPosition wal_pos,
                      std::wstring* error);
bool SaveParentStoreFile(const std::wstring& path,
                         const std::vector<Frn>& parent_frns,
                         std::uint64_t mutation_version,
                         WalPosition wal_pos,
                         std::wstring* error);
bool SaveDirParentStoreFile(const std::wstring& path,
                            const std::vector<DirParentEntry>& dirs,
                            std::uint64_t mutation_version,
                            WalPosition wal_pos,
                            std::wstring* error);

// Replaces target_path with temp_path. Unlike MoveFileEx(MOVEFILE_REPLACE_EXISTING),
// this also succeeds when target_path still has an active mapped view (for example
// the previous in-memory index that is serving searches while a new one is built).
// Windows rejects replacing a mapped destination with ERROR_ACCESS_DENIED, so the
// old file is renamed aside first and removed after the new file is installed.
bool ReplaceFileAllowingMappedTarget(const std::wstring& temp_path,
                                     const std::wstring& target_path,
                                     unsigned long* win32_error);

class MappedFileNameStore {
public:
    MappedFileNameStore() = default;
    ~MappedFileNameStore();

    MappedFileNameStore(const MappedFileNameStore&) = delete;
    MappedFileNameStore& operator=(const MappedFileNameStore&) = delete;

    std::string Get(const std::wstring& path, std::size_t file_index) const;
    void Close() const;

private:
    bool OpenLocked(const std::wstring& path) const;

    mutable void* file_ = nullptr;
    mutable void* mapping_ = nullptr;
    mutable const char* view_ = nullptr;
    mutable std::uint64_t size_ = 0;
    mutable std::uint64_t file_count_ = 0;
    mutable std::uint64_t data_offset_ = 0;
    mutable std::mutex mutex_;
};

class MappedFrnStore {
public:
    MappedFrnStore() = default;
    ~MappedFrnStore();

    MappedFrnStore(const MappedFrnStore&) = delete;
    MappedFrnStore& operator=(const MappedFrnStore&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    std::size_t Size() const;
    std::uint64_t MutationVersion() const;
    WalPosition WalPosition() const;
    Frn Get(std::size_t index) const;
    std::vector<Frn> Snapshot() const;

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const char* view_ = nullptr;
    const Frn* frns_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t mutation_version_ = 0;
    fsengine::WalPosition wal_position_;
};

class MappedParentStore {
public:
    MappedParentStore() = default;
    ~MappedParentStore();

    MappedParentStore(const MappedParentStore&) = delete;
    MappedParentStore& operator=(const MappedParentStore&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    std::size_t Size() const;
    std::uint64_t MutationVersion() const;
    WalPosition WalPosition() const;
    Frn Get(std::size_t index) const;

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const char* view_ = nullptr;
    const Frn* parent_frns_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t mutation_version_ = 0;
    fsengine::WalPosition wal_position_;
};

class MappedDirParentStore {
public:
    MappedDirParentStore() = default;
    ~MappedDirParentStore();

    MappedDirParentStore(const MappedDirParentStore&) = delete;
    MappedDirParentStore& operator=(const MappedDirParentStore&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    std::size_t Size() const;
    std::uint64_t MutationVersion() const;
    WalPosition WalPosition() const;
    Frn ParentOf(Frn frn) const;

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const char* view_ = nullptr;
    const DirParentEntry* dirs_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t mutation_version_ = 0;
    fsengine::WalPosition wal_position_;
};

}  // namespace fsengine

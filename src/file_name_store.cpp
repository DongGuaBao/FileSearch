#include "file_name_store.h"

#include "win_handle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fsengine {

bool ReplaceFileAllowingMappedTarget(const std::wstring& temp_path,
                                     const std::wstring& target_path,
                                     unsigned long* win32_error);

namespace {

constexpr char kMagic[8] = {'F', 'S', 'E', 'N', 'A', 'M', '1', '\0'};
constexpr char kFrnMagic[8] = {'F', 'S', 'E', 'F', 'R', 'N', '1', '\0'};
constexpr char kParentMagic[8] = {'F', 'S', 'E', 'P', 'A', 'R', '1', '\0'};
constexpr char kDirParentMagic[8] = {'F', 'S', 'E', 'D', 'I', 'R', '1', '\0'};
constexpr std::uint64_t kMaxFileCount = 10000000ull;

std::filesystem::path TemporaryStorePath(const std::wstring& path) {
    std::filesystem::path temp(path);
    temp += L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetCurrentThreadId());
    return temp;
}

std::wstring StoreIoError(const wchar_t* message,
                          const std::wstring& path,
                          std::uint64_t expected_bytes,
                          int crt_error,
                          DWORD win32_error) {
    std::error_code space_error;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    const auto space = std::filesystem::space(parent.empty() ? std::filesystem::current_path() : parent,
                                               space_error);
    std::wostringstream out;
    out << message << L" path=\"" << path << L"\""
        << L" expected_bytes=" << expected_bytes;
    if (!space_error) {
        out << L" available_bytes=" << space.available;
    }
    out << L" errno=" << crt_error
        << L" win32_error=" << win32_error;
    return out.str();
}

bool ReplaceStoreFile(const std::filesystem::path& temp_path,
                      const std::wstring& path,
                      std::uint64_t expected_bytes,
                      std::wstring* error) {
    unsigned long win32_error = ERROR_SUCCESS;
    if (ReplaceFileAllowingMappedTarget(temp_path.wstring(), path, &win32_error)) {
        return true;
    }
    if (error != nullptr) {
        *error = StoreIoError(L"Failed to replace store file.", path, expected_bytes,
                              errno, static_cast<DWORD>(win32_error));
    }
    return false;
}

bool EnsureParentDirectory(const std::wstring& path, std::wstring* error, const wchar_t* message) {
    try {
        const std::filesystem::path store_path(path);
        if (!store_path.parent_path().empty()) {
            std::filesystem::create_directories(store_path.parent_path());
        }
    } catch (...) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }
    return true;
}

template <typename T>
bool SaveFixedStoreFile(const std::wstring& path,
                        const char (&magic)[8],
                        const std::vector<T>& values,
                        std::uint64_t mutation_version,
                        WalPosition wal_pos,
                        const wchar_t* directory_error,
                        const wchar_t* open_error,
                        const wchar_t* write_error,
                        std::wstring* error) {
    if (!EnsureParentDirectory(path, error, directory_error)) {
        return false;
    }
    const std::uint64_t expected_bytes = sizeof(magic) + sizeof(std::uint64_t) * 4 +
                                         static_cast<std::uint64_t>(values.size()) * sizeof(T);
    const std::filesystem::path temp_path = TemporaryStorePath(path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = StoreIoError(open_error, path, expected_bytes, errno, GetLastError());
        }
        return false;
    }
    const std::uint64_t count = static_cast<std::uint64_t>(values.size());
    out.write(magic, sizeof(magic));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(&mutation_version), sizeof(mutation_version));
    if (!values.empty()) {
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(T)));
    }
    out.write(reinterpret_cast<const char*>(&wal_pos.generation), sizeof(wal_pos.generation));
    out.write(reinterpret_cast<const char*>(&wal_pos.offset), sizeof(wal_pos.offset));
    out.flush();
    if (!out.good()) {
        if (error != nullptr) {
            *error = StoreIoError(write_error, path, expected_bytes, errno, GetLastError());
        }
        return false;
    }
    out.close();
    return ReplaceStoreFile(temp_path, path, expected_bytes, error);
}

template <typename T>
bool OpenFixedStoreFile(const std::wstring& path,
                        const char (&magic)[8],
                        void** file_out,
                        void** mapping_out,
                        const char** view_out,
                        const T** values_out,
                        std::uint64_t* size_out,
                        std::uint64_t* count_out,
                        std::uint64_t* mutation_version_out,
                        WalPosition* wal_position_out) {
    UniqueHandle file(CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file.Valid()) {
        return false;
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file.Get(), &file_size) || file_size.QuadPart <= 0) {
        return false;
    }
    UniqueHandle mapping(CreateFileMappingW(file.Get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.Valid()) {
        return false;
    }
    const char* view = static_cast<const char*>(MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr) {
        return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(file_size.QuadPart);
    if (size < sizeof(magic) + sizeof(std::uint64_t) * 2 ||
        std::memcmp(view, magic, sizeof(magic)) != 0) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* count_ptr = reinterpret_cast<const std::uint64_t*>(view + sizeof(magic));
    const std::uint64_t count = *count_ptr;
    const auto* version_ptr = reinterpret_cast<const std::uint64_t*>(view + sizeof(magic) + sizeof(std::uint64_t));
    const std::uint64_t mutation_version = *version_ptr;
    const std::uint64_t data_offset = sizeof(magic) + sizeof(std::uint64_t) * 2;
    if (count > kMaxFileCount || data_offset + count * sizeof(T) > size) {
        UnmapViewOfFile(view);
        return false;
    }
    WalPosition wal_position;
    const std::uint64_t wal_data_offset = data_offset + count * sizeof(T);
    if (wal_data_offset + sizeof(std::uint64_t) * 2 <= size) {
        wal_position.generation = *reinterpret_cast<const std::uint64_t*>(view + wal_data_offset);
        wal_position.offset = *reinterpret_cast<const std::uint64_t*>(view + wal_data_offset + sizeof(std::uint64_t));
    }

    *file_out = file.Release();
    *mapping_out = mapping.Release();
    *view_out = view;
    *values_out = reinterpret_cast<const T*>(view + data_offset);
    *size_out = size;
    *count_out = count;
    *mutation_version_out = mutation_version;
    *wal_position_out = wal_position;
    return true;
}

}  // namespace

bool ReplaceFileAllowingMappedTarget(const std::wstring& temp_path,
                                     const std::wstring& target_path,
                                     unsigned long* win32_error) {
    if (win32_error != nullptr) {
        *win32_error = ERROR_SUCCESS;
    }
    if (MoveFileExW(temp_path.c_str(), target_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD direct_error = GetLastError();
    if (direct_error != ERROR_ACCESS_DENIED && direct_error != ERROR_SHARING_VIOLATION) {
        if (win32_error != nullptr) {
            *win32_error = direct_error;
        }
        return false;
    }

    // A mapped view of the destination blocks replacement. Move the old file
    // aside first; its mappings keep resolving to the renamed file until the
    // owning index is destroyed.
    std::filesystem::path backup_path(target_path);
    backup_path += L".old-" + std::to_wstring(GetCurrentProcessId()) +
                   L"-" + std::to_wstring(GetCurrentThreadId()) +
                   L"-" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(target_path.c_str(), backup_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (win32_error != nullptr) {
            *win32_error = GetLastError();
        }
        return false;
    }
    if (!MoveFileExW(temp_path.c_str(), target_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD install_error = GetLastError();
        // Best effort: put the previous file back so the cache stays loadable.
        MoveFileExW(backup_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (win32_error != nullptr) {
            *win32_error = install_error;
        }
        return false;
    }
    DeleteFileW(backup_path.c_str());
    return true;
}

std::wstring FileNameStorePath(const std::wstring& cache_directory, wchar_t drive) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    std::wstring file_name;
    file_name.push_back(static_cast<wchar_t>(std::towupper(drive)));
    file_name.append(L".ngram.lowmem.fsenames");
    path /= file_name;
    return path.wstring();
}

std::wstring GlobalFileNameStorePath(const std::wstring& cache_directory) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    path /= L"global.ngram.lowmem.fsenames";
    return path.wstring();
}

std::wstring GlobalFrnStorePath(const std::wstring& cache_directory) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    path /= L"global.ngram.lowmem.frns";
    return path.wstring();
}

std::wstring GlobalParentStorePath(const std::wstring& cache_directory) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    path /= L"global.ngram.lowmem.parents";
    return path.wstring();
}

std::wstring GlobalDirParentStorePath(const std::wstring& cache_directory) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    path /= L"global.ngram.lowmem.dirs";
    return path.wstring();
}

bool SaveFileNameStoreFile(const std::wstring& path,
                           const std::vector<std::string>& names,
                           std::wstring* error) {
    try {
        const std::filesystem::path store_path(path);
        if (!store_path.parent_path().empty()) {
            std::filesystem::create_directories(store_path.parent_path());
        }
    } catch (...) {
        if (error != nullptr) {
            *error = L"Failed to create file-name store directory.";
        }
        return false;
    }

    std::uint64_t name_bytes = 0;
    for (const std::string& name : names) {
        name_bytes += static_cast<std::uint64_t>(name.size());
    }
    const std::uint64_t expected_bytes = sizeof(kMagic) + sizeof(std::uint64_t) * (names.size() + 2ull) +
                                         name_bytes;
    std::error_code space_error;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    const auto space = std::filesystem::space(parent.empty() ? std::filesystem::current_path() : parent,
                                               space_error);
    if (!space_error && space.available < expected_bytes) {
        if (error != nullptr) {
            *error = StoreIoError(L"Insufficient space for file-name store.", path,
                                  expected_bytes, 0, ERROR_DISK_FULL);
        }
        return false;
    }

    const std::filesystem::path temp_path = TemporaryStorePath(path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = StoreIoError(L"Failed to open file-name store.", path,
                                  expected_bytes, errno, GetLastError());
        }
        return false;
    }

    const std::uint64_t file_count = static_cast<std::uint64_t>(names.size());
    std::uint64_t cursor = 0;

    out.write(kMagic, sizeof(kMagic));
    out.write(reinterpret_cast<const char*>(&file_count), sizeof(file_count));
    out.write(reinterpret_cast<const char*>(&cursor), sizeof(cursor));
    for (const std::string& name : names) {
        cursor += static_cast<std::uint64_t>(name.size());
        out.write(reinterpret_cast<const char*>(&cursor), sizeof(cursor));
    }
    for (const std::string& name : names) {
        if (!name.empty()) {
            out.write(name.data(), static_cast<std::streamsize>(name.size()));
        }
    }
    out.flush();
    if (!out.good()) {
        if (error != nullptr) {
            *error = StoreIoError(L"Failed to write file-name store.", path,
                                  expected_bytes, errno, GetLastError());
        }
        return false;
    }
    out.close();
    return ReplaceStoreFile(temp_path, path, expected_bytes, error);
}

bool SaveFrnStoreFile(const std::wstring& path,
                      const std::vector<Frn>& frns,
                      std::uint64_t mutation_version,
                      WalPosition wal_pos,
                      std::wstring* error) {
    return SaveFixedStoreFile(path, kFrnMagic, frns, mutation_version, wal_pos,
                              L"Failed to create FRN store directory.",
                              L"Failed to open FRN store.",
                              L"Failed to write FRN store.",
                              error);
}

bool SaveParentStoreFile(const std::wstring& path,
                         const std::vector<Frn>& parent_frns,
                         std::uint64_t mutation_version,
                         WalPosition wal_pos,
                         std::wstring* error) {
    return SaveFixedStoreFile(path, kParentMagic, parent_frns, mutation_version, wal_pos,
                              L"Failed to create parent store directory.",
                              L"Failed to open parent store.",
                              L"Failed to write parent store.",
                              error);
}

bool SaveDirParentStoreFile(const std::wstring& path,
                            const std::vector<DirParentEntry>& dirs,
                            std::uint64_t mutation_version,
                            WalPosition wal_pos,
                            std::wstring* error) {
    return SaveFixedStoreFile(path, kDirParentMagic, dirs, mutation_version, wal_pos,
                              L"Failed to create directory-parent store directory.",
                              L"Failed to open directory-parent store.",
                              L"Failed to write directory-parent store.",
                              error);
}

MappedFileNameStore::~MappedFileNameStore() {
    Close();
}

std::string MappedFileNameStore::Get(const std::wstring& path, std::size_t file_index) const {
    std::lock_guard lock(mutex_);
    if (view_ == nullptr && !path.empty() && !OpenLocked(path)) {
        return {};
    }

    if (view_ != nullptr && file_index + 1 <= file_count_) {
        const auto* offsets = reinterpret_cast<const std::uint64_t*>(
            view_ + sizeof(kMagic) + sizeof(std::uint64_t));
        const std::uint64_t begin = offsets[file_index];
        const std::uint64_t end = offsets[file_index + 1];
        if (begin <= end && data_offset_ + end <= size_) {
            const char* data = view_ + data_offset_ + begin;
            return std::string(data, static_cast<std::size_t>(end - begin));
        }
    }
    return {};
}

void MappedFileNameStore::Close() const {
    std::lock_guard lock(mutex_);
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    size_ = 0;
    file_count_ = 0;
    data_offset_ = 0;
}

bool MappedFileNameStore::OpenLocked(const std::wstring& path) const {
    UniqueHandle file(CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file.Valid()) {
        return false;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file.Get(), &file_size) || file_size.QuadPart <= 0) {
        return false;
    }

    UniqueHandle mapping(CreateFileMappingW(file.Get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.Valid()) {
        return false;
    }

    const char* view = static_cast<const char*>(MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr) {
        return false;
    }

    const std::uint64_t size = static_cast<std::uint64_t>(file_size.QuadPart);
    bool valid = false;
    std::uint64_t file_count = 0;
    std::uint64_t data_offset = 0;
    if (size >= sizeof(kMagic) + sizeof(std::uint64_t) &&
        std::memcmp(view, kMagic, sizeof(kMagic)) == 0) {
        const auto* count_ptr = reinterpret_cast<const std::uint64_t*>(view + sizeof(kMagic));
        file_count = *count_ptr;
        const std::uint64_t offsets_bytes = (file_count + 1) * sizeof(std::uint64_t);
        data_offset = sizeof(kMagic) + sizeof(std::uint64_t) + offsets_bytes;
        valid = file_count <= kMaxFileCount && data_offset <= size;
    }

    if (!valid) {
        UnmapViewOfFile(view);
        return false;
    }

    file_ = file.Release();
    mapping_ = mapping.Release();
    view_ = view;
    size_ = size;
    file_count_ = file_count;
    data_offset_ = data_offset;
    return true;
}

MappedFrnStore::~MappedFrnStore() {
    Close();
}

bool MappedFrnStore::Open(const std::wstring& path) {
    Close();
    UniqueHandle file(CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file.Valid()) {
        return false;
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file.Get(), &file_size) || file_size.QuadPart <= 0) {
        return false;
    }
    UniqueHandle mapping(CreateFileMappingW(file.Get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.Valid()) {
        return false;
    }
    const char* view = static_cast<const char*>(MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr) {
        return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(file_size.QuadPart);
    if (size < sizeof(kFrnMagic) + sizeof(std::uint64_t) * 2 ||
        std::memcmp(view, kFrnMagic, sizeof(kFrnMagic)) != 0) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* count_ptr = reinterpret_cast<const std::uint64_t*>(view + sizeof(kFrnMagic));
    const std::uint64_t count = *count_ptr;
    const auto* version_ptr = reinterpret_cast<const std::uint64_t*>(view + sizeof(kFrnMagic) + sizeof(std::uint64_t));
    const std::uint64_t mutation_version = *version_ptr;
    const std::uint64_t data_offset = sizeof(kFrnMagic) + sizeof(std::uint64_t) * 2;
    if (count > kMaxFileCount || data_offset + count * sizeof(Frn) > size) {
        UnmapViewOfFile(view);
        return false;
    }
    file_ = file.Release();
    mapping_ = mapping.Release();
    view_ = view;
    frns_ = reinterpret_cast<const Frn*>(view + data_offset);
    size_ = size;
    count_ = count;
    mutation_version_ = mutation_version;
    // 读取末尾的 wal_pos (兼容旧格式)
    const std::uint64_t wal_data_offset = data_offset + count * sizeof(Frn);
    if (wal_data_offset + sizeof(std::uint64_t) * 2 <= size) {
        wal_position_.generation = *reinterpret_cast<const std::uint64_t*>(view + wal_data_offset);
        wal_position_.offset = *reinterpret_cast<const std::uint64_t*>(view + wal_data_offset + sizeof(std::uint64_t));
    }
    return true;
}

void MappedFrnStore::Close() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    frns_ = nullptr;
    size_ = 0;
    count_ = 0;
    mutation_version_ = 0;
    wal_position_ = {};
}

std::size_t MappedFrnStore::Size() const {
    return static_cast<std::size_t>(count_);
}

std::uint64_t MappedFrnStore::MutationVersion() const {
    return mutation_version_;
}

WalPosition MappedFrnStore::WalPosition() const {
    return wal_position_;
}

Frn MappedFrnStore::Get(std::size_t index) const {
    if (index >= count_ || frns_ == nullptr) {
        return 0;
    }
    return frns_[index];
}

std::vector<Frn> MappedFrnStore::Snapshot() const {
    if (frns_ == nullptr) {
        return {};
    }
    return std::vector<Frn>(frns_, frns_ + count_);
}

MappedParentStore::~MappedParentStore() {
    Close();
}

bool MappedParentStore::Open(const std::wstring& path) {
    Close();
    return OpenFixedStoreFile(path,
                              kParentMagic,
                              &file_,
                              &mapping_,
                              &view_,
                              &parent_frns_,
                              &size_,
                              &count_,
                              &mutation_version_,
                              &wal_position_);
}

void MappedParentStore::Close() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    parent_frns_ = nullptr;
    size_ = 0;
    count_ = 0;
    mutation_version_ = 0;
    wal_position_ = {};
}

std::size_t MappedParentStore::Size() const {
    return static_cast<std::size_t>(count_);
}

std::uint64_t MappedParentStore::MutationVersion() const {
    return mutation_version_;
}

WalPosition MappedParentStore::WalPosition() const {
    return wal_position_;
}

Frn MappedParentStore::Get(std::size_t index) const {
    if (index >= count_ || parent_frns_ == nullptr) {
        return 0;
    }
    return parent_frns_[index];
}

MappedDirParentStore::~MappedDirParentStore() {
    Close();
}

bool MappedDirParentStore::Open(const std::wstring& path) {
    Close();
    return OpenFixedStoreFile(path,
                              kDirParentMagic,
                              &file_,
                              &mapping_,
                              &view_,
                              &dirs_,
                              &size_,
                              &count_,
                              &mutation_version_,
                              &wal_position_);
}

void MappedDirParentStore::Close() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    dirs_ = nullptr;
    size_ = 0;
    count_ = 0;
    mutation_version_ = 0;
    wal_position_ = {};
}

std::size_t MappedDirParentStore::Size() const {
    return static_cast<std::size_t>(count_);
}

std::uint64_t MappedDirParentStore::MutationVersion() const {
    return mutation_version_;
}

WalPosition MappedDirParentStore::WalPosition() const {
    return wal_position_;
}

Frn MappedDirParentStore::ParentOf(Frn frn) const {
    if (dirs_ == nullptr || frn == 0) {
        return 0;
    }
    const auto* begin = dirs_;
    const auto* end = dirs_ + count_;
    const auto* found = std::lower_bound(begin, end, frn,
                                         [](const DirParentEntry& entry, Frn value) {
                                             return entry.frn < value;
                                         });
    if (found == end || found->frn != frn) {
        return 0;
    }
    return found->parent_frn;
}

}  // namespace fsengine

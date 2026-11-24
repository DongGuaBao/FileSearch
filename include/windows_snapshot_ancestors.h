#pragma once
#include "mft_snapshot.h"
#include "file_id_codec.h"
#include "flat_map.h"
#include "utf8.h"
#include <Windows.h>
#include <set>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fsengine {
// FSCTL_ENUM_USN_DATA can omit fixed NTFS metadata records whose USN is zero.
// Complete only those stable ancestors in the owned export, using kernel IDs
// and names. Never guess a missing ordinary directory or alter the live cache.
inline void CompleteWindowsSnapshotAncestors(MftSnapshot& snapshot,
    const std::vector<std::pair<wchar_t, HANDLE>>& volumes) {
    FlatMap<Frn, bool> directories;
    std::size_t count = 0;
    for (const auto& entry : snapshot.entries) count += entry.is_directory;
    directories.reserve(count + 32 * volumes.size());
    for (const auto& entry : snapshot.entries) if (entry.is_directory) directories[entry.frn] = true;
    std::set<Frn> missing;
    for (const auto& entry : snapshot.entries)
        if (entry.parent_frn && directories.find(entry.parent_frn) == directories.end()) missing.insert(entry.parent_frn);
    // Include volume roots even for an otherwise empty volume.
    for (const auto& [drive, volume] : volumes) {
        (void)volume;
        const auto root_path = std::wstring(1, drive) + L":\\";
        HANDLE root = CreateFileW(root_path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (root == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot inspect snapshot volume root");
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = GetFileInformationByHandle(root, &info) != 0;
        CloseHandle(root);
        if (!ok) throw std::runtime_error("cannot obtain snapshot volume root ID");
        Frn encoded = 0;
        const auto raw = (static_cast<Frn>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        if (!EncodeFrn(drive, raw, &encoded)) throw std::runtime_error("unsupported volume root ID");
        if (directories.find(encoded) == directories.end()) missing.insert(encoded);
    }
    unsigned added = 0;
    while (!missing.empty()) {
        const auto frn = *missing.begin();
        missing.erase(missing.begin());
        if (directories.find(frn) != directories.end()) continue;
        const auto raw = DecodeRawFrn(frn);
        if ((raw & kFrnRecordMask) >= 32 || ++added > 32 * volumes.size())
            throw std::runtime_error("snapshot has a missing non-metadata ancestor");
        const auto drive = DecodeFrnDrive(frn);
        HANDLE volume = INVALID_HANDLE_VALUE;
        for (const auto& source : volumes) if (source.first == drive) volume = source.second;
        if (volume == INVALID_HANDLE_VALUE) throw std::runtime_error("snapshot ancestor volume unavailable");
        FILE_ID_DESCRIPTOR descriptor{};
        descriptor.dwSize = sizeof(descriptor);
        descriptor.Type = FileIdType;
        descriptor.FileId.QuadPart = static_cast<LONGLONG>(raw);
        HANDLE object = OpenFileById(volume, &descriptor, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (object == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open missing NTFS metadata ancestor");
        struct Close { HANDLE value; ~Close() { CloseHandle(value); } } object_guard{object};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(object, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            ((static_cast<Frn>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != raw)
            throw std::runtime_error("NTFS metadata ancestor generation mismatch");
        std::vector<unsigned char> buffer(sizeof(FILE_NAME_INFO) + 32768 * sizeof(wchar_t));
        if (!GetFileInformationByHandleEx(object, FileNameInfo, buffer.data(), static_cast<DWORD>(buffer.size())))
            throw std::runtime_error("cannot obtain NTFS metadata ancestor name");
        const auto* name_info = reinterpret_cast<const FILE_NAME_INFO*>(buffer.data());
        if (name_info->FileNameLength > buffer.size() - offsetof(FILE_NAME_INFO, FileName) || name_info->FileNameLength % sizeof(wchar_t))
            throw std::runtime_error("invalid NTFS metadata ancestor name");
        const std::wstring path(name_info->FileName, name_info->FileNameLength / sizeof(wchar_t));
        if (path.empty() || path.front() != L'\\') throw std::runtime_error("invalid NTFS metadata ancestor path");
        Frn parent = 0;
        std::wstring name;
        if (path == L"\\") name = std::wstring(1, drive) + L":";
        else {
            const auto separator = path.find_last_of(L'\\');
            name = path.substr(separator + 1);
            const auto parent_path = std::wstring(1, drive) + L":" + (separator ? path.substr(0, separator) : L"\\");
            HANDLE parent_object = CreateFileW(parent_path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (parent_object == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open metadata ancestor parent");
            BY_HANDLE_FILE_INFORMATION parent_info{};
            const bool ok = GetFileInformationByHandle(parent_object, &parent_info) != 0;
            CloseHandle(parent_object);
            const auto parent_raw = (static_cast<Frn>(parent_info.nFileIndexHigh) << 32) | parent_info.nFileIndexLow;
            if (!ok || !(parent_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parent_raw & kFrnRecordMask) >= 32 ||
                !EncodeFrn(drive, parent_raw, &parent)) throw std::runtime_error("invalid metadata ancestor parent");
        }
        const auto lowered = WideToLowerUtf8(name);
        if (snapshot.names.CharCount() > std::numeric_limits<std::uint32_t>::max() ||
            lowered.size() > std::numeric_limits<std::uint32_t>::max() - snapshot.names.CharCount())
            throw std::runtime_error("snapshot ancestor string pool capacity exceeded");
        snapshot.entries.push_back({frn, parent, snapshot.names.Add(lowered), true});
        directories[frn] = true;
        if (parent && directories.find(parent) == directories.end()) missing.insert(parent);
    }
}
}

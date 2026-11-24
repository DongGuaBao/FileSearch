#include "win_file_id.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace fsengine {

namespace {

std::wstring StripFinalPathPrefix(std::wstring path) {
    constexpr std::wstring_view kUncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view kDosPrefix = L"\\\\?\\";
    if (path.rfind(kUncPrefix, 0) == 0) {
        path.replace(0, kUncPrefix.size(), L"\\\\");
    } else if (path.rfind(kDosPrefix, 0) == 0) {
        path.erase(0, kDosPrefix.size());
    }
    return path;
}

}  // namespace

std::wstring_view FileNamePart(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

UniqueHandle OpenVolumeByRoot(const std::wstring& volume_root) {
    std::wstring volume_path = L"\\\\.\\";
    volume_path.append(volume_root);
    return UniqueHandle(CreateFileW(volume_path.c_str(),
                                    0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr));
}

std::wstring PathFromFileId(void* volume_handle, Frn frn) {
    if (volume_handle == nullptr || frn == 0) {
        return {};
    }

    FILE_ID_DESCRIPTOR descriptor = {};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId.QuadPart = static_cast<LONGLONG>(frn);
    UniqueHandle file(OpenFileById(static_cast<HANDLE>(volume_handle),
                                   &descriptor,
                                   0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   FILE_FLAG_BACKUP_SEMANTICS));
    if (!file.Valid()) {
        return {};
    }

    std::wstring path(1024, L'\0');
    DWORD length = GetFinalPathNameByHandleW(file.Get(),
                                             path.data(),
                                             static_cast<DWORD>(path.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= path.size()) {
        path.assign(static_cast<std::size_t>(length) + 1, L'\0');
        length = GetFinalPathNameByHandleW(file.Get(),
                                           path.data(),
                                           static_cast<DWORD>(path.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return StripFinalPathPrefix(std::move(path));
}

bool FileIdFromPath(const std::wstring& path, Frn* frn) {
    if (frn == nullptr || path.empty()) {
        return false;
    }
    UniqueHandle file(CreateFileW(path.c_str(),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr));
    if (!file.Valid()) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(file.Get(), &info)) {
        return false;
    }
    *frn = (static_cast<Frn>(info.nFileIndexHigh) << 32) |
           static_cast<Frn>(info.nFileIndexLow);
    return *frn != 0;
}

}  // namespace fsengine

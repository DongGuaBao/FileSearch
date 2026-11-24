#pragma once

#include "types.h"
#include "win_handle.h"

#include <string>
#include <string_view>

namespace fsengine {

std::wstring_view FileNamePart(std::wstring_view path);
UniqueHandle OpenVolumeByRoot(const std::wstring& volume_root);
std::wstring PathFromFileId(void* volume_handle, Frn frn);
bool FileIdFromPath(const std::wstring& path, Frn* frn);

}  // namespace fsengine

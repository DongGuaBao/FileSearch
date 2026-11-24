#pragma once

#include <string>

namespace fsengine {

std::string ToPinyinFull(std::wstring_view value);
std::string ToPinyinInitials(std::wstring_view value);

}  // namespace fsengine

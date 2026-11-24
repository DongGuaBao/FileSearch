#pragma once

#include "types.h"

#include <string>

namespace fsengine {

FSENGINE_API void LogLine(const std::string& line);
FSENGINE_API void LogLine(std::string&& line);

}  // namespace fsengine

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace fsengine {

std::vector<std::uint8_t> EncodePostingList(const std::vector<std::uint32_t>& postings);

bool ForEachEncodedPostingValue(const std::uint8_t* data,
                                std::uint32_t byte_size,
                                std::uint32_t count,
                                const std::function<bool(std::uint32_t)>& callback);

}  // namespace fsengine

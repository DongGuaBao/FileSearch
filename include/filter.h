#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fsengine {

struct NameFilter {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

struct FilterDictionary {
    static constexpr std::size_t kMaxNgrams = 20;
    std::vector<std::string> ngrams;
};

enum class Filter64Strategy : std::uint32_t {
    DynamicUnicodeHash = 0,
    Utf8Base36 = 1,
};

inline bool operator==(NameFilter left, NameFilter right) {
    return left.low == right.low && left.high == right.high;
}

inline bool operator<(NameFilter left, NameFilter right) {
    return left.high < right.high || (left.high == right.high && left.low < right.low);
}

inline bool ContainsFilter(NameFilter value, NameFilter query) {
    return (value.low & query.low) == query.low &&
           (value.high & query.high) == query.high;
}

NameFilter MakeFilter64(std::string_view lowered_utf8,
                        const FilterDictionary& dictionary,
                        Filter64Strategy strategy);
NameFilter MakeAsciiFilter128(std::string_view value);
FilterDictionary BuildFilterDictionaryFromNames(const std::vector<std::string_view>& lowered_names);

// Returns the minimum edit distance between `pattern` and any substring
// of `text`, or (max_distance + 1) if none is <= max_distance.
int FuzzyDistance(std::string_view text, std::string_view pattern, int max_distance);

}  // namespace fsengine

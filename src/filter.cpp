#include "filter.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utf8.h"

namespace fsengine {

namespace {

constexpr unsigned int kNonAsciiBit64 = 38;
constexpr unsigned int kDynamicBitBase64 = 39;
constexpr unsigned int kUnicodeHashBitBase64 = 59;
constexpr unsigned int kDynamicMinFrequency = 8;

std::uint64_t BaseBitForFilter64Ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        c = static_cast<unsigned char>(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
        return 1ull << (c - 'a');
    }
    if (c >= '0' && c <= '9') {
        return 1ull << (26 + c - '0');
    }
    switch (c) {
    case ' ': return 1ull << 36;
    case '-':
    case '_': return 1ull << 37;
    default:
        return 0;
    }
}

bool DecodeUtf8Codepoint(std::string_view text, std::size_t* offset, std::uint32_t* codepoint) {
    const std::size_t i = *offset;
    const unsigned char first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) {
        *codepoint = first;
        *offset = i + 1;
        return true;
    }

    std::uint32_t value = 0;
    std::size_t length = 0;
    if ((first & 0xE0) == 0xC0) {
        value = first & 0x1F;
        length = 2;
    } else if ((first & 0xF0) == 0xE0) {
        value = first & 0x0F;
        length = 3;
    } else if ((first & 0xF8) == 0xF0) {
        value = first & 0x07;
        length = 4;
    } else {
        *codepoint = first;
        *offset = i + 1;
        return false;
    }
    if (i + length > text.size()) {
        *codepoint = first;
        *offset = i + 1;
        return false;
    }
    for (std::size_t j = 1; j < length; ++j) {
        const unsigned char next = static_cast<unsigned char>(text[i + j]);
        if ((next & 0xC0) != 0x80) {
            *codepoint = first;
            *offset = i + 1;
            return false;
        }
        value = (value << 6) | (next & 0x3F);
    }
    *codepoint = value;
    *offset = i + length;
    return true;
}

std::uint32_t HashUnicodeCodepoint(std::uint32_t codepoint) {
    codepoint ^= codepoint >> 16;
    codepoint *= 0x7feb352du;
    codepoint ^= codepoint >> 15;
    codepoint *= 0x846ca68bu;
    codepoint ^= codepoint >> 16;
    return codepoint;
}

unsigned int Base36Bit(unsigned int value) {
    value %= 36u;
    return value < 26u ? value : 26u + (value - 26u);
}

void AddUtf8Base36Bits(std::uint64_t* filter, std::uint32_t codepoint, std::size_t utf8_length) {
    const std::size_t symbol_count = std::clamp<std::size_t>(utf8_length, 2, 4);
    std::uint32_t hash = HashUnicodeCodepoint(codepoint);
    for (std::size_t i = 0; i < symbol_count; ++i) {
        hash = HashUnicodeCodepoint(hash + static_cast<std::uint32_t>(i * 0x9e3779b9u + 0x85ebca6bu));
        *filter |= 1ull << Base36Bit(hash);
    }
}

bool IsDictionaryChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == ' ' || c == '_' || c == '-';
}

bool IsAsciiLowercaseName(std::string_view name) {
    for (unsigned char c : name) {
        if (c >= 0x80 || (c >= 'A' && c <= 'Z')) {
            return false;
        }
    }
    return true;
}

void CollectNgrams(std::string_view name, std::unordered_set<std::string>* out) {
    for (std::size_t start = 0; start < name.size();) {
        while (start < name.size() && !IsDictionaryChar(static_cast<unsigned char>(name[start]))) {
            ++start;
        }
        std::size_t end = start;
        while (end < name.size() && IsDictionaryChar(static_cast<unsigned char>(name[end]))) {
            ++end;
        }
        const std::string_view run = name.substr(start, end - start);
        for (std::size_t i = 0; i + 2 <= run.size(); ++i) {
            out->insert(std::string(run.substr(i, 2)));
        }
        for (std::size_t i = 0; i + 3 <= run.size(); ++i) {
            out->insert(std::string(run.substr(i, 3)));
        }
        start = end;
    }
}

void SetBit128(NameFilter* filter, unsigned int bit) {
    if (bit < 64) {
        filter->low |= 1ull << bit;
    } else {
        filter->high |= 1ull << (bit - 64);
    }
}

unsigned int CharBit128(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        c = static_cast<unsigned char>(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= '0' && c <= '9') {
        return 26u + c - '0';
    }
    switch (c) {
    case '.': return 36;
    case '_': return 37;
    case '-': return 38;
    case ' ': return 39;
    default:
        return 40u + (c % 24u);
    }
}

unsigned int PairBit128(unsigned char left, unsigned char right) {
    return 64u + ((static_cast<unsigned int>(left) * 131u + right * 17u) % 64u);
}

unsigned int TrigramBit128(unsigned char a, unsigned char b, unsigned char c) {
    return 64u + ((static_cast<unsigned int>(a) * 131u +
                   static_cast<unsigned int>(b) * 31u +
                   static_cast<unsigned int>(c) * 7u) % 64u);
}

}  // namespace

NameFilter MakeFilter64(std::string_view lowered_utf8,
                        const FilterDictionary& dictionary,
                        Filter64Strategy strategy) {
    std::uint64_t filter = 0;
    for (std::size_t offset = 0; offset < lowered_utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(lowered_utf8[offset]);
        if (c < 0x80) {
            filter |= BaseBitForFilter64Ascii(c);
            ++offset;
            continue;
        }
        const std::size_t codepoint_offset = offset;
        std::uint32_t codepoint = 0;
        DecodeUtf8Codepoint(lowered_utf8, &offset, &codepoint);
        filter |= 1ull << kNonAsciiBit64;
        if (strategy == Filter64Strategy::Utf8Base36) {
            AddUtf8Base36Bits(&filter, codepoint, offset - codepoint_offset);
        } else {
            filter |= 1ull << (kUnicodeHashBitBase64 + (HashUnicodeCodepoint(codepoint) % 5u));
        }
    }

    const std::size_t count = std::min<std::size_t>(dictionary.ngrams.size(), FilterDictionary::kMaxNgrams);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& ngram = dictionary.ngrams[i];
        if (!ngram.empty() && lowered_utf8.find(ngram) != std::string_view::npos) {
            filter |= 1ull << (kDynamicBitBase64 + i);
        }
    }
    return NameFilter{filter, 0};
}

NameFilter MakeAsciiFilter128(std::string_view value) {
    NameFilter filter;
    unsigned char previous1 = 0;
    unsigned char previous2 = 0;
    bool has_previous1 = false;
    bool has_previous2 = false;
    for (char raw : value) {
        unsigned char c = static_cast<unsigned char>(raw);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        SetBit128(&filter, CharBit128(c));
        if (has_previous1) {
            SetBit128(&filter, PairBit128(previous1, c));
        }
        if (has_previous2) {
            SetBit128(&filter, TrigramBit128(previous2, previous1, c));
        }
        previous2 = previous1;
        previous1 = c;
        has_previous2 = has_previous1;
        has_previous1 = true;
    }
    return filter;
}

FilterDictionary BuildFilterDictionaryFromNames(const std::vector<std::string_view>& lowered_names) {
    std::unordered_map<std::string, std::uint32_t> frequencies;
    std::unordered_set<std::string> document_ngrams;
    for (std::string_view name : lowered_names) {
        if (!IsAsciiLowercaseName(name)) {
            continue;
        }
        document_ngrams.clear();
        CollectNgrams(name, &document_ngrams);
        for (const std::string& ngram : document_ngrams) {
            ++frequencies[ngram];
        }
    }

    struct Candidate {
        std::string ngram;
        std::uint32_t frequency = 0;
    };
    std::vector<Candidate> candidates;
    std::vector<Candidate> rare_candidates;
    candidates.reserve(frequencies.size());
    for (auto& [ngram, frequency] : frequencies) {
        if (frequency >= kDynamicMinFrequency) {
            candidates.push_back(Candidate{std::move(ngram), frequency});
        } else {
            rare_candidates.push_back(Candidate{ngram, frequency});
        }
    }
    const auto by_rank = [](const Candidate& left, const Candidate& right) {
        if (left.frequency != right.frequency) {
            return left.frequency < right.frequency;
        }
        if (left.ngram.size() != right.ngram.size()) {
            return left.ngram.size() > right.ngram.size();
        }
        return left.ngram < right.ngram;
    };
    std::sort(candidates.begin(), candidates.end(), by_rank);
    std::sort(rare_candidates.begin(), rare_candidates.end(), by_rank);

    FilterDictionary dictionary;
    dictionary.ngrams.reserve(FilterDictionary::kMaxNgrams);
    for (std::size_t i = 0; i < candidates.size() &&
                            dictionary.ngrams.size() < FilterDictionary::kMaxNgrams; ++i) {
        dictionary.ngrams.push_back(std::move(candidates[i].ngram));
    }
    for (std::size_t i = 0; i < rare_candidates.size() &&
                            dictionary.ngrams.size() < FilterDictionary::kMaxNgrams; ++i) {
        dictionary.ngrams.push_back(std::move(rare_candidates[i].ngram));
    }
    return dictionary;
}

namespace {

template <typename StringViewT>
int FuzzyDistanceImpl(StringViewT text, StringViewT pattern, int max_distance) {
    if (pattern.empty()) {
        return 0;
    }

    const std::size_t p_len = pattern.size();
    const std::size_t t_len = text.size();

    std::vector<int> prev(p_len + 1);
    for (std::size_t i = 0; i <= p_len; ++i) {
        prev[i] = static_cast<int>(i);
    }

    std::vector<int> curr(p_len + 1);
    int best = max_distance + 1;
    for (std::size_t j = 1; j <= t_len; ++j) {
        curr[0] = 0;
        for (std::size_t i = 1; i <= p_len; ++i) {
            const int cost = (text[j - 1] == pattern[i - 1]) ? 0 : 1;
            curr[i] = std::min({
                prev[i] + 1,
                curr[i - 1] + 1,
                prev[i - 1] + cost
            });
        }
        if (curr[p_len] < best) {
            best = curr[p_len];
        }
        std::swap(prev, curr);
    }

    return best <= max_distance ? best : max_distance + 1;
}

}  // namespace

int FuzzyDistance(std::string_view text, std::string_view pattern, int max_distance) {
    return FuzzyDistanceImpl(text, pattern, max_distance);
}

}  // namespace fsengine

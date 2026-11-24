#include "ngram_index.h"

#include "file_id_codec.h"
#include "file_name_store.h"
#include "filter.h"
#include "logger.h"
#include "mft_service.h"
#include "ngram_search_index.h"
#include "pinyin.h"
#include "posting_codec.h"
#include "utf8.h"
#include "win_file_id.h"
#include "win_handle.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "Psapi.lib")

namespace fsengine {

namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t ProcessPrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage / (1024 * 1024));
}

void LogNgram(const char* message) {
    std::ostringstream out;
    out << "[ngram] " << message
        << " private_mb=" << ProcessPrivateMb();
    LogLine(out.str());
}

void LogNgramDone(const char* message, Clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::ostringstream out;
    out << "[ngram] " << message
        << " elapsed_ms=" << ms
        << " private_mb=" << ProcessPrivateMb();
    LogLine(out.str());
}

constexpr std::size_t kGramSize = 3;
constexpr std::size_t kShortQueryFuzzyThreshold = 5;
constexpr char kNgramIndexMagic[8] = {'F', 'S', 'E', 'N', 'G', 'R', '3', '\0'};

void TrimCrtHeap() {
    _heapmin();
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string CandidateTextForName(std::string_view name) {
    std::string text(name);
    const std::wstring wide_name = Utf8ToWide(name);
    const std::string pinyin = LowerAscii(ToPinyinFull(wide_name));
    const std::string initials = LowerAscii(ToPinyinInitials(wide_name));
    text.push_back('\n');
    text.append(pinyin);
    text.push_back('\n');
    text.append(initials);
    return text;
}

std::string_view FileNamePartUtf8(std::string_view name) {
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    return name;
}

std::string_view ExtensionPart(std::string_view name) {
    name = FileNamePartUtf8(name);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot + 1 < name.size()) {
        return name.substr(dot + 1);
    }
    return {};
}

int CompareReversed(std::string_view left, std::string_view right) {
    auto left_it = left.rbegin();
    auto right_it = right.rbegin();
    while (left_it != left.rend() && right_it != right.rend()) {
        if (*left_it < *right_it) {
            return -1;
        }
        if (*right_it < *left_it) {
            return 1;
        }
        ++left_it;
        ++right_it;
    }
    if (left_it == left.rend() && right_it == right.rend()) {
        return 0;
    }
    return left_it == left.rend() ? -1 : 1;
}

bool CompressionNameLess(std::string_view left, std::string_view right) {
    const std::string_view left_ext = ExtensionPart(left);
    const std::string_view right_ext = ExtensionPart(right);
    if (left_ext != right_ext) {
        return left_ext < right_ext;
    }
    left = FileNamePartUtf8(left);
    right = FileNamePartUtf8(right);
    const int reversed = CompareReversed(left, right);
    if (reversed != 0) {
        return reversed < 0;
    }
    return left < right;
}

std::uint32_t PackGram(std::string_view gram) {
    return (static_cast<std::uint32_t>(kGramSize) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(gram[0])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(gram[1])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(gram[2]));
}

std::vector<std::uint32_t> MakeGrams(std::string_view value, std::size_t gram_size = kGramSize) {
    std::vector<std::uint32_t> grams;
    if (value.size() < gram_size) {
        return grams;
    }
    for (std::size_t i = 0; i + gram_size <= value.size(); ++i) {
        const std::string_view sub = value.substr(i, gram_size);
        if (sub.find('\n') != std::string_view::npos) {
            continue;
        }
        grams.push_back(PackGram(sub));
    }
    std::sort(grams.begin(), grams.end());
    grams.erase(std::unique(grams.begin(), grams.end()), grams.end());
    return grams;
}

std::vector<std::size_t> IntersectSorted(const std::vector<std::size_t>& left,
                                         const std::vector<std::uint32_t>& right) {
    std::vector<std::size_t> out;
    out.reserve(std::min<std::size_t>(left.size(), right.size()));
    auto left_it = left.begin();
    auto right_it = right.begin();
    while (left_it != left.end() && right_it != right.end()) {
        if (*left_it < *right_it) {
            ++left_it;
        } else if (*right_it < *left_it) {
            ++right_it;
        } else {
            out.push_back(*left_it);
            ++left_it;
            ++right_it;
        }
    }
    return out;
}

struct GramDf {
    std::uint32_t gram = 0;
    std::uint32_t df = 0;
};

struct NgramIndexFileHeader {
    char magic[8];
    std::uint64_t gram_count = 0;
    std::uint64_t stop_gram_count = 0;
    std::uint64_t compressed_posting_bytes = 0;
    std::uint64_t indexed_file_count = 0;
};

std::wstring GlobalNgramIndexPath(const std::wstring& cache_directory) {
    std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache")
                                                         : std::filesystem::path(cache_directory);
    path /= L"global.ngram.lowmem.index";
    return path.wstring();
}

std::wstring NormalizeScopeComparable(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') {
        value.pop_back();
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool PathMatchesScope(std::wstring_view path, const SearchScope& scope) {
    if (!scope.enabled) {
        return true;
    }
    const std::wstring normalized = NormalizeScopeComparable(std::wstring(path));
    if (!scope.normalized_prefix.empty() && scope.normalized_prefix.back() == L'\\') {
        return normalized.rfind(scope.normalized_prefix, 0) == 0;
    }
    return normalized == scope.normalized_prefix ||
           (normalized.size() > scope.normalized_prefix.size() &&
            normalized.rfind(scope.normalized_prefix, 0) == 0 &&
            normalized[scope.normalized_prefix.size()] == L'\\');
}

bool SortNamesFrnsAndParents(std::vector<std::string>* names,
                             std::vector<Frn>* frns,
                             std::vector<Frn>* parent_frns,
                             std::wstring* error) {
    if (names->size() != frns->size() || names->size() != parent_frns->size()) {
        if (error != nullptr) {
            *error = L"Ngram name, FRN, and parent-FRN counts do not match.";
        }
        return false;
    }
    if (names->size() > (std::numeric_limits<std::uint32_t>::max)()) {
        if (error != nullptr) {
            *error = L"Ngram low-memory index has too many entries.";
        }
        return false;
    }

    std::vector<std::uint32_t> order;
    order.reserve(names->size());
    for (std::size_t i = 0; i < names->size(); ++i) {
        order.push_back(static_cast<std::uint32_t>(i));
    }
    std::stable_sort(order.begin(), order.end(),
                     [names](std::uint32_t left, std::uint32_t right) {
                         const std::string& left_name = (*names)[left];
                         const std::string& right_name = (*names)[right];
                         if (left_name == right_name) {
                             return left < right;
                         }
                         return CompressionNameLess(left_name, right_name);
                     });

    std::vector<std::string> sorted_names;
    std::vector<Frn> sorted_frns;
    std::vector<Frn> sorted_parent_frns;
    sorted_names.reserve(names->size());
    sorted_frns.reserve(frns->size());
    sorted_parent_frns.reserve(parent_frns->size());
    for (std::uint32_t index : order) {
        sorted_names.push_back(std::move((*names)[index]));
        sorted_frns.push_back((*frns)[index]);
        sorted_parent_frns.push_back((*parent_frns)[index]);
    }
    *names = std::move(sorted_names);
    *frns = std::move(sorted_frns);
    *parent_frns = std::move(sorted_parent_frns);
    return true;
}

void BuildNgramFromSortedNames(const std::vector<std::string>& names, NgramIndex* index) {
    std::vector<std::uint32_t> unique_indices;
    unique_indices.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const bool is_same_name_previous = i != 0 && names[i - 1] == names[i];
        if (!is_same_name_previous) {
            unique_indices.push_back(static_cast<std::uint32_t>(i));
        }
    }
    index->BuildFromEntries(unique_indices.size(),
                            [&names, &unique_indices](std::size_t i) {
                                const std::uint32_t name_index = unique_indices[i];
                                return NgramBuildEntry{
                                    name_index,
                                    CandidateTextForName(names[name_index]),
                                };
                            });
}

}  // namespace

NgramIndex::~NgramIndex() {
    CloseMapped();
}

void NgramIndex::BuildFromTexts(const std::vector<std::string>& candidate_texts) {
    std::vector<NgramBuildEntry> entries;
    entries.reserve(candidate_texts.size());
    for (std::size_t i = 0; i < candidate_texts.size(); ++i) {
        if (i > (std::numeric_limits<std::uint32_t>::max)()) {
            break;
        }
        entries.push_back(NgramBuildEntry{static_cast<std::uint32_t>(i), candidate_texts[i]});
    }
    BuildFromEntries(entries);
}

void NgramIndex::BuildFromEntries(const std::vector<NgramBuildEntry>& candidate_texts) {
    BuildFromEntries(candidate_texts.size(),
                     [&candidate_texts](std::size_t i) {
                         return candidate_texts[i];
                     });
}

void NgramIndex::BuildFromEntries(std::size_t entry_count,
                                  const std::function<NgramBuildEntry(std::size_t)>& entry_provider) {
    const auto total_start = Clock::now();
    {
        std::ostringstream out;
        out << "[ngram] BuildFromEntries begin"
            << " entries=" << entry_count
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }
    CloseMapped();

    const std::size_t file_count = entry_count;
    struct PostingPair {
        std::uint32_t gram = 0;
        std::uint32_t index = 0;
    };
    struct ChunkInfo {
        std::filesystem::path path;
        std::size_t pair_count = 0;
    };
    struct TempDirGuard {
        std::filesystem::path path;
        ~TempDirGuard() {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        }
    };

    constexpr std::size_t kChunkPairLimit = 4ull * 1024ull * 1024ull;
    const auto ticks = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    TempDirGuard temp_dir{
        std::filesystem::temp_directory_path() /
        (L"filesearch-ngram-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks))
    };
    std::error_code create_ec;
    std::filesystem::create_directories(temp_dir.path, create_ec);
    if (create_ec) {
        LogLine("[ngram] BuildFromEntries failed to create temp dir");
        grams_.clear();
        stop_grams_.clear();
        compressed_postings_.clear();
        indexed_file_count_ = 0;
        return;
    }

    std::vector<ChunkInfo> chunks;
    std::vector<PostingPair> chunk_pairs;
    chunk_pairs.reserve((std::min)(kChunkPairLimit, std::max<std::size_t>(1024, entry_count * 8)));
    std::unordered_map<std::uint32_t, std::uint32_t> gram_df_map;
    gram_df_map.reserve(128 * 1024);
    std::size_t posting_count = 0;
    std::size_t posting_capacity = kChunkPairLimit;

    auto flush_chunk = [&]() {
        if (chunk_pairs.empty()) {
            return true;
        }
        std::sort(chunk_pairs.begin(), chunk_pairs.end(),
                  [](const PostingPair& left, const PostingPair& right) {
                      if (left.gram != right.gram) {
                          return left.gram < right.gram;
                      }
                      return left.index < right.index;
                  });
        chunk_pairs.erase(std::unique(chunk_pairs.begin(),
                                      chunk_pairs.end(),
                                      [](const PostingPair& left, const PostingPair& right) {
                                          return left.gram == right.gram && left.index == right.index;
                                      }),
                          chunk_pairs.end());

        for (const PostingPair& pair : chunk_pairs) {
            ++gram_df_map[pair.gram];
        }
        posting_count += chunk_pairs.size();

        const std::filesystem::path chunk_path =
            temp_dir.path / (L"chunk-" + std::to_wstring(chunks.size()) + L".bin");
        std::ofstream out(chunk_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(chunk_pairs.data()),
                  static_cast<std::streamsize>(chunk_pairs.size() * sizeof(PostingPair)));
        if (!out.good()) {
            return false;
        }
        chunks.push_back(ChunkInfo{chunk_path, chunk_pairs.size()});
        chunk_pairs.clear();
        return true;
    };

    const auto collect_start = Clock::now();
    for (std::size_t i = 0; i < entry_count; ++i) {
        const NgramBuildEntry entry = entry_provider(i);
        for (const std::uint32_t gram : MakeGrams(entry.text, kGramSize)) {
            chunk_pairs.push_back(PostingPair{gram, entry.index});
            if (chunk_pairs.size() >= kChunkPairLimit && !flush_chunk()) {
                LogLine("[ngram] BuildFromEntries failed to write chunk");
                grams_.clear();
                stop_grams_.clear();
                compressed_postings_.clear();
                indexed_file_count_ = 0;
                return;
            }
        }
    }
    if (!flush_chunk()) {
        LogLine("[ngram] BuildFromEntries failed to write final chunk");
        grams_.clear();
        stop_grams_.clear();
        compressed_postings_.clear();
        indexed_file_count_ = 0;
        return;
    }
    chunk_pairs.shrink_to_fit();
    {
        std::ostringstream out;
        out << "[ngram] BuildFromEntries collect chunks done"
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - collect_start).count()
            << " chunks=" << chunks.size()
            << " grams=" << gram_df_map.size()
            << " posting_count=" << posting_count
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }

    const auto sort_start = Clock::now();
    std::vector<GramPosting> grams;
    grams.reserve(gram_df_map.size());
    std::vector<GramDf> gram_dfs;
    gram_dfs.reserve(gram_df_map.size());
    for (const auto& [gram, df] : gram_df_map) {
        gram_dfs.push_back(GramDf{gram, df});
    }
    {
        std::ostringstream out;
        out << "[ngram] BuildFromEntries chunk sort done"
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sort_start).count()
            << " posting_count=" << posting_count
            << " posting_capacity=" << posting_capacity
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }

    const auto stop_start = Clock::now();
    std::size_t stop_df_threshold_count = 0;
    std::size_t stop_top_count = 0;
    std::vector<std::uint32_t> stop_grams;
    {
        std::ostringstream out;
        out << "[ngram] BuildFromEntries stop grams done"
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stop_start).count()
            << " stop_grams=0"
            << " disabled=1"
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }

    const auto compress_start = Clock::now();
    std::vector<std::uint8_t> compressed_postings;
    compressed_postings.reserve(std::max<std::size_t>(1024, posting_count * sizeof(std::uint32_t) / 2));
    std::size_t stop_posting_count = 0;

    struct ChunkCursor {
        std::ifstream in;
        PostingPair current;
        bool has_current = false;

        explicit ChunkCursor(const std::filesystem::path& path) : in(path, std::ios::binary) {}

        bool ReadNext() {
            if (!in.read(reinterpret_cast<char*>(&current), sizeof(current))) {
                has_current = false;
                return false;
            }
            has_current = true;
            return true;
        }
    };
    struct HeapItem {
        PostingPair pair;
        std::size_t cursor = 0;
    };
    struct HeapCompare {
        bool operator()(const HeapItem& left, const HeapItem& right) const {
            if (left.pair.gram != right.pair.gram) {
                return left.pair.gram > right.pair.gram;
            }
            return left.pair.index > right.pair.index;
        }
    };

    std::vector<std::unique_ptr<ChunkCursor>> cursors;
    cursors.reserve(chunks.size());
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare> heap;
    for (const ChunkInfo& chunk : chunks) {
        auto cursor = std::make_unique<ChunkCursor>(chunk.path);
        if (cursor->ReadNext()) {
            heap.push(HeapItem{cursor->current, cursors.size()});
        }
        cursors.push_back(std::move(cursor));
    }

    std::vector<std::uint32_t> posting_list;
    while (!heap.empty()) {
        const std::uint32_t gram = heap.top().pair.gram;
        posting_list.clear();
        bool have_last = false;
        std::uint32_t last_index = 0;
        while (!heap.empty() && heap.top().pair.gram == gram) {
            const HeapItem item = heap.top();
            heap.pop();
            if (!have_last || item.pair.index != last_index) {
                posting_list.push_back(item.pair.index);
                last_index = item.pair.index;
                have_last = true;
            }
            ChunkCursor& cursor = *cursors[item.cursor];
            if (cursor.ReadNext()) {
                heap.push(HeapItem{cursor.current, item.cursor});
            }
        }

        const std::size_t offset = compressed_postings.size();
        const std::vector<std::uint8_t> encoded = EncodePostingList(posting_list);
        if (offset > (std::numeric_limits<std::uint32_t>::max)() ||
            posting_list.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            encoded.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            continue;
        }
        compressed_postings.insert(compressed_postings.end(), encoded.begin(), encoded.end());
        grams.push_back(GramPosting{
            gram,
            static_cast<std::uint32_t>(offset),
            static_cast<std::uint32_t>(posting_list.size()),
            static_cast<std::uint32_t>(encoded.size()),
        });
    }
    grams.shrink_to_fit();
    compressed_postings.shrink_to_fit();

    grams_ = std::move(grams);
    stop_grams_ = std::move(stop_grams);
    compressed_postings_ = std::move(compressed_postings);
    build_temp_posting_count_ = posting_count;
    build_temp_posting_capacity_ = posting_capacity;
    stop_posting_count_ = stop_posting_count;
    stop_df_threshold_count_ = stop_df_threshold_count;
    stop_top_count_ = stop_top_count;
    indexed_file_count_ = file_count;
    {
        std::ostringstream out;
        out << "[ngram] BuildFromEntries compress done"
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - compress_start).count()
            << " grams=" << grams_.size()
            << " compressed_bytes=" << compressed_postings_.size()
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }
    LogNgramDone("BuildFromEntries done", total_start);
}

bool NgramIndex::SaveToFile(const std::wstring& path, std::wstring* error) const {
    try {
        const std::filesystem::path index_path(path);
        if (!index_path.parent_path().empty()) {
            std::filesystem::create_directories(index_path.parent_path());
        }
    } catch (...) {
        if (error != nullptr) {
            *error = L"Failed to create ngram index directory.";
        }
        return false;
    }

    std::filesystem::path temp_path(path);
    temp_path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(GetCurrentThreadId());
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = L"Failed to open ngram index temporary file: " + temp_path.wstring() +
                     L" win32_error=" + std::to_wstring(GetLastError());
        }
        return false;
    }
    NgramIndexFileHeader header;
    std::memcpy(header.magic, kNgramIndexMagic, sizeof(kNgramIndexMagic));
    header.gram_count = static_cast<std::uint64_t>(grams_.size());
    header.stop_gram_count = static_cast<std::uint64_t>(stop_grams_.size());
    header.compressed_posting_bytes = static_cast<std::uint64_t>(compressed_postings_.size());
    header.indexed_file_count = indexed_file_count_;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(grams_.data()),
              static_cast<std::streamsize>(grams_.size() * sizeof(GramPosting)));
    out.write(reinterpret_cast<const char*>(stop_grams_.data()),
              static_cast<std::streamsize>(stop_grams_.size() * sizeof(std::uint32_t)));
    out.write(reinterpret_cast<const char*>(compressed_postings_.data()),
              static_cast<std::streamsize>(compressed_postings_.size()));
    out.flush();
    if (!out.good()) {
        if (error != nullptr) {
            *error = L"Failed to write ngram index file: " + path +
                     L" win32_error=" + std::to_wstring(GetLastError());
        }
        return false;
    }
    out.close();
    unsigned long win32_error = ERROR_SUCCESS;
    if (!ReplaceFileAllowingMappedTarget(temp_path.wstring(), path, &win32_error)) {
        if (error != nullptr) {
            *error = L"Failed to replace ngram index file: " + path +
                     L" win32_error=" + std::to_wstring(win32_error);
        }
        return false;
    }
    return true;
}

bool NgramIndex::LoadMapped(const std::wstring& path) {
    CloseMapped();
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
    if (size < sizeof(NgramIndexFileHeader)) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* header = reinterpret_cast<const NgramIndexFileHeader*>(view);
    if (std::memcmp(header->magic, kNgramIndexMagic, sizeof(kNgramIndexMagic)) != 0) {
        UnmapViewOfFile(view);
        return false;
    }
    std::uint64_t offset = sizeof(NgramIndexFileHeader);
    const std::uint64_t grams_bytes = header->gram_count * sizeof(GramPosting);
    const std::uint64_t stop_bytes = header->stop_gram_count * sizeof(std::uint32_t);
    if (offset > size || grams_bytes > size - offset) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* grams = reinterpret_cast<const GramPosting*>(view + offset);
    offset += grams_bytes;
    if (stop_bytes > size - offset) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* stop_grams = reinterpret_cast<const std::uint32_t*>(view + offset);
    offset += stop_bytes;
    if (header->compressed_posting_bytes > size - offset) {
        UnmapViewOfFile(view);
        return false;
    }
    const auto* compressed = reinterpret_cast<const std::uint8_t*>(view + offset);

    grams_.clear();
    stop_grams_.clear();
    compressed_postings_.clear();
    mapped_file_ = file.Release();
    mapped_mapping_ = mapping.Release();
    mapped_view_ = view;
    mapped_size_ = size;
    mapped_grams_ = grams;
    mapped_gram_count_ = header->gram_count;
    mapped_stop_grams_ = stop_grams;
    mapped_stop_gram_count_ = header->stop_gram_count;
    mapped_compressed_postings_ = compressed;
    mapped_compressed_posting_bytes_ = header->compressed_posting_bytes;
    indexed_file_count_ = static_cast<std::size_t>(header->indexed_file_count);
    return true;
}

void NgramIndex::CloseMapped() {
    if (mapped_view_ != nullptr) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_ = nullptr;
    }
    if (mapped_mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapped_mapping_));
        mapped_mapping_ = nullptr;
    }
    if (mapped_file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapped_file_));
        mapped_file_ = nullptr;
    }
    mapped_size_ = 0;
    mapped_grams_ = nullptr;
    mapped_gram_count_ = 0;
    mapped_stop_grams_ = nullptr;
    mapped_stop_gram_count_ = 0;
    mapped_compressed_postings_ = nullptr;
    mapped_compressed_posting_bytes_ = 0;
}

bool NgramIndex::Empty() const {
    return mapped_view_ != nullptr ? mapped_gram_count_ == 0 : grams_.empty();
}

bool NgramIndex::CanQuery(std::string_view query) const {
    return query.size() >= kGramSize;
}

const NgramIndex::GramPosting* NgramIndex::FindPosting(std::uint32_t gram) const {
    const GramPosting* begin = mapped_view_ != nullptr ? mapped_grams_ : grams_.data();
    const std::size_t count = mapped_view_ != nullptr ? static_cast<std::size_t>(mapped_gram_count_) : grams_.size();
    if (count == 0) {
        return nullptr;
    }
    const GramPosting* end = begin + count;
    const auto found = std::lower_bound(begin, end, gram,
                                        [](const GramPosting& posting, std::uint32_t value) {
                                            return posting.gram < value;
                                        });
    if (found == end || found->gram != gram) {
        return nullptr;
    }
    return found;
}

NgramIndexStats NgramIndex::Stats() const {
    NgramIndexStats stats;
    const GramPosting* grams = mapped_view_ != nullptr ? mapped_grams_ : grams_.data();
    const std::size_t gram_count = mapped_view_ != nullptr ? static_cast<std::size_t>(mapped_gram_count_) : grams_.size();
    stats.gram_count = gram_count;
    for (std::size_t i = 0; i < gram_count; ++i) {
        const GramPosting& posting = grams[i];
        stats.posting_count += posting.count;
    }
    stats.gram_bytes = gram_count * sizeof(GramPosting) +
                       (mapped_view_ != nullptr ? mapped_stop_gram_count_ : stop_grams_.capacity()) * sizeof(std::uint32_t);
    stats.posting_bytes = mapped_view_ != nullptr ? mapped_compressed_posting_bytes_ : compressed_postings_.capacity() * sizeof(std::uint8_t);
    stats.posting_raw_bytes = stats.posting_count * sizeof(std::uint32_t);
    stats.total_bytes = stats.gram_bytes + stats.posting_bytes;
    if (stats.posting_raw_bytes != 0) {
        stats.posting_compression_ratio =
            static_cast<double>(stats.posting_bytes) / static_cast<double>(stats.posting_raw_bytes);
    }
    for (std::size_t i = 0; i < gram_count; ++i) {
        const GramPosting& posting = grams[i];
        const std::uint32_t gram_size = posting.gram >> 24;
        if (gram_size == kGramSize) {
            ++stats.trigram_gram_count;
            stats.trigram_posting_count += posting.count;
        }
    }
    stats.build_temp_posting_count = build_temp_posting_count_;
    stats.build_temp_posting_capacity = build_temp_posting_capacity_;
    stats.stop_gram_count = stop_grams_.size();
    stats.stop_posting_count = stop_posting_count_;
    stats.stop_df_threshold_count = stop_df_threshold_count_;
    stats.stop_top_count = stop_top_count_;
    return stats;
}

bool NgramIndex::IsStopGram(std::uint32_t gram) const {
    if (mapped_view_ != nullptr) {
        if (mapped_stop_gram_count_ == 0) {
            return false;
        }
        return std::binary_search(mapped_stop_grams_,
                                  mapped_stop_grams_ + mapped_stop_gram_count_,
                                  gram);
    }
    return std::binary_search(stop_grams_.begin(), stop_grams_.end(), gram);
}

std::vector<std::uint32_t> NgramIndex::FilterStopGrams(
    const std::vector<std::uint32_t>& grams) const {
    std::vector<std::uint32_t> filtered;
    filtered.reserve(grams.size());
    for (const std::uint32_t gram : grams) {
        if (!IsStopGram(gram)) {
            filtered.push_back(gram);
        }
    }
    return filtered;
}

std::vector<std::uint32_t> NgramIndex::DecodePostingList(const GramPosting& posting) const {
    std::vector<std::uint32_t> values;
    values.reserve(posting.count);
    if (!ForEachPostingValue(posting, [&values](std::uint32_t value) {
            values.push_back(value);
            return true;
        })) {
        return {};
    }
    return values;
}

bool NgramIndex::ForEachPostingValue(
    const GramPosting& posting,
    const std::function<bool(std::uint32_t)>& callback) const {
    if (mapped_view_ != nullptr) {
        if (posting.byte_offset > mapped_compressed_posting_bytes_ ||
            posting.byte_size > mapped_compressed_posting_bytes_ - posting.byte_offset) {
            return false;
        }
    } else if (posting.byte_offset > compressed_postings_.size() ||
               posting.byte_size > compressed_postings_.size() - posting.byte_offset) {
        return false;
    }
    const std::uint8_t* data = (mapped_view_ != nullptr ? mapped_compressed_postings_ : compressed_postings_.data()) + posting.byte_offset;
    return ForEachEncodedPostingValue(data, posting.byte_size, posting.count, callback);
}

std::vector<std::size_t> NgramIndex::Query(std::wstring_view query) const {
    return QueryLowered(WideToLowerUtf8(query));
}

std::vector<std::size_t> NgramIndex::QueryLowered(std::string_view query_lower) const {
    const std::vector<std::uint32_t> grams = MakeGrams(query_lower);
    if (grams.empty()) {
        return {};
    }
    const GramPosting* smallest = nullptr;
    for (const std::uint32_t gram : grams) {
        const GramPosting* found = FindPosting(gram);
        if (found == nullptr) {
            return {};
        }
        if (smallest == nullptr || found->count < smallest->count) {
            smallest = found;
        }
    }

    const std::vector<std::uint32_t> smallest_values = DecodePostingList(*smallest);
    std::vector<std::size_t> candidates;
    candidates.reserve(smallest->count);
    for (const std::uint32_t value : smallest_values) {
        candidates.push_back(value);
    }

    for (const std::uint32_t gram : grams) {
        const GramPosting* found = FindPosting(gram);
        if (found == smallest) {
            continue;
        }
        std::vector<std::size_t> intersected;
        intersected.reserve(std::min<std::size_t>(candidates.size(), found->count));
        std::size_t candidate_pos = 0;
        const bool decoded_ok = ForEachPostingValue(*found, [&](std::uint32_t value) {
            while (candidate_pos < candidates.size() && candidates[candidate_pos] < value) {
                ++candidate_pos;
            }
            if (candidate_pos >= candidates.size()) {
                return false;
            }
            if (candidates[candidate_pos] == value) {
                intersected.push_back(candidates[candidate_pos]);
                ++candidate_pos;
            }
            return true;
        });
        if (!decoded_ok) {
            return {};
        }
        candidates = std::move(intersected);
        if (candidates.empty()) {
            break;
        }
    }
    return candidates;
}

namespace {

template <typename GramContainer>
std::vector<std::size_t> QueryFuzzyImpl(
    const NgramIndex::GramPosting* grams_index,
    std::size_t grams_index_count,
    const NgramIndex& index,
    const GramContainer& grams,
    std::size_t max_missing) {

    if (grams.empty()) {
        return {};
    }

    // Collect posting list pointers for existing grams
    struct GramInfo {
        const NgramIndex::GramPosting* posting = nullptr;
    };
    std::vector<GramInfo> gram_infos;
    gram_infos.reserve(grams.size());
    std::size_t missing = 0;

    for (const std::uint32_t gram : grams) {
        const auto found = std::lower_bound(
            grams_index, grams_index + grams_index_count, gram,
            [](const NgramIndex::GramPosting& posting, std::uint32_t value) {
                return posting.gram < value;
            });
        if (found == grams_index + grams_index_count || found->gram != gram) {
            ++missing;
            gram_infos.push_back({});
        } else {
            gram_infos.push_back({&*found});
        }
    }

    if (missing > max_missing || missing == grams.size()) {
        return {};
    }

    const std::size_t total_grams = grams.size();
    const std::size_t min_matches = max_missing >= total_grams ? 1 : total_grams - max_missing;

    // Estimate total unique files across all posting lists for reserve
    std::size_t estimated_entries = 0;
    for (std::size_t g = 0; g < gram_infos.size(); ++g) {
        if (gram_infos[g].posting != nullptr) {
            estimated_entries += gram_infos[g].posting->count;
        }
    }
    // Heuristic: unique entries ≈ total / 3 (many files share multiple grams)
    estimated_entries = std::max<std::size_t>(1, estimated_entries / 3);

    // Union-count: count how many query grams each file across ALL posting lists has.
    // This correctly handles the case where a file is not in every posting list.
    std::unordered_map<std::size_t, std::size_t> match_counts;
    match_counts.reserve(estimated_entries);
    for (std::size_t g = 0; g < gram_infos.size(); ++g) {
        if (gram_infos[g].posting == nullptr) {
            continue;
        }
        const bool decoded_ok = index.ForEachPostingValue(*gram_infos[g].posting, [&](std::uint32_t value) {
            const std::size_t file_idx = value;
            ++match_counts[file_idx];
            return true;
        });
        if (!decoded_ok) {
            return {};
        }
    }

    // Filter to files with enough matches and sort by match count descending
    std::vector<std::pair<std::size_t, std::size_t>> scored;
    scored.reserve(match_counts.size());
    for (const auto& [file_idx, count] : match_counts) {
        if (count >= min_matches) {
            scored.emplace_back(file_idx, count);
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::size_t> candidates;
    candidates.reserve(scored.size());
    for (const auto& [file_idx, count] : scored) {
        candidates.push_back(file_idx);
    }
    return candidates;
}

}  // namespace

std::vector<std::size_t> NgramIndex::QueryFuzzy(std::wstring_view query,
                                                  std::size_t max_missing) const {
    return QueryFuzzyLowered(WideToLowerUtf8(query), max_missing);
}

std::vector<std::size_t> NgramIndex::QueryFuzzyLowered(std::string_view query_lower,
                                                         std::size_t max_missing) const {
    if (query_lower.size() <= kShortQueryFuzzyThreshold) {
            return {};
    }
    std::vector<std::uint32_t> trigrams = MakeGrams(query_lower, kGramSize);
    const GramPosting* grams = mapped_view_ != nullptr ? mapped_grams_ : grams_.data();
    const std::size_t gram_count = mapped_view_ != nullptr ? static_cast<std::size_t>(mapped_gram_count_) : grams_.size();
    if (gram_count == 0) {
        return {};
    }
    return QueryFuzzyImpl(grams, gram_count, *this, trigrams, max_missing);
}

bool NgramSearchIndex::LoadOrBuild(const MftSnapshot& snapshot,
                                   const AutoFileSearchConfig& config,
                                   WalPosition wal_pos,
                                   std::wstring* error) {
    const auto total_start = Clock::now();
    {
        std::ostringstream out;
        out << "[ngram] LoadOrBuild begin"
            << " snapshot_entries=" << snapshot.entries.size()
            << " mutation_version=" << snapshot.mutation_version
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }
    config_ = config;
    wal_position_ = wal_pos;
    file_name_store_path_ = GlobalFileNameStorePath(config.cache_directory);
    frn_store_path_ = GlobalFrnStorePath(config.cache_directory);
    parent_store_path_ = GlobalParentStorePath(config.cache_directory);
    dir_parent_store_path_ = GlobalDirParentStorePath(config.cache_directory);
    ngram_index_path_ = GlobalNgramIndexPath(config.cache_directory);

    volume_roots_.clear();
    directory_count_ = 0;
    mutation_version_ = snapshot.mutation_version;
    for (const MftSnapshotVolume& volume : snapshot.volumes) {
        volume_roots_[static_cast<wchar_t>(std::towupper(volume.drive))] = volume.volume_root;
    }
    for (const MftEntryRecord& entry : snapshot.entries) {
        if (entry.is_directory) {
            ++directory_count_;
        }
    }

    auto mapped_ngram = std::make_shared<NgramIndex>();
    const bool names_exists = std::filesystem::exists(std::filesystem::path(file_name_store_path_));
    const bool frns_open = base_frns_.Open(frn_store_path_);
    const bool parents_open = base_parents_.Open(parent_store_path_);
    const bool dir_parents_open = dir_parents_.Open(dir_parent_store_path_);
    const bool index_loaded = mapped_ngram->LoadMapped(ngram_index_path_);
    {
        std::ostringstream out;
        out << "[ngram] LoadOrBuild mmap check"
            << " names_exists=" << names_exists
            << " frns_open=" << frns_open
            << " parents_open=" << parents_open
            << " dir_parents_open=" << dir_parents_open
            << " index_loaded=" << index_loaded
            << " base_frns=" << (frns_open ? base_frns_.Size() : 0)
            << " base_parents=" << (parents_open ? base_parents_.Size() : 0)
            << " dirs=" << (dir_parents_open ? dir_parents_.Size() : 0)
            << " base_wal_generation=" << (frns_open ? base_frns_.WalPosition().generation : 0)
            << " current_wal_generation=" << wal_pos.generation
            << " snapshot_entries=" << snapshot.entries.size()
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }
    if (names_exists && frns_open && parents_open && dir_parents_open &&
        base_parents_.Size() == base_frns_.Size() &&
        base_parents_.MutationVersion() == base_frns_.MutationVersion() &&
        dir_parents_.MutationVersion() == base_frns_.MutationVersion() &&
        base_parents_.WalPosition().generation == base_frns_.WalPosition().generation &&
        base_parents_.WalPosition().offset == base_frns_.WalPosition().offset &&
        dir_parents_.WalPosition().generation == base_frns_.WalPosition().generation &&
        dir_parents_.WalPosition().offset == base_frns_.WalPosition().offset &&
        index_loaded) {
        ngram_index_ = mapped_ngram;
        lowmem_base_file_count_ = base_frns_.Size();
        wal_position_ = base_frns_.WalPosition();
        invalid_frns_.clear();
        ready_ = true;
        LogNgramDone("LoadOrBuild mmap fast path done", total_start);
        return true;
    }
    base_frns_.Close();
    base_parents_.Close();
    dir_parents_.Close();
    // A stale index may still have an active file mapping here. Windows does not
    // allow SaveToFile to truncate a file while a mapped view of it exists.
    mapped_ngram.reset();

    LogNgram("LoadOrBuild rebuild path begin");
    if (!BuildLowMemoryFromSnapshot(snapshot, error)) {
        if (error != nullptr && !error->empty()) {
            LogLine("[ngram] LoadOrBuild rebuild error=" + WideToUtf8(*error));
        }
        LogNgramDone("LoadOrBuild rebuild path failed", total_start);
        return false;
    }
    ready_ = true;
    LogNgramDone("LoadOrBuild rebuild path done", total_start);
    return true;
}

bool NgramSearchIndex::RebuildFromSnapshot(const MftSnapshot& snapshot,
                                           const AutoFileSearchConfig& config,
                                           WalPosition wal_pos,
                                           std::wstring* error) {
    config_ = config;
    wal_position_ = wal_pos;
    file_name_store_path_ = GlobalFileNameStorePath(config.cache_directory);
    frn_store_path_ = GlobalFrnStorePath(config.cache_directory);
    parent_store_path_ = GlobalParentStorePath(config.cache_directory);
    dir_parent_store_path_ = GlobalDirParentStorePath(config.cache_directory);
    ngram_index_path_ = GlobalNgramIndexPath(config.cache_directory);
    if (!BuildLowMemoryFromSnapshot(snapshot, error)) {
        return false;
    }
    ready_ = true;
    return true;
}

void NgramSearchIndex::Stop(const std::vector<IndexChange>& changes) {
    ApplyPending(changes);
    Clear();
}

void NgramSearchIndex::Clear() {
    std::unique_lock state_lock(state_mutex_);
    mapped_names_.Close();
    {
        std::lock_guard volume_lock(system_volume_mutex_);
        system_volume_handles_.clear();
    }
    queued_changes_.clear();
    volume_roots_.clear();
    mutation_version_ = 0;
    base_frns_.Close();
    base_parents_.Close();
    dir_parents_.Close();
    directory_count_ = 0;
    ngram_index_ = std::make_shared<NgramIndex>();
    invalid_frns_.clear();
    lowmem_base_file_count_ = 0;
    TrimCrtHeap();
    ready_ = false;
}

bool NgramSearchIndex::BuildLowMemoryFromSnapshot(const MftSnapshot& snapshot, std::wstring* error) {
    const auto total_start = Clock::now();
    {
        std::ostringstream out;
        out << "[ngram] BuildLowMemoryFromSnapshot begin"
            << " snapshot_entries=" << snapshot.entries.size()
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }
    Clear();

    std::size_t entry_count = 0;
    for (const MftSnapshotVolume& volume : snapshot.volumes) {
        volume_roots_[static_cast<wchar_t>(std::towupper(volume.drive))] = volume.volume_root;
    }
    mutation_version_ = snapshot.mutation_version;
    for (const MftEntryRecord& entry : snapshot.entries) {
        if (entry.is_directory) {
            ++directory_count_;
        }
        ++entry_count;
    }

    std::vector<std::string> names;
    std::vector<Frn> frns;
    std::vector<Frn> parent_frns;
    std::vector<DirParentEntry> dir_parent_entries;
    names.reserve(entry_count);
    frns.reserve(entry_count);
    parent_frns.reserve(entry_count);
    dir_parent_entries.reserve(directory_count_);

    const auto collect_start = Clock::now();
    for (const MftEntryRecord& entry : snapshot.entries) {
        if (frns.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            break;
        }
        const std::string name(snapshot.Name(entry));
        frns.push_back(entry.frn);
        parent_frns.push_back(entry.parent_frn);
        names.push_back(name);
        if (entry.is_directory && entry.frn != 0) {
            dir_parent_entries.push_back(DirParentEntry{StripSearchFrnFlags(entry.frn),
                                                        StripSearchFrnFlags(entry.parent_frn)});
        }
    }
    {
        std::ostringstream out;
        out << "[ngram] BuildLowMemory collect names done"
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - collect_start).count()
            << " names=" << names.size()
            << " private_mb=" << ProcessPrivateMb();
        LogLine(out.str());
    }

    std::sort(dir_parent_entries.begin(), dir_parent_entries.end(),
              [](const DirParentEntry& left, const DirParentEntry& right) {
                  return left.frn < right.frn;
              });
    dir_parent_entries.erase(std::unique(dir_parent_entries.begin(),
                                         dir_parent_entries.end(),
                                         [](const DirParentEntry& left, const DirParentEntry& right) {
                                             return left.frn == right.frn;
                                         }),
                             dir_parent_entries.end());

    const auto sort_start = Clock::now();
    if (!SortNamesFrnsAndParents(&names, &frns, &parent_frns, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory sort names/frns done", sort_start);

    const auto flag_start = Clock::now();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const bool has_same_name_next = i + 1 < names.size() && names[i + 1] == names[i];
        frns[i] = SetSameNameNext(frns[i], has_same_name_next);
    }
    LogNgramDone("BuildLowMemory same-name flags done", flag_start);

    const auto save_names_start = Clock::now();
    if (!SaveFileNameStoreFile(file_name_store_path_, names, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory save fsenames done", save_names_start);
    const auto save_frns_start = Clock::now();
    if (!SaveFrnStoreFile(frn_store_path_, frns, mutation_version_, wal_position_, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory save frns done", save_frns_start);
    const auto save_parents_start = Clock::now();
    if (!SaveParentStoreFile(parent_store_path_, parent_frns, mutation_version_, wal_position_, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory save parents done", save_parents_start);
    const auto save_dir_parents_start = Clock::now();
    if (!SaveDirParentStoreFile(dir_parent_store_path_, dir_parent_entries, mutation_version_, wal_position_, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory save dir parents done", save_dir_parents_start);
    auto new_ngram = std::make_shared<NgramIndex>();
    const auto build_index_start = Clock::now();
    BuildNgramFromSortedNames(names, new_ngram.get());
    LogNgramDone("BuildLowMemory build ngram done", build_index_start);
    const auto save_index_start = Clock::now();
    if (!new_ngram->SaveToFile(ngram_index_path_, error)) {
        return false;
    }
    LogNgramDone("BuildLowMemory save ngram index done", save_index_start);
    auto mapped_ngram = std::make_shared<NgramIndex>();
    const auto map_index_start = Clock::now();
    if (!mapped_ngram->LoadMapped(ngram_index_path_)) {
        ngram_index_ = new_ngram;
    } else {
        ngram_index_ = mapped_ngram;
    }
    LogNgramDone("BuildLowMemory map ngram index done", map_index_start);
    const auto map_frn_start = Clock::now();
    if (!base_frns_.Open(frn_store_path_)) {
        if (error != nullptr) {
            *error = L"Failed to map FRN store.";
        }
        return false;
    }
    LogNgramDone("BuildLowMemory map frns done", map_frn_start);
    const auto map_parent_start = Clock::now();
    if (!base_parents_.Open(parent_store_path_)) {
        if (error != nullptr) {
            *error = L"Failed to map parent store.";
        }
        return false;
    }
    LogNgramDone("BuildLowMemory map parents done", map_parent_start);
    const auto map_dir_parent_start = Clock::now();
    if (!dir_parents_.Open(dir_parent_store_path_)) {
        if (error != nullptr) {
            *error = L"Failed to map directory-parent store.";
        }
        return false;
    }
    LogNgramDone("BuildLowMemory map dir parents done", map_dir_parent_start);
    TrimCrtHeap();
    LogNgram("BuildLowMemory after TrimCrtHeap");
    invalid_frns_.clear();
    lowmem_base_file_count_ = base_frns_.Size();
    LogNgramDone("BuildLowMemoryFromSnapshot done", total_start);
    return true;
}

void NgramSearchIndex::ApplyPending(const std::vector<IndexChange>& changes) {
    std::unique_lock state_lock(state_mutex_);
    ApplyPendingLocked(changes);
}

void NgramSearchIndex::ApplyPendingLocked(const std::vector<IndexChange>& changes) {
    if (!ready_) {
        queued_changes_.insert(queued_changes_.end(), changes.begin(), changes.end());
        return;
    }
    if (!queued_changes_.empty()) {
        std::vector<IndexChange> combined;
        combined.reserve(queued_changes_.size() + changes.size());
        combined.insert(combined.end(), queued_changes_.begin(), queued_changes_.end());
        combined.insert(combined.end(), changes.begin(), changes.end());
        queued_changes_.clear();
        ApplyPendingLocked(combined);
        return;
    }
    for (const IndexChange& change : changes) {
        if (change.remove) {
            RemoveEntry(change.entry.frn, change.entry.is_directory);
        } else {
            UpsertEntry(change.entry);
        }
    }
}

WalPosition NgramSearchIndex::GetWalPosition() const { return wal_position_; }
void NgramSearchIndex::SetWalPosition(WalPosition pos) { wal_position_ = pos; }

std::vector<SearchResult> NgramSearchIndex::Search(std::wstring_view query,
                                                   std::size_t max_results,
                                                   SearchPhase phase,
                                                   const SearchScope& scope) {
    std::shared_lock state_lock(state_mutex_);
    std::vector<SearchResult> results;
    if (!ready_ || query.empty() || max_results == 0) {
        return results;
    }
    const std::string query_lower = WideToLowerUtf8(query);
    auto ngram = ngram_index_;
    std::vector<std::size_t> candidates;
    if (ngram && !ngram->Empty() && ngram->CanQuery(query_lower)) {
        candidates = phase == SearchPhase::FuzzyOnly
            ? ngram->QueryFuzzyLowered(query_lower, query_lower.size() <= 5 ? 1 : 2)
            : ngram->QueryLowered(query_lower);
    }

    const bool use_system_names = candidates.size() <= 1000;
    const bool use_mapped_names = !use_system_names;
    std::unordered_set<std::size_t> seen;
    seen.reserve(candidates.size());
    std::unordered_map<Frn, bool> scope_membership_cache;
    if (scope.enabled) {
        scope_membership_cache.reserve(1024);
    }
    struct RankedSearchResult {
        SearchResult result;
        std::size_t rank = 0;
        std::size_t expansion = 0;
    };
    auto append_result = [&](std::size_t i,
                             std::size_t rank,
                             std::size_t expansion,
                             std::vector<RankedSearchResult>* ranked_results) {
        if (i >= TotalFileCount() || !seen.insert(i).second) {
            return;
        }
        if (scope.enabled && !IsInScope(i, scope, &scope_membership_cache)) {
            return;
        }
        std::string text = CandidateText(i, use_mapped_names, use_system_names);
        if (text.empty()) {
            return;
        }
        bool matched = false;
        if (phase == SearchPhase::FuzzyOnly) {
            const int max_distance = query_lower.size() <= 5 ? 1 : 2;
            matched = FuzzyDistance(text, query_lower, max_distance) <= max_distance;
        } else {
            matched = text.find(query_lower) != std::string::npos;
        }
        if (!matched) {
            return;
        }

        SearchResult result;
        result.frn = StripSearchFrnFlags(FrnAt(i));
        if (IsFrnInvalid(result.frn)) {
            return;
        }
        result.path = PathFromSystem(i);
        if (result.path.empty()) {
            return;
        }
        if (!PathMatchesScope(result.path, scope)) {
            return;
        }
        const std::wstring_view name_from_path = FileNamePart(result.path);
        if (name_from_path.empty()) {
            return;
        }
        const std::string current_text = CandidateTextForName(WideToLowerUtf8(name_from_path));
        if (phase == SearchPhase::FuzzyOnly) {
            const int max_distance = query_lower.size() <= 5 ? 1 : 2;
            if (FuzzyDistance(current_text, query_lower, max_distance) > max_distance) {
                return;
            }
        } else if (current_text.find(query_lower) == std::string::npos) {
            return;
        }
        result.name.assign(name_from_path.data(), name_from_path.size());
        if (ranked_results != nullptr) {
            ranked_results->push_back(RankedSearchResult{std::move(result), rank, expansion});
        } else {
            results.push_back(std::move(result));
        }
    };

    if (phase == SearchPhase::FuzzyOnly) {
        struct CandidateRead {
            std::size_t index = 0;
            std::size_t rank = 0;
            std::size_t expansion = 0;
        };
        std::vector<CandidateRead> read_plan;
        read_plan.reserve(candidates.size());
        for (std::size_t rank = 0; rank < candidates.size(); ++rank) {
            std::size_t i = candidates[rank];
            if (i >= TotalFileCount()) {
                continue;
            }
            std::size_t expansion = 0;
            read_plan.push_back(CandidateRead{i, rank, expansion});
            if (i >= BaseFileCount()) {
                continue;
            }
            while (i < BaseFileCount() && HasSameNameNext(FrnAt(i))) {
                ++i;
                ++expansion;
                read_plan.push_back(CandidateRead{i, rank, expansion});
            }
        }
        std::sort(read_plan.begin(), read_plan.end(),
                  [](const CandidateRead& left, const CandidateRead& right) {
                      if (left.index != right.index) {
                          return left.index < right.index;
                      }
                      if (left.rank != right.rank) {
                          return left.rank < right.rank;
                      }
                      return left.expansion < right.expansion;
                  });

        std::vector<RankedSearchResult> ranked_results;
        ranked_results.reserve((std::min)(read_plan.size(), max_results));
        for (const CandidateRead& item : read_plan) {
            append_result(item.index, item.rank, item.expansion, &ranked_results);
        }
        std::sort(ranked_results.begin(), ranked_results.end(),
                  [](const RankedSearchResult& left, const RankedSearchResult& right) {
                      if (left.rank != right.rank) {
                          return left.rank < right.rank;
                      }
                      return left.expansion < right.expansion;
                  });
        results.reserve((std::min)(ranked_results.size(), max_results));
        for (RankedSearchResult& item : ranked_results) {
            if (results.size() >= max_results) {
                break;
            }
            results.push_back(std::move(item.result));
        }
        return results;
    }

    for (std::size_t i : candidates) {
        if (i >= TotalFileCount() || results.size() >= max_results) {
            continue;
        }
        append_result(i, 0, 0, nullptr);
        if (i >= BaseFileCount()) {
            continue;
        }
        while (i < BaseFileCount() && HasSameNameNext(FrnAt(i)) && results.size() < max_results) {
            ++i;
            append_result(i, 0, 0, nullptr);
        }
    }
    return results;
}

bool NgramSearchIndex::Ready() const {
    return ready_;
}

std::size_t NgramSearchIndex::FileCount() const {
    return ready_ ? TotalFileCount() : 0;
}

std::size_t NgramSearchIndex::DirectoryCount() const {
    return ready_ ? directory_count_ : 0;
}

bool NgramSearchIndex::SupportsFuzzy() const {
    return true;
}

std::size_t NgramSearchIndex::BaseFileCount() const {
    return base_frns_.Size();
}

std::size_t NgramSearchIndex::TotalFileCount() const {
    return base_frns_.Size();
}

Frn NgramSearchIndex::FrnAt(std::size_t file_index) const {
    const std::size_t base_count = BaseFileCount();
    if (file_index < base_count) {
        return base_frns_.Get(file_index);
    }
    return 0;
}

Frn NgramSearchIndex::ParentFrnAt(std::size_t file_index) const {
    const std::size_t base_count = BaseFileCount();
    if (file_index < base_count) {
        return base_parents_.Get(file_index);
    }
    return 0;
}

bool NgramSearchIndex::IsInScope(std::size_t file_index,
                                 const SearchScope& scope,
                                 std::unordered_map<Frn, bool>* membership_cache) const {
    if (!scope.enabled) {
        return true;
    }
    Frn current = StripSearchFrnFlags(FrnAt(file_index));
    const Frn scope_frn = StripSearchFrnFlags(scope.root_frn);
    if (current == 0 || scope_frn == 0 || DecodeFrnDrive(current) != scope.drive) {
        return false;
    }
    if (current == scope_frn) {
        return true;
    }

    current = StripSearchFrnFlags(ParentFrnAt(file_index));
    std::vector<Frn> visited;
    visited.reserve(16);
    while (current != 0 && current != kRootParentFrn) {
        if (current == scope_frn) {
            if (membership_cache != nullptr) {
                for (Frn frn : visited) {
                    (*membership_cache)[frn] = true;
                }
            }
            return true;
        }
        if (membership_cache != nullptr) {
            const auto cached = membership_cache->find(current);
            if (cached != membership_cache->end()) {
                const bool result = cached->second;
                for (Frn frn : visited) {
                    (*membership_cache)[frn] = result;
                }
                return result;
            }
        }
        visited.push_back(current);
        current = StripSearchFrnFlags(dir_parents_.ParentOf(current));
    }
    if (membership_cache != nullptr) {
        for (Frn frn : visited) {
            (*membership_cache)[frn] = false;
        }
    }
    return false;
}

std::string NgramSearchIndex::FileNameForSearch(std::size_t file_index, bool prefer_mapped) const {
    const std::size_t base_count = BaseFileCount();
    if (file_index >= base_count) {
        return {};
    }

    return mapped_names_.Get(file_name_store_path_, file_index);
}

std::string NgramSearchIndex::FileNameFromSystem(std::size_t file_index) const {
    const std::wstring path = PathFromSystem(file_index);
    if (path.empty()) {
        return {};
    }
    return WideToLowerUtf8(FileNamePart(path));
}

std::wstring NgramSearchIndex::PathFromSystem(std::size_t file_index) const {
    const Frn encoded_frn = StripSearchFrnFlags(FrnAt(file_index));
    if (encoded_frn == 0) {
        return {};
    }
    return PathFromFileId(SystemVolumeHandle(encoded_frn), DecodeRawFrn(encoded_frn));
}

void* NgramSearchIndex::SystemVolumeHandle(Frn encoded_frn) const {
    encoded_frn = StripSearchFrnFlags(encoded_frn);
    const wchar_t drive = DecodeFrnDrive(encoded_frn);
    if (drive == L'\0') {
        return nullptr;
    }
    std::lock_guard volume_lock(system_volume_mutex_);
    auto found = system_volume_handles_.find(drive);
    if (found != system_volume_handles_.end() && found->second.Valid()) {
        return found->second.Get();
    }
    std::wstring root;
    const auto root_found = volume_roots_.find(drive);
    if (root_found != volume_roots_.end()) {
        root = root_found->second;
    } else {
        root = std::wstring(1, drive) + L":";
    }
    UniqueHandle handle = OpenVolumeByRoot(root);
    if (!handle.Valid()) {
        return nullptr;
    }
    void* raw = handle.Get();
    system_volume_handles_[drive] = std::move(handle);
    return raw;
}

bool NgramSearchIndex::IsFrnInvalid(Frn encoded_frn) const {
    const Frn frn = StripSearchFrnFlags(encoded_frn);
    return frn == 0 || invalid_frns_.find(frn) != invalid_frns_.end();
}

void NgramSearchIndex::UpsertEntry(const RawEntry& entry) {
    if (entry.frn == 0 || entry.name.empty()) {
        return;
    }
    invalid_frns_.erase(StripSearchFrnFlags(entry.frn));
    if (entry.is_directory) {
        ++directory_count_;
        ++mutation_version_;
        return;
    }
    ++mutation_version_;
}

void NgramSearchIndex::RemoveEntry(Frn frn, bool is_directory) {
    invalid_frns_.insert(StripSearchFrnFlags(frn));
    if (is_directory) {
        if (directory_count_ != 0) {
            --directory_count_;
        }
    }
    ++mutation_version_;
}

std::string NgramSearchIndex::CandidateText(std::size_t file_index,
                                            bool prefer_mapped,
                                            bool use_system_name) const {
    if (use_system_name) {
        return CandidateTextForName(FileNameFromSystem(file_index));
    }
    return CandidateTextForName(FileNameForSearch(file_index, prefer_mapped));
}


}  // namespace fsengine



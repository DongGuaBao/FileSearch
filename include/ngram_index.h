#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fsengine {

struct NgramIndexStats {
    std::size_t gram_count = 0;
    std::size_t posting_count = 0;
    std::size_t gram_bytes = 0;
    std::size_t posting_bytes = 0;
    std::size_t posting_raw_bytes = 0;
    std::size_t total_bytes = 0;
    double posting_compression_ratio = 0.0;
    std::size_t trigram_gram_count = 0;
    std::size_t trigram_posting_count = 0;
    std::size_t build_temp_posting_count = 0;
    std::size_t build_temp_posting_capacity = 0;
    std::size_t stop_gram_count = 0;
    std::size_t stop_posting_count = 0;
    std::size_t stop_df_threshold_count = 0;
    std::size_t stop_top_count = 0;
};

struct NgramBuildEntry {
    std::uint32_t index = 0;
    std::string text;
};

class FSENGINE_API NgramIndex {
public:
    ~NgramIndex();

    void BuildFromTexts(const std::vector<std::string>& candidate_texts);
    void BuildFromEntries(const std::vector<NgramBuildEntry>& candidate_texts);
    void BuildFromEntries(std::size_t entry_count,
                          const std::function<NgramBuildEntry(std::size_t)>& entry_provider);
    bool SaveToFile(const std::wstring& path, std::wstring* error) const;
    bool LoadMapped(const std::wstring& path);
    void CloseMapped();

    // Public API — converts query to lower internally
    std::vector<std::size_t> Query(std::wstring_view query) const;
    std::vector<std::size_t> QueryFuzzy(std::wstring_view query,
                                        std::size_t max_missing) const;

    // Fast paths for callers that already have a lowered query
    std::vector<std::size_t> QueryLowered(std::string_view query_lower) const;
    std::vector<std::size_t> QueryFuzzyLowered(std::string_view query_lower,
                                               std::size_t max_missing) const;

    bool CanQuery(std::string_view query) const;
    bool Empty() const;
    NgramIndexStats Stats() const;
    std::size_t IndexedFileCount() const { return indexed_file_count_; }

    struct GramPosting {
        std::uint32_t gram = 0;
        std::uint32_t byte_offset = 0;
        std::uint32_t count = 0;
        std::uint32_t byte_size = 0;
    };

    std::vector<std::uint32_t> DecodePostingList(const GramPosting& posting) const;
    bool ForEachPostingValue(const GramPosting& posting,
                             const std::function<bool(std::uint32_t)>& callback) const;

private:
    const GramPosting* FindPosting(std::uint32_t gram) const;
    bool IsStopGram(std::uint32_t gram) const;
    std::vector<std::uint32_t> FilterStopGrams(const std::vector<std::uint32_t>& grams) const;

    std::vector<GramPosting> grams_;
    std::vector<std::uint32_t> stop_grams_;
    std::vector<std::uint8_t> compressed_postings_;
    void* mapped_file_ = nullptr;
    void* mapped_mapping_ = nullptr;
    const char* mapped_view_ = nullptr;
    std::uint64_t mapped_size_ = 0;
    const GramPosting* mapped_grams_ = nullptr;
    std::uint64_t mapped_gram_count_ = 0;
    const std::uint32_t* mapped_stop_grams_ = nullptr;
    std::uint64_t mapped_stop_gram_count_ = 0;
    const std::uint8_t* mapped_compressed_postings_ = nullptr;
    std::uint64_t mapped_compressed_posting_bytes_ = 0;
    std::size_t build_temp_posting_count_ = 0;
    std::size_t build_temp_posting_capacity_ = 0;
    std::size_t stop_posting_count_ = 0;
    std::size_t stop_df_threshold_count_ = 0;
    std::size_t stop_top_count_ = 0;
    std::size_t indexed_file_count_ = 0;
};

}  // namespace fsengine

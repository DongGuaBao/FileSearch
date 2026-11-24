#pragma once

#include "flat_map.h"
#include "mft_snapshot.h"
#include "search_index.h"

#include <cstdint>
#include <unordered_map>

namespace fsengine {

struct FilterIndexDir {
    Frn parent_frn = kRootParentFrn;
    std::string name;
};

struct FilterIndexFile {
    Frn frn = 0;
    Frn parent_frn = kRootParentFrn;
    std::string name;
    NameFilter filter;
};

class FilterIndex final : public SearchIndex {
public:
    bool LoadOrBuild(const MftSnapshot& snapshot,
                     const AutoFileSearchConfig& config,
                     WalPosition wal_pos,
                     std::wstring* error) override;
    bool RebuildFromSnapshot(const MftSnapshot& snapshot,
                             const AutoFileSearchConfig& config,
                             WalPosition wal_pos,
                             std::wstring* error) override;
    void Stop(const std::vector<IndexChange>& changes) override;
    void ApplyPending(const std::vector<IndexChange>& changes) override;
    std::vector<SearchResult> Search(std::wstring_view query,
                                     std::size_t max_results,
                                     SearchPhase phase,
                                     const SearchScope& scope) override;
    bool Ready() const override;
    std::size_t FileCount() const override;
    std::size_t DirectoryCount() const override;
    bool SupportsFuzzy() const override;
    WalPosition GetWalPosition() const override;
    void SetWalPosition(WalPosition pos) override;

private:
    void Clear();
    void BuildFromSnapshot(const MftSnapshot& snapshot);
    void UpsertEntry(const RawEntry& entry);
    void RemoveEntry(Frn frn, bool is_directory);
    std::wstring BuildPathForFile(std::size_t file_index) const;
    NameFilter QueryFilter(std::string_view lowered_query) const;

    AutoFileSearchConfig config_;
    std::vector<IndexChange> queued_changes_;
    std::unordered_map<wchar_t, std::wstring> volume_roots_;
    std::uint64_t usn_journal_id_ = 0;
    std::int64_t next_usn_ = 0;
    std::uint64_t mutation_version_ = 0;
    FilterDictionary dictionary_;
    FlatMap<Frn, FilterIndexDir> dirs_;
    FlatMap<Frn, std::uint32_t> frn_to_file_index_;
    std::vector<FilterIndexFile> files_;
    fsengine::WalPosition wal_position_;
    bool ready_ = false;
};

}  // namespace fsengine

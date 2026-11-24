#pragma once

#include "auto_file_search.h"
#include "mft_snapshot.h"
#include "types.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fsengine {

class MftService;

class SearchIndex {
public:
    virtual ~SearchIndex() = default;

    virtual bool LoadOrBuild(const MftSnapshot& snapshot,
                             const AutoFileSearchConfig& config,
                             WalPosition wal_pos,
                             std::wstring* error) = 0;
    virtual bool RebuildFromSnapshot(const MftSnapshot& snapshot,
                                     const AutoFileSearchConfig& config,
                                     WalPosition wal_pos,
                                     std::wstring* error) = 0;
    virtual void Stop(const std::vector<IndexChange>& changes) = 0;
    virtual void ApplyPending(const std::vector<IndexChange>& changes) = 0;
    virtual WalPosition GetWalPosition() const = 0;
    virtual void SetWalPosition(WalPosition pos) = 0;
    virtual std::vector<SearchResult> Search(std::wstring_view query,
                                             std::size_t max_results,
                                             SearchPhase phase,
                                             const SearchScope& scope) = 0;
    virtual bool Ready() const = 0;
    virtual std::size_t FileCount() const = 0;
    virtual std::size_t DirectoryCount() const = 0;
    virtual bool SupportsFuzzy() const = 0;
};

std::unique_ptr<SearchIndex> CreateSearchIndex(SearchMode mode);

}  // namespace fsengine

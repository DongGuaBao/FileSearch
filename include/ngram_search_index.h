#pragma once

#include "file_name_store.h"
#include "mft_snapshot.h"
#include "ngram_index.h"
#include "search_index.h"
#include "win_handle.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace fsengine {

class NgramSearchIndex final : public SearchIndex {
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
    bool BuildLowMemoryFromSnapshot(const MftSnapshot& snapshot, std::wstring* error);
    void ApplyPendingLocked(const std::vector<IndexChange>& changes);
    std::string FileNameForSearch(std::size_t file_index, bool prefer_mapped) const;
    std::string FileNameFromSystem(std::size_t file_index) const;
    std::wstring PathFromSystem(std::size_t file_index) const;
    void* SystemVolumeHandle(Frn encoded_frn) const;
    bool IsFrnInvalid(Frn encoded_frn) const;
    bool IsInScope(std::size_t file_index,
                   const SearchScope& scope,
                   std::unordered_map<Frn, bool>* membership_cache) const;
    std::size_t BaseFileCount() const;
    std::size_t TotalFileCount() const;
    Frn FrnAt(std::size_t file_index) const;
    Frn ParentFrnAt(std::size_t file_index) const;
    void UpsertEntry(const RawEntry& entry);
    void RemoveEntry(Frn frn, bool is_directory);
    std::string CandidateText(std::size_t file_index, bool prefer_mapped, bool use_system_name) const;

    AutoFileSearchConfig config_;
    std::vector<IndexChange> queued_changes_;
    std::unordered_map<wchar_t, std::wstring> volume_roots_;
    std::uint64_t mutation_version_ = 0;
    MappedFrnStore base_frns_;
    MappedParentStore base_parents_;
    MappedDirParentStore dir_parents_;
    WalPosition wal_position_;
    std::size_t directory_count_ = 0;
    std::wstring file_name_store_path_;
    std::wstring frn_store_path_;
    std::wstring parent_store_path_;
    std::wstring dir_parent_store_path_;
    std::wstring ngram_index_path_;
    std::shared_ptr<NgramIndex> ngram_index_;
    std::unordered_set<Frn> invalid_frns_;
    std::size_t lowmem_base_file_count_ = 0;
    bool ready_ = false;

    mutable std::shared_mutex state_mutex_;
    mutable MappedFileNameStore mapped_names_;
    mutable std::mutex system_volume_mutex_;
    mutable std::unordered_map<wchar_t, UniqueHandle> system_volume_handles_;
};

}  // namespace fsengine

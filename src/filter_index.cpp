#include "filter_index.h"

#include "file_id_codec.h"
#include "mft_service.h"
#include "ngram_search_index.h"
#include "utf8.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace fsengine {

namespace {

std::wstring NormalizeScopeComparable(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && value.back() == L'\\') {
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
    const std::wstring prefix = NormalizeScopeComparable(scope.normalized_prefix);
    if (!prefix.empty() && prefix.back() == L'\\') {
        return normalized.rfind(prefix, 0) == 0;
    }
    return normalized == prefix ||
           (normalized.size() > prefix.size() &&
            normalized.rfind(prefix, 0) == 0 &&
            normalized[prefix.size()] == L'\\');
}

}  // namespace

std::unique_ptr<SearchIndex> CreateSearchIndex(SearchMode mode) {
    if (mode == SearchMode::Ngram) {
        return std::make_unique<NgramSearchIndex>();
    }
    return std::make_unique<FilterIndex>();
}

bool FilterIndex::LoadOrBuild(const MftSnapshot& snapshot,
                              const AutoFileSearchConfig& config,
                              WalPosition wal_pos,
                              std::wstring* error) {
    (void)error;
    return RebuildFromSnapshot(snapshot, config, wal_pos, error);
}

bool FilterIndex::RebuildFromSnapshot(const MftSnapshot& snapshot,
                                      const AutoFileSearchConfig& config,
                                      WalPosition wal_pos,
                                      std::wstring* error) {
    (void)error;
    config_ = config;
    wal_position_ = wal_pos;

    BuildFromSnapshot(snapshot);
    ready_ = true;
    return true;
}

void FilterIndex::Stop(const std::vector<IndexChange>& changes) {
    ApplyPending(changes);
    Clear();
}

void FilterIndex::Clear() {
    queued_changes_.clear();
    volume_roots_.clear();
    usn_journal_id_ = 0;
    next_usn_ = 0;
    mutation_version_ = 0;
    dictionary_ = FilterDictionary{};
    dirs_.clear();
    frn_to_file_index_.clear();
    files_.clear();
    ready_ = false;
}

void FilterIndex::BuildFromSnapshot(const MftSnapshot& snapshot) {
    Clear();
    for (const MftSnapshotVolume& volume : snapshot.volumes) {
        volume_roots_[static_cast<wchar_t>(std::towupper(volume.drive))] = volume.volume_root;
    }
    mutation_version_ = snapshot.mutation_version;

    std::vector<std::string_view> names;
    names.reserve(snapshot.entries.size());
    for (const MftEntryRecord& entry : snapshot.entries) {
        names.push_back(snapshot.Name(entry));
    }
    if (!config_.use_128_bit_filters &&
        config_.filter64_strategy == Filter64Strategy::DynamicUnicodeHash) {
        dictionary_ = BuildFilterDictionaryFromNames(names);
    }

    dirs_.reserve(snapshot.entries.size());
    files_.reserve(snapshot.entries.size());
    frn_to_file_index_.reserve(snapshot.entries.size());
    for (const MftEntryRecord& entry : snapshot.entries) {
        const std::string name(snapshot.Name(entry));
        if (entry.is_directory) {
            dirs_[entry.frn] = FilterIndexDir{entry.parent_frn, name};
        }
        if (files_.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            break;
        }
        FilterIndexFile file;
        file.frn = entry.frn;
        file.parent_frn = entry.parent_frn;
        file.name = name;
        file.filter = config_.use_128_bit_filters
            ? MakeAsciiFilter128(name)
            : MakeFilter64(name, dictionary_, config_.filter64_strategy);
        frn_to_file_index_[file.frn] = static_cast<std::uint32_t>(files_.size());
        files_.push_back(std::move(file));
    }
}

void FilterIndex::ApplyPending(const std::vector<IndexChange>& changes) {
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
        ApplyPending(combined);
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

WalPosition FilterIndex::GetWalPosition() const { return wal_position_; }
void FilterIndex::SetWalPosition(WalPosition pos) { wal_position_ = pos; }

std::vector<SearchResult> FilterIndex::Search(std::wstring_view query,
                                              std::size_t max_results,
                                              SearchPhase phase,
                                              const SearchScope& scope) {
    std::vector<SearchResult> results;
    if (!ready_ || query.empty() || max_results == 0 || phase == SearchPhase::FuzzyOnly) {
        return {};
    }
    const std::string query_lower = WideToLowerUtf8(query);
    const NameFilter query_filter = QueryFilter(query_lower);
    results.reserve(max_results);
    for (std::size_t i = 0; i < files_.size() && results.size() < max_results; ++i) {
        const FilterIndexFile& file = files_[i];
        if (!ContainsFilter(file.filter, query_filter) ||
            file.name.find(query_lower) == std::string::npos) {
            continue;
        }
        SearchResult result;
        result.frn = file.frn;
        result.name = Utf8ToWide(file.name);
        result.path = BuildPathForFile(i);
        if (!PathMatchesScope(result.path, scope)) {
            continue;
        }
        results.push_back(std::move(result));
    }
    return results;
}

bool FilterIndex::Ready() const {
    return ready_;
}

std::size_t FilterIndex::FileCount() const {
    return ready_ ? files_.size() : 0;
}

std::size_t FilterIndex::DirectoryCount() const {
    return ready_ ? dirs_.size() : 0;
}

bool FilterIndex::SupportsFuzzy() const {
    return false;
}

void FilterIndex::UpsertEntry(const RawEntry& entry) {
    if (entry.frn == 0 || entry.name.empty()) {
        return;
    }
    const std::string lowered_name = WideToLowerUtf8(entry.name);
    if (entry.is_directory) {
        dirs_[entry.frn] = FilterIndexDir{entry.parent_frn, lowered_name};
    }

    FilterIndexFile file;
    file.frn = entry.frn;
    file.parent_frn = entry.parent_frn;
    file.name = lowered_name;
    file.filter = QueryFilter(lowered_name);
    const auto found = frn_to_file_index_.find(entry.frn);
    if (found != frn_to_file_index_.end()) {
        files_[found->value] = std::move(file);
    } else if (files_.size() <= (std::numeric_limits<std::uint32_t>::max)()) {
        frn_to_file_index_[file.frn] = static_cast<std::uint32_t>(files_.size());
        files_.push_back(std::move(file));
    }
    ++mutation_version_;
}

void FilterIndex::RemoveEntry(Frn frn, bool is_directory) {
    if (!is_directory) {
        const auto found = frn_to_file_index_.find(frn);
        if (found == frn_to_file_index_.end()) {
            return;
        }
        const std::size_t index = found->value;
        const std::size_t last = files_.size() - 1;
        if (index != last) {
            files_[index] = std::move(files_[last]);
            frn_to_file_index_[files_[index].frn] = static_cast<std::uint32_t>(index);
        }
        files_.pop_back();
        frn_to_file_index_.erase(found);
        ++mutation_version_;
        return;
    }
    // 目录: 先从 files_ 中移除该目录自身
    {
        const auto found = frn_to_file_index_.find(frn);
        if (found != frn_to_file_index_.end()) {
            const std::size_t index = found->value;
            const std::size_t last = files_.size() - 1;
            if (index != last) {
                files_[index] = std::move(files_[last]);
                frn_to_file_index_[files_[index].frn] = static_cast<std::uint32_t>(index);
            }
            files_.pop_back();
            frn_to_file_index_.erase(found);
        }
    }

    std::unordered_set<Frn> removed_dirs;
    removed_dirs.insert(frn);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& entry : dirs_) {
            if (removed_dirs.count(entry.key) == 0 &&
                removed_dirs.count(entry.value.parent_frn) != 0) {
                removed_dirs.insert(entry.key);
                changed = true;
            }
        }
    }
    for (Frn dir : removed_dirs) {
        dirs_.erase(dir);
    }
    for (std::size_t i = 0; i < files_.size();) {
        if (removed_dirs.count(files_[i].parent_frn) == 0 &&
            removed_dirs.count(files_[i].frn) == 0) {
            ++i;
            continue;
        }
        const Frn file_frn = files_[i].frn;
        const std::size_t last = files_.size() - 1;
        if (i != last) {
            files_[i] = std::move(files_[last]);
            frn_to_file_index_[files_[i].frn] = static_cast<std::uint32_t>(i);
        }
        files_.pop_back();
        frn_to_file_index_.erase(file_frn);
    }
    ++mutation_version_;
}

std::wstring FilterIndex::BuildPathForFile(std::size_t file_index) const {
    if (file_index >= files_.size()) {
        return {};
    }
    std::vector<std::string> parts;
    parts.push_back(files_[file_index].name);
    std::unordered_set<Frn> seen;
    const wchar_t drive = DecodeFrnDrive(files_[file_index].frn);
    Frn current = files_[file_index].parent_frn;
    while (current != kRootParentFrn && seen.insert(current).second) {
        const auto found = dirs_.find(current);
        if (found == dirs_.end()) {
            break;
        }
        if (!found->value.name.empty()) {
            parts.push_back(found->value.name);
        }
        current = found->value.parent_frn;
    }
    std::wstring path;
    const auto root_found = volume_roots_.find(drive);
    if (root_found != volume_roots_.end()) {
        path = root_found->second;
    }
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
            path.push_back(L'\\');
        }
        path.append(Utf8ToWide(*it));
    }
    return path;
}

NameFilter FilterIndex::QueryFilter(std::string_view lowered_query) const {
    return config_.use_128_bit_filters
        ? MakeAsciiFilter128(lowered_query)
        : MakeFilter64(lowered_query, dictionary_, config_.filter64_strategy);
}

}  // namespace fsengine

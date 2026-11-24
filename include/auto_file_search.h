#pragma once

#include "filter.h"
#include "notifier.h"
#include "mft_snapshot.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fsengine
{

    enum class SearchMode
    {
        Filter,
        Ngram
    };

    enum class SearchPhase
    {
        ExactThenFuzzy,
        ExactOnly,
        FuzzyOnly,
    };

    struct AutoFileSearchConfig
    {
        std::wstring cache_directory = L"cache";
        std::vector<wchar_t> drives;
        SearchMode search_mode = SearchMode::Ngram;
        Filter64Strategy filter64_strategy = Filter64Strategy::DynamicUnicodeHash;
        bool monitor_enabled = true;
        bool use_128_bit_filters = false;
        std::size_t deferred_filter_merge_threshold = 4096;
    };

    struct AutoFileSearchOptions
    {
        std::size_t max_results = 50;
        SearchMode mode = SearchMode::Ngram;
        SearchPhase phase = SearchPhase::ExactThenFuzzy;
        std::wstring scope_path;
    };

    struct AutoFileSearchDriveStatus
    {
        wchar_t drive = L'\0';
        bool ready = false;
        std::wstring error;
        std::size_t file_count = 0;
        std::size_t directory_count = 0;
    };

    struct AutoFileSearchStatus
    {
        bool ready = false;
        bool running = false;
        bool mft_running = false;
        bool search_running = false;
        bool mft_building = false;
        bool search_building = false;
        std::wstring mft_error;
        std::wstring search_error;
        std::size_t ready_drive_count = 0;
        std::size_t total_file_count = 0;
        std::size_t total_directory_count = 0;
        std::vector<AutoFileSearchDriveStatus> drives;
    };

    class FSENGINE_API AutoFileSearch
    {
    public:
        explicit AutoFileSearch(AutoFileSearchConfig config = {});
        ~AutoFileSearch();

        AutoFileSearch(const AutoFileSearch &) = delete;
        AutoFileSearch &operator=(const AutoFileSearch &) = delete;

        bool Start(std::wstring *error);
        bool Configure(AutoFileSearchConfig config, std::wstring *error = nullptr);
        bool ConfigureSearch(AutoFileSearchConfig config, std::wstring *error = nullptr);
        bool StartMft(std::wstring *error);
        bool StartSearch(std::wstring *error);
        bool StartMftAsync(std::wstring *error);
        bool StartSearchAsync(std::wstring *error);
        void StopSearch();
        void StopMft();
        void Stop();
        void ReleaseFacts();
        bool Running() const;

        AutoFileSearchStatus Status() const;
        std::vector<SearchResult> Search(std::wstring_view query, const AutoFileSearchOptions &options = {});
        // Additive opt-in entry point; Search and the options ABI stay unchanged.
        std::vector<SearchResult> SearchWithPhase(std::wstring_view query, const AutoFileSearchOptions &options);
        ChangePosition CurrentChangePosition() const;
        bool CaptureChangeSnapshot(MftSnapshot* snapshot, ChangePosition* position, std::wstring* error = nullptr);
        FileChangePullResult PullFileChanges(ChangePosition cursor,
                                             std::size_t max_events = 1024) const;
        ChangePosition WaitFileChanges(ChangePosition cursor,
                                       std::uint32_t timeout_ms = 30000) const;
        void RebuildNgramIndexesIfNeeded();

    private:
        struct Impl;
        std::vector<SearchResult> SearchImpl(std::wstring_view query, const AutoFileSearchOptions& options, bool explicit_phase);
        bool StartMftImpl(AutoFileSearchConfig config, std::wstring *error);
        bool StartSearchImpl(AutoFileSearchConfig config, std::wstring *error);
        void SearchChangeLoop();
        void JoinMftWorker();
        void JoinSearchWorker();
        std::unique_ptr<Impl> impl_;
    };

} // namespace fsengine

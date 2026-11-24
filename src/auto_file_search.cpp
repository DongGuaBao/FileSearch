#include "auto_file_search.h"

#include "file_id_codec.h"
#include "filter.h"
#include "logger.h"
#include "mft_service.h"
#include "pinyin.h"
#include "search_index.h"
#include "utf8.h"
#include "win_file_id.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "Psapi.lib")

namespace fsengine
{

    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::uint64_t ProcessPrivateMb()
        {
            PROCESS_MEMORY_COUNTERS_EX counters = {};
            counters.cb = sizeof(counters);
            if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                      sizeof(counters)))
            {
                return 0;
            }
            return static_cast<std::uint64_t>(counters.PrivateUsage / (1024 * 1024));
        }

        void LogStartup(const char *message)
        {
            std::ostringstream out;
            out << "[startup] " << message
                << " private_mb=" << ProcessPrivateMb();
            LogLine(out.str());
        }

        void LogStartupDone(const char *message, Clock::time_point start)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            std::ostringstream out;
            out << "[startup] " << message
                << " elapsed_ms=" << ms
                << " private_mb=" << ProcessPrivateMb();
            LogLine(out.str());
        }

        class ScopedThreadBackgroundMode
        {
        public:
            ScopedThreadBackgroundMode()
            {
                HANDLE thread = GetCurrentThread();
                if (SetThreadPriority(thread, THREAD_MODE_BACKGROUND_BEGIN))
                {
                    background_mode_ = true;
                    return;
                }
                previous_priority_ = GetThreadPriority(thread);
                if (previous_priority_ != THREAD_PRIORITY_ERROR_RETURN &&
                    SetThreadPriority(thread, THREAD_PRIORITY_LOWEST))
                {
                    priority_changed_ = true;
                }
            }

            ~ScopedThreadBackgroundMode()
            {
                HANDLE thread = GetCurrentThread();
                if (background_mode_)
                {
                    SetThreadPriority(thread, THREAD_MODE_BACKGROUND_END);
                }
                else if (priority_changed_)
                {
                    SetThreadPriority(thread, previous_priority_);
                }
            }

            ScopedThreadBackgroundMode(const ScopedThreadBackgroundMode &) = delete;
            ScopedThreadBackgroundMode &operator=(const ScopedThreadBackgroundMode &) = delete;

        private:
            bool background_mode_ = false;
            bool priority_changed_ = false;
            int previous_priority_ = THREAD_PRIORITY_NORMAL;
        };

        std::vector<wchar_t> EnumerateIndexDrives()
        {
            std::vector<wchar_t> drives;
            const DWORD mask = GetLogicalDrives();
            for (wchar_t drive = L'A'; drive <= L'Z'; ++drive)
            {
                if ((mask & (1u << (drive - L'A'))) == 0)
                {
                    continue;
                }
                const wchar_t root[] = {drive, L':', L'\\', L'\0'};
                const UINT type = GetDriveTypeW(root);
                if (type == DRIVE_FIXED)
                {
                    drives.push_back(drive);
                }
            }
            return drives;
        }

        std::vector<wchar_t> NormalizeDrives(std::vector<wchar_t> drives)
        {
            for (wchar_t &drive : drives)
            {
                drive = static_cast<wchar_t>(std::towupper(drive));
            }
            std::sort(drives.begin(), drives.end());
            drives.erase(std::unique(drives.begin(), drives.end()), drives.end());
            return drives;
        }

        std::wstring WidenSystemError(const char *message)
        {
            std::wstring text;
            if (message == nullptr)
            {
                return text;
            }
            while (*message != '\0')
            {
                text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*message)));
                ++message;
            }
            return text;
        }

        std::wstring GlobalCachePath(const std::wstring &cache_directory, const wchar_t *file_name)
        {
            std::filesystem::path path = cache_directory.empty() ? std::filesystem::path(L"cache") : std::filesystem::path(cache_directory);
            path /= file_name;
            return path.wstring();
        }

        struct DriveCounts
        {
            bool ready = false;
            std::size_t files = 0;
            std::size_t directories = 0;
        };

        std::unordered_map<wchar_t, DriveCounts> CountSnapshotEntries(const MftSnapshot &snapshot)
        {
            std::unordered_map<wchar_t, DriveCounts> counts;
            for (const MftSnapshotVolume &volume : snapshot.volumes)
            {
                counts[static_cast<wchar_t>(std::towupper(volume.drive))].ready = true;
            }
            for (const MftEntryRecord &entry : snapshot.entries)
            {
                const wchar_t drive = DecodeFrnDrive(entry.frn);
                if (drive == L'\0')
                {
                    continue;
                }
                DriveCounts &item = counts[drive];
                if (entry.is_directory)
                {
                    ++item.directories;
                }
                else
                {
                    ++item.files;
                }
            }
            return counts;
        }

        std::string LowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        std::string CandidateTextForWalName(std::wstring_view name)
        {
            const std::wstring wide_name(name);
            std::string text = WideToLowerUtf8(wide_name);
            text.push_back('\n');
            text.append(LowerAscii(ToPinyinFull(wide_name)));
            text.push_back('\n');
            text.append(LowerAscii(ToPinyinInitials(wide_name)));
            return text;
        }

        std::wstring PathFromEncodedFrn(Frn encoded_frn, std::unordered_map<wchar_t, UniqueHandle> *handles)
        {
            encoded_frn = StripSearchFrnFlags(encoded_frn);
            const wchar_t drive = DecodeFrnDrive(encoded_frn);
            if (drive == L'\0')
            {
                return {};
            }
            auto found = handles->find(drive);
            if (found == handles->end() || !found->second.Valid())
            {
                UniqueHandle handle = OpenVolumeByRoot(std::wstring(1, drive) + L":");
                if (!handle.Valid())
                {
                    return {};
                }
                found = handles->insert_or_assign(drive, std::move(handle)).first;
            }
            return PathFromFileId(found->second.Get(), DecodeRawFrn(encoded_frn));
        }

        std::wstring NormalizeScopePath(std::wstring path)
        {
            if (path.empty())
            {
                return {};
            }
            std::replace(path.begin(), path.end(), L'/', L'\\');
            DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
            if (needed != 0)
            {
                std::wstring full(needed, L'\0');
                DWORD written = GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
                if (written != 0 && written < needed)
                {
                    full.resize(written);
                    path = std::move(full);
                }
            }
            while (path.size() > 3 && path.back() == L'\\')
            {
                path.pop_back();
            }
            std::transform(path.begin(), path.end(), path.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            return path;
        }

        bool PathMatchesScope(std::wstring_view path, const SearchScope &scope)
        {
            if (!scope.enabled)
            {
                return true;
            }
            const std::wstring normalized = NormalizeScopePath(std::wstring(path));
            if (!scope.normalized_prefix.empty() && scope.normalized_prefix.back() == L'\\')
            {
                return normalized.rfind(scope.normalized_prefix, 0) == 0;
            }
            return normalized == scope.normalized_prefix ||
                   (normalized.size() > scope.normalized_prefix.size() &&
                    normalized.rfind(scope.normalized_prefix, 0) == 0 &&
                    normalized[scope.normalized_prefix.size()] == L'\\');
        }

        SearchScope ResolveSearchScope(const std::wstring &scope_path)
        {
            SearchScope scope;
            if (scope_path.empty())
            {
                return scope;
            }
            const std::wstring normalized = NormalizeScopePath(scope_path);
            scope.enabled = true;
            scope.normalized_prefix = normalized;
            if (normalized.size() < 2 || normalized[1] != L':')
            {
                return scope;
            }
            Frn raw_frn = 0;
            if (!FileIdFromPath(normalized, &raw_frn))
            {
                return scope;
            }
            const wchar_t drive = static_cast<wchar_t>(std::towupper(normalized[0]));
            Frn encoded_frn = 0;
            if (!EncodeFrn(drive, raw_frn, &encoded_frn))
            {
                return scope;
            }
            scope.drive = drive;
            scope.root_frn = encoded_frn;
            return scope;
        }

        void AppendWalSearchResults(std::wstring_view query,
                                    std::size_t max_results,
                                    const std::vector<IndexChange> &changes,
                                    const SearchScope &scope,
                                    std::vector<SearchResult> *results,
                                    bool fuzzy = false)
        {
            if (results->size() >= max_results || changes.empty())
            {
                return;
            }
            const std::string query_lower = WideToLowerUtf8(query);
            if (query_lower.empty())
            {
                return;
            }
            const int max_distance = query_lower.size() <= 5 ? 1 : 2;
            auto matches = [&](const std::string& text) {
                return fuzzy ? FuzzyDistance(text, query_lower, max_distance) <= max_distance
                             : text.find(query_lower) != std::string::npos;
            };

            std::unordered_map<Frn, RawEntry> latest;
            latest.reserve(changes.size());
            for (const IndexChange &change : changes)
            {
                const Frn frn = StripSearchFrnFlags(change.entry.frn);
                if (frn == 0)
                {
                    continue;
                }
                if (change.remove)
                {
                    latest.erase(frn);
                }
                else
                {
                    latest[frn] = change.entry;
                }
            }

            std::unordered_set<Frn> seen;
            seen.reserve(results->size() + latest.size());
            for (const SearchResult &result : *results)
            {
                seen.insert(StripSearchFrnFlags(result.frn));
            }

            std::unordered_map<wchar_t, UniqueHandle> volume_handles;
            for (const auto &item : latest)
            {
                if (results->size() >= max_results)
                {
                    break;
                }
                const RawEntry &entry = item.second;
                const Frn frn = StripSearchFrnFlags(entry.frn);
                if (entry.is_directory || entry.name.empty() || !seen.insert(frn).second)
                {
                    continue;
                }
                const std::string wal_text = CandidateTextForWalName(entry.name);
                if (!matches(wal_text))
                {
                    continue;
                }
                const std::wstring path = PathFromEncodedFrn(entry.frn, &volume_handles);
                if (path.empty())
                {
                    continue;
                }
                if (!PathMatchesScope(path, scope))
                {
                    continue;
                }
                const std::wstring_view current_name = FileNamePart(path);
                if (current_name.empty())
                {
                    continue;
                }
                const std::string current_text = CandidateTextForWalName(current_name);
                if (!matches(current_text))
                {
                    continue;
                }
                SearchResult result;
                result.frn = frn;
                result.path = path;
                result.name.assign(current_name.data(), current_name.size());
                results->push_back(std::move(result));
            }
        }

    } // namespace

    struct AutoFileSearch::Impl
    {
        struct DriveContext
        {
            wchar_t drive = L'\0';
            bool mft_ready = false;
            bool search_ready = false;
            std::size_t file_count = 0;
            std::size_t directory_count = 0;
            std::wstring error;
        };

        explicit Impl(AutoFileSearchConfig config_value) : config(std::move(config_value))
        {
        }

        AutoFileSearchConfig config;
        std::vector<DriveContext> contexts;
        std::shared_ptr<MftService> mft_service;
        std::shared_ptr<SearchIndex> search_index;
        mutable std::mutex mutex;
        std::condition_variable state_changed;
        std::shared_mutex search_mutex;
        std::uint64_t search_generation = 0;
        // Latest state of every entry changed since the current ngram base
        // index was built. The WAL is capacity-bounded and is discarded when it
        // wraps, so search visibility cannot depend on it alone.
        std::mutex pending_mutex;
        std::unordered_map<Frn, RawEntry> pending_entries;
        std::thread mft_worker;
        std::thread search_worker;
        bool mft_running = false;
        std::atomic<bool> search_running{false};
        std::atomic<bool> search_stop_requested{false};
        bool mft_building = false;
        bool search_building = false;
        std::wstring mft_error;
        std::wstring search_error;
    };

    AutoFileSearch::AutoFileSearch(AutoFileSearchConfig config) : impl_(std::make_unique<Impl>(std::move(config)))
    {
    }

    AutoFileSearch::~AutoFileSearch()
    {
        Stop();
    }

    void AutoFileSearch::ReleaseFacts()
    {
        if (impl_->mft_service != nullptr)
        {
            impl_->mft_service->ReleaseFactsIfIdle();
        }
    }

    bool AutoFileSearch::Start(std::wstring *error)
    {
        const auto start = Clock::now();
        LogStartup("Start begin");
        const bool ok = StartMft(error) && StartSearch(error);
        LogStartupDone(ok ? "Start done" : "Start failed", start);
        return ok;
    }

    bool AutoFileSearch::Configure(AutoFileSearchConfig config, std::wstring *error)
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->mft_running || impl_->search_running.load() || impl_->mft_building || impl_->search_building)
        {
            if (error != nullptr)
            {
                *error = L"Cannot configure while MFT or search is running.";
            }
            return false;
        }
        impl_->config = std::move(config);
        return true;
    }

    bool AutoFileSearch::ConfigureSearch(AutoFileSearchConfig config, std::wstring *error)
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->search_running.load() || impl_->search_building)
        {
            if (error != nullptr)
            {
                *error = L"Cannot configure search while search is running.";
            }
            return false;
        }
        impl_->config.search_mode = config.search_mode;
        impl_->config.filter64_strategy = config.filter64_strategy;
        impl_->config.use_128_bit_filters = config.use_128_bit_filters;
        impl_->config.deferred_filter_merge_threshold = config.deferred_filter_merge_threshold;
        return true;
    }

    bool AutoFileSearch::StartMft(std::wstring *error)
    {
        const auto total_start = Clock::now();
        LogStartup("StartMft begin");
        JoinMftWorker();
        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->mft_running)
            {
                return true;
            }
            if (impl_->mft_building)
            {
                if (error != nullptr)
                {
                    *error = L"MFT is already building.";
                }
                return false;
            }
            impl_->mft_building = true;
            impl_->mft_error.clear();
            config = impl_->config;
        }

        std::wstring start_error;
        const auto impl_start = Clock::now();
        const bool ok = StartMftImpl(std::move(config), &start_error);
        LogStartupDone(ok ? "StartMftImpl done" : "StartMftImpl failed", impl_start);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->mft_building = false;
            impl_->mft_error = ok ? std::wstring{} : start_error;
        }
        impl_->state_changed.notify_all();
        if (!ok && error != nullptr)
        {
            *error = start_error;
        }
        LogStartupDone(ok ? "StartMft done" : "StartMft failed", total_start);
        return ok;
    }

    bool AutoFileSearch::StartMftAsync(std::wstring *error)
    {
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->mft_running || impl_->mft_building)
            {
                return true;
            }
        }
        JoinMftWorker();

        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->mft_running || impl_->mft_building)
            {
                return true;
            }
            impl_->mft_building = true;
            impl_->mft_error.clear();
            config = impl_->config;
        }

        try
        {
            impl_->mft_worker = std::thread(
                [this, config = std::move(config)]() mutable
                {
                    ScopedThreadBackgroundMode background_mode;
                    std::wstring start_error;
                    bool ok = false;
                    try
                    {
                        ok = StartMftImpl(std::move(config), &start_error);
                    }
                    catch (const std::exception &ex)
                    {
                        start_error = L"MFT worker exception: " + WidenSystemError(ex.what());
                        LogLine("[mft] Worker exception: " + std::string(ex.what()));
                    }
                    catch (...)
                    {
                        start_error = L"MFT worker unknown exception.";
                        LogLine("[mft] Worker unknown exception");
                    }
                    std::lock_guard lock(impl_->mutex);
                    impl_->mft_building = false;
                    impl_->mft_error = ok ? std::wstring{} : start_error;
                    impl_->state_changed.notify_all();
                });
        }
        catch (const std::system_error &ex)
        {
            std::lock_guard lock(impl_->mutex);
            impl_->mft_building = false;
            impl_->mft_error = WidenSystemError(ex.what());
            impl_->state_changed.notify_all();
            if (error != nullptr)
            {
                *error = impl_->mft_error;
            }
            return false;
        }
        return true;
    }

    bool AutoFileSearch::StartMftImpl(AutoFileSearchConfig config, std::wstring *error)
    {
        const auto total_start = Clock::now();
        std::vector<wchar_t> drives = config.drives.empty() ? EnumerateIndexDrives() : config.drives;
        drives = NormalizeDrives(std::move(drives));
        if (drives.empty())
        {
            if (error != nullptr)
            {
                *error = L"No indexable drives found.";
            }
            return false;
        }

        auto contexts = std::vector<Impl::DriveContext>{};
        contexts.reserve(drives.size());
        for (wchar_t drive : drives)
        {
            Impl::DriveContext context;
            context.drive = drive;
            contexts.push_back(std::move(context));
        }

        auto mft_service = std::make_unique<MftService>();
        MftServiceConfig mft_config;
        mft_config.drives = drives;
        mft_config.snapshot_path = GlobalCachePath(config.cache_directory, L"global.fsemft");
        mft_config.wal_path = GlobalCachePath(config.cache_directory, L"global.fsewal");

        std::wstring start_error;
        LogStartup("MFT LoadCache begin");
        const auto load_start = Clock::now();
        const bool mft_cache_loaded = mft_service->LoadCache(std::move(mft_config), &start_error);
        LogStartupDone(mft_cache_loaded ? "MFT LoadCache hit" : "MFT LoadCache miss", load_start);

        bool mft_cache_synced = false;
        if (mft_cache_loaded)
        {
            LogStartup("MFT SyncCacheToLatest begin");
            const auto sync_start = Clock::now();
            mft_cache_synced = mft_service->SyncCacheToLatest(&start_error);
            LogStartupDone(mft_cache_synced ? "MFT SyncCacheToLatest done" : "MFT SyncCacheToLatest failed", sync_start);
        }
        if (!mft_cache_synced)
        {
            std::wstring rebuild_error;
            LogStartup("MFT RebuildCache begin");
            const auto rebuild_start = Clock::now();
            if (!mft_service->RebuildCache(&rebuild_error))
            {
                LogStartupDone("MFT RebuildCache failed", rebuild_start);
                if (error != nullptr)
                {
                    *error = rebuild_error.empty() ? start_error : rebuild_error;
                }
                return false;
            }
            LogStartupDone("MFT RebuildCache done", rebuild_start);
        }
        if (config.monitor_enabled)
        {
            std::wstring monitor_error;
            const auto monitor_start = Clock::now();
            if (!mft_service->StartMonitor(&monitor_error))
            {
                LogStartupDone("MFT StartMonitor failed", monitor_start);
                if (error != nullptr)
                {
                    *error = L"MFT monitor start failed: " + monitor_error;
                }
                mft_service->Stop();
                return false;
            }
            LogStartupDone("MFT StartMonitor done", monitor_start);
        }

        LogStartup("MFT Snapshot for counts begin");
        const auto snapshot_start = Clock::now();
        const MftSnapshot snapshot = mft_service->Snapshot();
        LogStartupDone("MFT Snapshot for counts done", snapshot_start);
        const auto counts = CountSnapshotEntries(snapshot);
        std::size_t ready_count = 0;
        for (auto &context : contexts)
        {
            const auto found = counts.find(context.drive);
            if (found == counts.end() || !found->second.ready)
            {
                context.error = L"MFT cache is not ready.";
                continue;
            }
            context.mft_ready = true;
            context.file_count = found->second.files;
            context.directory_count = found->second.directories;
            ++ready_count;
        }

        if (ready_count == 0)
        {
            if (error != nullptr)
            {
                *error = L"No drive MFT cache is ready.";
            }
            mft_service->Stop();
            return false;
        }

        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->mft_service != nullptr)
            {
                impl_->mft_service->Stop();
            }
            impl_->contexts = std::move(contexts);
            impl_->mft_service = std::move(mft_service);
            impl_->mft_running = true;
        }
        LogStartupDone("StartMftImpl total done", total_start);
        return true;
    }

    bool AutoFileSearch::StartSearch(std::wstring *error)
    {
        const auto total_start = Clock::now();
        LogStartup("StartSearch begin");
        JoinSearchWorker();
        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->search_running.load())
            {
                return true;
            }
            if (impl_->search_building)
            {
                if (error != nullptr)
                {
                    *error = L"Search index is already building.";
                }
                return false;
            }
            impl_->search_building = true;
            impl_->search_stop_requested.store(false);
            impl_->search_error.clear();
            config = impl_->config;
        }

        std::wstring start_error;
        const auto impl_start = Clock::now();
        const bool ok = StartSearchImpl(std::move(config), &start_error);
        LogStartupDone(ok ? "StartSearchImpl done" : "StartSearchImpl failed", impl_start);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->search_building = false;
            impl_->search_error = ok ? std::wstring{} : start_error;
        }
        impl_->state_changed.notify_all();
        if (!ok && error != nullptr)
        {
            *error = start_error;
        }
        if (ok)
        {
            try
            {
                impl_->search_worker = std::thread([this]()
                {
                    try
                    {
                        SearchChangeLoop();
                    }
                    catch (const std::exception &ex)
                    {
                        LogLine("[search] Change worker exception: " + std::string(ex.what()));
                        std::lock_guard lock(impl_->mutex);
                        impl_->search_error = L"Search change worker exception: " + WidenSystemError(ex.what());
                        impl_->search_running.store(false);
                        impl_->state_changed.notify_all();
                    }
                    catch (...)
                    {
                        LogLine("[search] Change worker unknown exception");
                        std::lock_guard lock(impl_->mutex);
                        impl_->search_error = L"Search change worker unknown exception.";
                        impl_->search_running.store(false);
                        impl_->state_changed.notify_all();
                    }
                });
            }
            catch (const std::system_error &ex)
            {
                StopSearch();
                std::lock_guard lock(impl_->mutex);
                impl_->search_error = WidenSystemError(ex.what());
                if (error != nullptr)
                {
                    *error = impl_->search_error;
                }
                return false;
            }
        }
        LogStartupDone(ok ? "StartSearch done" : "StartSearch failed", total_start);
        return ok;
    }

    bool AutoFileSearch::StartSearchAsync(std::wstring *error)
    {
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->search_running.load() || impl_->search_building)
            {
                return true;
            }
        }
        JoinSearchWorker();

        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->search_running.load() || impl_->search_building)
            {
                return true;
            }
            impl_->search_building = true;
            impl_->search_stop_requested.store(false);
            impl_->search_error.clear();
            config = impl_->config;
        }

        try
        {
            impl_->search_worker = std::thread(
                [this, config = std::move(config)]() mutable
                {
                    std::wstring start_error;
                    bool ok = false;
                    try
                    {
                        {
                            ScopedThreadBackgroundMode background_mode;
                            ok = StartSearchImpl(std::move(config), &start_error);
                        }
                    }
                    catch (const std::exception &ex)
                    {
                        start_error = L"Search worker exception: " + WidenSystemError(ex.what());
                        LogLine("[search] Worker exception: " + std::string(ex.what()));
                    }
                    catch (...)
                    {
                        start_error = L"Search worker unknown exception.";
                        LogLine("[search] Worker unknown exception");
                    }
                    {
                        std::lock_guard lock(impl_->mutex);
                        impl_->search_building = false;
                        impl_->search_error = ok ? std::wstring{} : start_error;
                        impl_->state_changed.notify_all();
                    }
                    if (ok)
                    {
                        try
                        {
                            SearchChangeLoop();
                        }
                        catch (const std::exception &ex)
                        {
                            LogLine("[search] Change worker exception: " + std::string(ex.what()));
                            std::lock_guard lock(impl_->mutex);
                            impl_->search_error = L"Search change worker exception: " + WidenSystemError(ex.what());
                            impl_->search_running.store(false);
                            impl_->state_changed.notify_all();
                        }
                        catch (...)
                        {
                            LogLine("[search] Change worker unknown exception");
                            std::lock_guard lock(impl_->mutex);
                            impl_->search_error = L"Search change worker unknown exception.";
                            impl_->search_running.store(false);
                            impl_->state_changed.notify_all();
                        }
                    }
                });
        }
        catch (const std::system_error &ex)
        {
            std::lock_guard lock(impl_->mutex);
            impl_->search_building = false;
            impl_->search_error = WidenSystemError(ex.what());
            impl_->state_changed.notify_all();
            if (error != nullptr)
            {
                *error = impl_->search_error;
            }
            return false;
        }
        return true;
    }

    void AutoFileSearch::SearchChangeLoop()
    {
        MftService* mft = nullptr;
        std::shared_ptr<SearchIndex> search_index;
        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            mft = impl_->mft_service.get();
            search_index = impl_->search_index;
            config = impl_->config;
        }
        if (mft == nullptr || search_index == nullptr)
        {
            return;
        }

        WalPosition last_pos = search_index->GetWalPosition();

        while (impl_->search_running.load() && !impl_->search_stop_requested.load())
        {
            mft->WaitWalNotification(500);
            bool wal_expired = false;
            std::vector<IndexChange> changes = mft->PullSince(last_pos, &wal_expired);
            if (wal_expired)
            {
                ScopedThreadBackgroundMode background_mode;
                WalPosition wal_pos;
                const MftSnapshot snapshot = mft->Snapshot(&wal_pos);
                auto rebuilt_index = CreateSearchIndex(config.search_mode);
                std::wstring rebuild_error;
                if (rebuilt_index->RebuildFromSnapshot(snapshot, config, wal_pos, &rebuild_error))
                {
                    auto counts = CountSnapshotEntries(snapshot);
                    bool should_swap = false;
                    {
                        std::lock_guard state_lock(impl_->mutex);
                        should_swap = impl_->search_running.load() &&
                                      !impl_->search_stop_requested.load() &&
                                      impl_->mft_service.get() == mft;
                        if (should_swap)
                        {
                            for (auto &context : impl_->contexts)
                            {
                                const auto found = counts.find(context.drive);
                                if (found == counts.end() || !found->second.ready)
                                {
                                    context.search_ready = false;
                                    continue;
                                }
                                context.file_count = found->second.files;
                                context.directory_count = found->second.directories;
                                context.search_ready = context.mft_ready;
                            }
                            impl_->search_error.clear();
                        }
                    }
                    if (should_swap)
                    {
                        std::unique_lock search_lock(impl_->search_mutex);
                        if (impl_->search_index != nullptr)
                        {
                            impl_->search_index = std::shared_ptr<SearchIndex>(std::move(rebuilt_index));
                            ++impl_->search_generation;
                            // The new base includes every change captured above.
                            std::lock_guard pending_lock(impl_->pending_mutex);
                            impl_->pending_entries.clear();
                        }
                    }
                }
                else
                {
                    LogLine("[search] WAL-expiry index rebuild failed: " + WideToUtf8(rebuild_error));
                    std::lock_guard state_lock(impl_->mutex);
                    impl_->search_error = rebuild_error.empty() ? L"Search index rebuild failed after WAL position expired." : rebuild_error;
                }
                last_pos = wal_pos;
                continue;
            }
            if (!changes.empty())
            {
                std::unique_lock search_lock(impl_->search_mutex);
                if (impl_->search_index != nullptr)
                {
                    impl_->search_index->ApplyPending(changes);
                    // Advance even in ngram mode: pending_entries retains the
                    // change set, so the (bounded) WAL no longer has to be
                    // replayed from the base build on every search.
                    impl_->search_index->SetWalPosition(last_pos);
                }
                std::lock_guard pending_lock(impl_->pending_mutex);
                for (const IndexChange &change : changes)
                {
                    const Frn frn = StripSearchFrnFlags(change.entry.frn);
                    if (frn == 0)
                    {
                        continue;
                    }
                    if (change.remove)
                    {
                        impl_->pending_entries.erase(frn);
                    }
                    else
                    {
                        impl_->pending_entries[frn] = change.entry;
                    }
                }
            }
        }
    }

    bool AutoFileSearch::StartSearchImpl(AutoFileSearchConfig config, std::wstring *error)
    {
        const auto total_start = Clock::now();
        MftService *mft_service = nullptr;
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->mft_running || impl_->mft_service == nullptr)
            {
                if (error != nullptr)
                {
                    *error = L"MFT is not running.";
                }
                return false;
            }
            mft_service = impl_->mft_service.get();
        }

        LogStartup("Search Snapshot materialize begin");
        const auto snapshot_start = Clock::now();
        WalPosition wal_pos;
        const MftSnapshot snapshot = mft_service->Snapshot(&wal_pos);
        {
            std::ostringstream out;
            out << "[startup] Search Snapshot materialize done"
                << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - snapshot_start).count()
                << " entries=" << snapshot.entries.size()
                << " private_mb=" << ProcessPrivateMb();
            LogLine(out.str());
        }
        auto counts = CountSnapshotEntries(snapshot);

        auto index = CreateSearchIndex(config.search_mode);
        std::wstring start_error;
        LogStartup("Search LoadOrBuild begin");
        const auto index_start = Clock::now();
        if (!index->LoadOrBuild(snapshot, config, wal_pos, &start_error))
        {
            LogStartupDone("Search LoadOrBuild failed", index_start);
            if (error != nullptr)
            {
                *error = start_error.empty() ? L"No search index is ready." : start_error;
            }
            return false;
        }
        LogStartupDone("Search LoadOrBuild done", index_start);

        // A WAL generation change alone does not mean that the cached index is
        // stale: one generation of records can still be present in the ring.
        // Let PullSince decide whether the saved position is actually expired.
        WalPosition catchup_pos = index->GetWalPosition();
        bool wal_expired = false;
        std::vector<IndexChange> catchup_changes = mft_service->PullSince(catchup_pos, &wal_expired);
        if (wal_expired)
        {
            LogStartup("Search cached WAL position expired; rebuild begin");
            const auto rebuild_start = Clock::now();
            WalPosition rebuild_pos;
            const MftSnapshot rebuild_snapshot = mft_service->Snapshot(&rebuild_pos);
            counts = CountSnapshotEntries(rebuild_snapshot);
            if (!index->RebuildFromSnapshot(rebuild_snapshot, config, rebuild_pos, &start_error))
            {
                LogStartupDone("Search expired WAL rebuild failed", rebuild_start);
                if (error != nullptr)
                {
                    *error = start_error.empty() ? L"Search index rebuild failed after WAL position expired." : start_error;
                }
                return false;
            }
            LogStartupDone("Search expired WAL rebuild done", rebuild_start);
        }
        else
        {
            if (!catchup_changes.empty())
            {
                index->ApplyPending(catchup_changes);
            }
            if (config.search_mode != SearchMode::Ngram)
            {
                index->SetWalPosition(catchup_pos);
            }
            std::ostringstream out;
            out << "[startup] Search WAL catch-up done"
                << " changes=" << catchup_changes.size()
                << " generation=" << catchup_pos.generation
                << " offset=" << catchup_pos.offset;
            LogLine(out.str());
        }

        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->mft_running || impl_->mft_service == nullptr)
            {
                if (error != nullptr)
                {
                    *error = L"MFT stopped while search index was building.";
                }
                return false;
            }
            if (impl_->search_stop_requested.load())
            {
                if (error != nullptr)
                {
                    *error = L"Search stopped while search index was building.";
                }
                return false;
            }
            for (auto &context : impl_->contexts)
            {
                const auto found = counts.find(context.drive);
                context.search_ready = false;
                if (found == counts.end() || !found->second.ready)
                {
                    continue;
                }
                context.file_count = found->second.files;
                context.directory_count = found->second.directories;
            }

            {
                std::unique_lock search_lock(impl_->search_mutex);
                impl_->search_index = std::shared_ptr<SearchIndex>(std::move(index));
                ++impl_->search_generation;
            }

            for (auto &context : impl_->contexts)
            {
                context.search_ready = context.mft_ready;
            }
            impl_->search_running.store(true);
        }
        LogStartupDone("StartSearchImpl total done", total_start);
        return true;
    }

    void AutoFileSearch::JoinMftWorker()
    {
        if (impl_->mft_worker.joinable() && impl_->mft_worker.get_id() != std::this_thread::get_id())
        {
            impl_->mft_worker.join();
        }
    }

    void AutoFileSearch::JoinSearchWorker()
    {
        if (impl_->search_worker.joinable() && impl_->search_worker.get_id() != std::this_thread::get_id())
        {
            impl_->search_worker.join();
        }
    }

    void AutoFileSearch::StopSearch()
    {
        impl_->search_stop_requested.store(true);
        impl_->search_running.store(false);
        JoinSearchWorker();
        std::unique_lock lock(impl_->mutex);
        impl_->state_changed.wait(lock, [this]() { return !impl_->search_building; });
        if (impl_->mft_service == nullptr)
        {
            return;
        }
        for (auto &context : impl_->contexts)
        {
            context.search_ready = false;
        }
        {
            std::unique_lock search_lock(impl_->search_mutex);
            ++impl_->search_generation;
            if (impl_->search_index != nullptr)
            {
                impl_->search_index->Stop({});
                impl_->search_index.reset();
            }
            std::lock_guard pending_lock(impl_->pending_mutex);
            impl_->pending_entries.clear();
        }
    }

    void AutoFileSearch::StopMft()
    {
        StopSearch();
        JoinMftWorker();
        std::unique_lock lock(impl_->mutex);
        impl_->state_changed.wait(lock, [this]() { return !impl_->mft_building; });
        if (impl_->mft_service != nullptr)
        {
            impl_->mft_service->Stop();
            impl_->mft_service.reset();
        }
        for (auto &context : impl_->contexts)
        {
            context.mft_ready = false;
        }
        impl_->contexts.clear();
        impl_->mft_running = false;
    }

    void AutoFileSearch::Stop()
    {
        StopMft();
    }

    bool AutoFileSearch::Running() const
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->search_running.load();
    }

    AutoFileSearchStatus AutoFileSearch::Status() const
    {
        std::lock_guard lock(impl_->mutex);
        AutoFileSearchStatus status;
        status.running = impl_->mft_running || impl_->search_running.load() || impl_->mft_building || impl_->search_building;
        status.mft_running = impl_->mft_running;
        status.search_running = impl_->search_running.load();
        status.mft_building = impl_->mft_building;
        status.search_building = impl_->search_building;
        status.mft_error = impl_->mft_error;
        status.search_error = impl_->search_error;
        status.drives.reserve(impl_->contexts.size());
        for (const auto &context : impl_->contexts)
        {
            AutoFileSearchDriveStatus drive_status;
            drive_status.drive = context.drive;
            drive_status.ready = context.search_ready;
            drive_status.error = context.error;
            drive_status.file_count = context.file_count;
            drive_status.directory_count = context.directory_count;
            if (drive_status.ready)
            {
                ++status.ready_drive_count;
                status.total_file_count += drive_status.file_count;
                status.total_directory_count += drive_status.directory_count;
            }
            status.drives.push_back(std::move(drive_status));
        }
        status.ready = impl_->search_running.load() && impl_->search_index != nullptr;
        return status;
    }

    std::vector<SearchResult> AutoFileSearch::Search(std::wstring_view query, const AutoFileSearchOptions &options)
    {
        return SearchImpl(query, options, false);
    }

    std::vector<SearchResult> AutoFileSearch::SearchWithPhase(std::wstring_view query, const AutoFileSearchOptions &options)
    {
        return SearchImpl(query, options, true);
    }

    std::vector<SearchResult> AutoFileSearch::SearchImpl(std::wstring_view query, const AutoFileSearchOptions &options, bool explicit_phase)
    {
        std::vector<SearchResult> results;
        if (!impl_->search_running.load() || query.empty() || options.max_results == 0)
        {
            return results;
        }

        MftService *mft_service = nullptr;
        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            mft_service = impl_->mft_service.get();
            config = impl_->config;
        }

        std::shared_lock search_lock(impl_->search_mutex);
        std::shared_ptr<SearchIndex> search_index = impl_->search_index;
        if (search_index == nullptr || !search_index->Ready())
        {
            return results;
        }
        const SearchScope scope = ResolveSearchScope(options.scope_path);

        const bool run_exact = options.phase != SearchPhase::FuzzyOnly;
        if (explicit_phase && options.phase != SearchPhase::ExactOnly && !search_index->SupportsFuzzy())
            throw std::runtime_error("Active Windows index does not support fuzzy phases.");
        const bool run_fuzzy = explicit_phase && options.phase != SearchPhase::ExactOnly && search_index->SupportsFuzzy();

        if (run_exact)
        {
            results = search_index->Search(query, options.max_results, SearchPhase::ExactOnly, scope);
        }
        auto append_pending_results = [&](bool fuzzy) {
            if (config.search_mode != SearchMode::Ngram || results.size() >= options.max_results)
            {
                return;
            }
            std::vector<IndexChange> pending;
            {
                std::lock_guard pending_lock(impl_->pending_mutex);
                pending.reserve(impl_->pending_entries.size());
                for (const auto &item : impl_->pending_entries)
                {
                    IndexChange change;
                    change.entry = item.second;
                    change.remove = false;
                    pending.push_back(std::move(change));
                }
            }
            AppendWalSearchResults(query, options.max_results, pending, scope, &results, fuzzy);
        };
        auto append_wal_results = [&](bool fuzzy) {
            if (config.search_mode != SearchMode::Ngram || results.size() >= options.max_results || mft_service == nullptr)
            {
                return;
            }
            WalPosition wal_pos = search_index->GetWalPosition();
            bool wal_expired = false;
            std::vector<IndexChange> changes = mft_service->PullSince(wal_pos, &wal_expired);
            if (!wal_expired)
            {
                AppendWalSearchResults(query, options.max_results, changes, scope, &results, fuzzy);
            }
        };
        auto append_wal_exact = [&]() {
            append_pending_results(false);
            append_wal_results(false);
        };
        if (explicit_phase && run_exact) append_wal_exact();
        if (run_fuzzy && results.size() < options.max_results)
        {
            // Fuzzy matching includes distance-zero exact matches. Request a
            // full result budget, then deduplicate, rather than underfilling
            // the page because exact duplicates consumed the remaining budget.
            auto fuzzy = search_index->Search(query, options.max_results, SearchPhase::FuzzyOnly, scope);
            results.reserve(results.size() + fuzzy.size());
            for (auto &item : fuzzy)
            {
                const auto duplicate = std::find_if(results.begin(), results.end(), [&item](const SearchResult &existing) { return existing.frn == item.frn; });
                if (duplicate == results.end())
                {
                    results.push_back(std::move(item));
                    if (results.size() == options.max_results) break;
                }
            }
        }
        if (!explicit_phase) append_wal_exact();
        if (run_fuzzy && results.size() < options.max_results)
        {
            bool wal_expired = false;
            auto wal_position = search_index->GetWalPosition();
            auto changes = mft_service != nullptr ? mft_service->PullSince(wal_position, &wal_expired) : std::vector<IndexChange>{};
            if (!wal_expired) AppendWalSearchResults(query, options.max_results, changes, scope, &results, true);
            append_pending_results(true);
        }
        return results;
    }

    ChangePosition AutoFileSearch::CurrentChangePosition() const
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->mft_service != nullptr
            ? impl_->mft_service->CurrentChangePosition()
            : ChangePosition{};
    }

    bool AutoFileSearch::CaptureChangeSnapshot(MftSnapshot* snapshot, ChangePosition* position, std::wstring* error)
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->mft_running || !impl_->mft_service) {
            if (error) *error = L"MFT service is not running.";
            return false;
        }
        return impl_->mft_service->CaptureChangeSnapshot(snapshot, position, error);
    }
    FileChangePullResult AutoFileSearch::PullFileChanges(ChangePosition cursor,
                                                         std::size_t max_events) const
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->mft_service == nullptr)
        {
            FileChangePullResult result;
            result.expired = true;
            return result;
        }
        return impl_->mft_service->PullFileChanges(cursor, max_events);
    }

    ChangePosition AutoFileSearch::WaitFileChanges(ChangePosition cursor,
                                                   std::uint32_t timeout_ms) const
    {
        std::shared_ptr<MftService> service;
        {
            std::lock_guard lock(impl_->mutex);
            service = impl_->mft_service;
        }
        return service != nullptr
            ? service->WaitFileChanges(cursor, timeout_ms)
            : ChangePosition{};
    }

    void AutoFileSearch::RebuildNgramIndexesIfNeeded()
    {
        MftService* mft = nullptr;
        std::shared_ptr<SearchIndex> search_index;
        AutoFileSearchConfig config;
        {
            std::lock_guard lock(impl_->mutex);
            mft = impl_->mft_service.get();
            search_index = impl_->search_index;
            config = impl_->config;
        }
        if (mft == nullptr || search_index == nullptr || !search_index->Ready() || config.search_mode != SearchMode::Ngram)
        {
            return;
        }

        ScopedThreadBackgroundMode background_mode;
        WalPosition wal_pos;
        const MftSnapshot snapshot = mft->Snapshot(&wal_pos);
        auto rebuilt_index = CreateSearchIndex(config.search_mode);
        std::wstring rebuild_error;
        if (!rebuilt_index->RebuildFromSnapshot(snapshot, config, wal_pos, &rebuild_error))
        {
            std::lock_guard lock(impl_->mutex);
            impl_->search_error = rebuild_error.empty() ? L"Ngram index rebuild failed." : rebuild_error;
            return;
        }

        auto counts = CountSnapshotEntries(snapshot);
        bool should_swap = false;
        {
            std::lock_guard lock(impl_->mutex);
            should_swap = impl_->mft_service.get() == mft &&
                          impl_->search_running.load() &&
                          !impl_->search_stop_requested.load();
            if (should_swap)
            {
                for (auto &context : impl_->contexts)
                {
                    const auto found = counts.find(context.drive);
                    if (found == counts.end() || !found->second.ready)
                    {
                        context.search_ready = false;
                        continue;
                    }
                    context.file_count = found->second.files;
                    context.directory_count = found->second.directories;
                    context.search_ready = context.mft_ready;
                }
                impl_->search_error.clear();
            }
        }
        if (!should_swap)
        {
            return;
        }

        std::unique_lock search_lock(impl_->search_mutex);
        if (impl_->search_index != nullptr)
        {
            impl_->search_index = std::shared_ptr<SearchIndex>(std::move(rebuilt_index));
            ++impl_->search_generation;
        }
    }

} // namespace fsengine

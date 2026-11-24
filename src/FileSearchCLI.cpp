#include "auto_file_search.h"

#include <chrono>
#include <iostream>
#include <locale>
#include <string>
#include <utility>

namespace {

const wchar_t* SearchModeName(fsengine::SearchMode mode) {
    switch (mode) {
    case fsengine::SearchMode::Ngram:
        return L"ngram";
    case fsengine::SearchMode::Filter:
    default:
        return L"filter";
    }
}

const wchar_t* Filter64StrategyName(fsengine::Filter64Strategy strategy) {
    switch (strategy) {
    case fsengine::Filter64Strategy::Utf8Base36:
        return L"utf8-base36";
    case fsengine::Filter64Strategy::DynamicUnicodeHash:
    default:
        return L"dynamic-unicode-hash";
    }
}

void PrintStatus(const fsengine::AutoFileSearchStatus& status) {
    if (status.mft_building || status.search_building) {
        std::wcout << L"Building:";
        if (status.mft_building) {
            std::wcout << L" MFT";
        }
        if (status.search_building) {
            std::wcout << L" search";
        }
        std::wcout << std::endl;
    }
    if (!status.mft_error.empty()) {
        std::wcout << L"MFT error: " << status.mft_error << std::endl;
    }
    if (!status.search_error.empty()) {
        std::wcout << L"Search error: " << status.search_error << std::endl;
    }
    std::wcout << L"Ready " << status.ready_drive_count << L" drive(s), "
               << status.total_file_count << L" files and "
               << status.total_directory_count << L" directories." << std::endl;
    for (const auto& drive : status.drives) {
        std::wcout << L"[" << drive.drive << L":] ";
        if (!drive.ready) {
            std::wcout << L"not ready";
            if (!drive.error.empty()) {
                std::wcout << L": " << drive.error;
            }
            std::wcout << std::endl;
            continue;
        }
        std::wcout << drive.file_count << L" files and "
                   << drive.directory_count << L" directories." << std::endl;
    }
}

}  // namespace

int wmain() {
    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    std::wcin.imbue(std::locale());

    fsengine::AutoFileSearchConfig config;
    config.search_mode = fsengine::SearchMode::Ngram;
    config.filter64_strategy = fsengine::Filter64Strategy::Utf8Base36;
    config.monitor_enabled = true;

    fsengine::AutoFileSearch search(std::move(config));

    const auto build_start = std::chrono::steady_clock::now();
    std::wstring start_error;
    if (!search.Start(&start_error)) {
        std::wcerr << L"FileSearch start failed: " << start_error << std::endl;
        return 1;
    }
    search.ReleaseFacts();
    const auto build_end = std::chrono::steady_clock::now();
    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(build_end - build_start).count();
    PrintStatus(search.Status());
    std::wcout << L"Search mode: " << SearchModeName(fsengine::SearchMode::Ngram) << std::endl;
    std::wcout << L"Filter64 strategy: " << Filter64StrategyName(fsengine::Filter64Strategy::Utf8Base36) << std::endl;
    std::wcout << L"Memory profile: lowmem" << std::endl;
    std::wcout << L"Initialized in " << build_ms << L" ms." << std::endl;

    std::wstring query;
    while (true) {
        std::wcout << L"\nquery> ";
        if (!std::getline(std::wcin, query) || query.empty() || query == L"exit") {
            break;
        }

        const auto search_start = std::chrono::steady_clock::now();
        fsengine::AutoFileSearchOptions options;
        options.max_results = 5;
        options.mode = fsengine::SearchMode::Ngram;
        options.phase = fsengine::SearchPhase::ExactThenFuzzy;
        const auto results = search.Search(query, options);
        const auto search_end = std::chrono::steady_clock::now();
        const auto search_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();

        std::wcout << L"Found " << results.size() << L" result(s) in "
                   << search_ms << L" ms." << std::endl;
        for (const auto& result : results) {
            std::wcout << result.path << std::endl;
        }
    }

    search.Stop();
    return 0;
}

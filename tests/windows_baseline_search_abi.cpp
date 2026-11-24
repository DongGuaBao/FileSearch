// Compile with Git-baseline headers/import library, run with the new DLL.
#include "auto_file_search.h"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    namespace fs = std::filesystem;
    using namespace fsengine;
    const fs::path root(argv[1]);
    std::ofstream report{fs::path(argv[2])};
    auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    try {
        require(root.is_absolute() && !fs::exists(root), "fixture must be fresh and absolute");
        fs::create_directories(root);
        AutoFileSearchConfig config;
        config.cache_directory = (root / "cache").wstring();
        config.drives = {root.wstring().front()};
        AutoFileSearch engine(config);
        std::wstring error;
        require(engine.StartMft(&error), "baseline StartMft failed");
        require(engine.StartSearch(&error), "baseline StartSearch failed");
        require(engine.Status().ready, "baseline status ABI failed");
        engine.ReleaseFacts();
        const auto original = root / "baseline_abi_7a825c_original.txt";
        const auto renamed = root / "baseline_abi_7a825c_renamed.txt";
        AutoFileSearchOptions options;
        options.scope_path = root.wstring();
        auto has = [&](const wchar_t* query, const fs::path& target) {
            for (const auto& item : engine.Search(query, options))
                if (_wcsicmp(item.path.c_str(), target.c_str()) == 0) return true;
            return false;
        };
        auto await = [&](auto predicate) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!predicate()) {
                require(std::chrono::steady_clock::now() < end, "baseline live search timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        { std::ofstream file(original); file << "ABI fixture"; }
        await([&] { return has(L"baseline_abi_7a825c_original", original); });
        fs::rename(original, renamed);
        await([&] { return has(L"baseline_abi_7a825c_renamed", renamed); });
        require(!has(L"baseline_abi_7a825c_original", original), "old name survived rename");
        fs::remove(renamed);
        await([&] { return !has(L"baseline_abi_7a825c_renamed", renamed); });
        engine.Stop();
        require(!engine.Running(), "baseline Stop failed");
        report << "ok: baseline headers/imports, new DLL, status, released facts, scoped search, create/rename/delete/stop\n";
        return 0;
    } catch (const std::exception& error) {
        report << "failed: " << error.what() << '\n';
        return 1;
    }
}

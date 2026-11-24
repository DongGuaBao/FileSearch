#include "linux_catalog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    if (geteuid() != 0) return 77;
    std::string pattern = "/home/donggua/filesearch-cached-start-XXXXXX";
    auto created = mkdtemp(pattern.data());
    Require(created, "fixture failed");
    const std::filesystem::path base(created), root = base / "files", state = base / "state";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{base};
    std::filesystem::create_directory(root); std::filesystem::create_directory(state);
    const auto db = (state / "catalog.db").string();
    std::string error;
    std::ofstream(root / "中文.txt") << "old";
    std::uint64_t previous_id = 0;
    {
        fsengine::LinuxCatalog first;
        Require(first.Start(root.string(), error, db, true), error.c_str());
        Require(!first.SearchReady() && !first.Ready(), "empty new store authorized cache");
        Require(first.Pump(error), error.c_str());
        previous_id = first.Search("zhongwen", "", 10).at(0).id;
    }
    std::filesystem::remove(root / "中文.txt");
    std::ofstream(root / "offline.txt") << "new";
    {
        fsengine::LinuxCatalog restart;
        Require(restart.Start(root.string(), error, db, true), error.c_str());
        Require(restart.SearchReady() && !restart.Ready(), "cache not searchable before scan");
        bool stale = false;
        const auto cached = restart.Search("zhongwen", "", 10, fsengine::LinuxSearchPhase::ExactOnly, &stale);
        Require(stale && cached.size() == 1 && cached[0].id == previous_id, "cache lost identity or pinyin search");
        Require(restart.Search("offline", "", 10).empty(), "cache unexpectedly already scanned");
        fsengine::ChangePosition position;
        bool refused = false;
        try { restart.Snapshot("", true, true, position); }
        catch (const fsengine::LinuxCatalogNotReady&) { refused = true; }
        Require(refused && !restart.CanResume(restart.Position()), "unvalidated snapshot/stream accepted");
        Require(restart.Pump(error), error.c_str());
        Require(restart.Ready() && restart.SearchReady(), "validation did not complete");
        Require(restart.Search("zhongwen", "", 10).empty(), "deleted cached entry survived validation");
        const auto current = restart.Search("offline", "", 10, fsengine::LinuxSearchPhase::ExactOnly, &stale);
        Require(!stale && current.size() == 1, "offline addition missing after validation");
        Require(restart.Snapshot("", true, true, position) != nullptr, "validated snapshot unavailable");
    }
    const auto other = base / "other";
    std::filesystem::create_directory(other);
    {
        fsengine::LinuxCatalog changed;
        Require(changed.Start(other.string(), error, db, true), error.c_str());
        Require(!changed.SearchReady(), "different root reused cached index");
        Require(changed.Pump(error) && changed.Ready(), "different root scan failed");
    }
    {
        fsengine::LinuxCatalog empty;
        Require(empty.Start(other.string(), error, db, true), error.c_str());
        Require(empty.SearchReady() && !empty.Ready(), "validated empty cache not reusable");
        Require(empty.Search("anything", "", 10).empty(), "empty cache returned entries");
    }
    std::cout << "linux_cached_start_test passed\n";
}

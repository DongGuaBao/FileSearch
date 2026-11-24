#include "linux_catalog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int main() {
    if (geteuid() != 0) return 77;
    std::string pattern = "/home/donggua/filesearch-catalog-XXXXXX";
    char* created = mkdtemp(pattern.data());
    Require(created != nullptr, "fixture failed");
    std::filesystem::path root(created);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    std::ofstream(root / "baseline.txt") << "base";
    fsengine::LinuxCatalog catalog;
    std::string error;
    Require(catalog.Start(root.string() + "/", error), error.c_str());
    Require(catalog.Search("baseline", "", 10).size() == 1, "baseline not found");
    std::ofstream(root / "中文文档.txt") << "pinyin";
    Require(catalog.Pump(error), error.c_str());
    Require(catalog.Search("zhongwen", "", 10).size() == 1, "incremental pinyin insert failed");
    std::filesystem::rename(root / "中文文档.txt", root / "renamed.txt");
    Require(catalog.Pump(error), error.c_str());
    Require(catalog.Search("zhongwen", "", 10).empty(), "rename retained old pinyin postings");
    Require(catalog.Search("renamed", "", 10).size() == 1, "rename missing new postings");
    auto position = catalog.Position();
    std::filesystem::create_directory(root / "rapid_old");
    std::ofstream(root / "rapid_old" / "rapid_child.txt") << "rapid";
    std::filesystem::rename(root / "rapid_old", root / "rapid_new");
    Require(catalog.Pump(error), error.c_str());
    auto rapid = catalog.Search("rapid_child", "", 10);
    Require(rapid.size() == 1 && rapid[0].path == (root / "rapid_new" / "rapid_child.txt").string(),
        "create then rename before pump lost subtree");
    std::filesystem::create_directory(root / "nested");
    std::ofstream(root / "nested" / "live.txt") << "live";
    Require(catalog.Pump(error), error.c_str());
    Require(catalog.Search("live", (root / "nested").string(), 10).size() == 1, "created file not found");
    Require(catalog.Search("live", (root / "nested").string() + "/", 10).size() == 1, "trailing scope separator lost matches");
    Require(catalog.Search("live", (root / "nested-sibling").string(), 10).empty(), "scope boundary wrong");
    auto before_move = catalog.Position();
    std::filesystem::rename(root / "nested", root / "moved");
    Require(catalog.Pump(error), error.c_str());
    auto moved = catalog.Search("live", "", 10);
    Require(moved.size() == 1 && moved[0].path == (root / "moved" / "live.txt").string(), "directory move wrong");
    auto move_events = catalog.Changes(before_move);
    Require(move_events.events.size() == 1, "directory move not a single event");
    Require(move_events.events[0].type == fsengine::FsEventType::DirRenamed &&
        move_events.events[0].old_path == (root / "nested").wstring() &&
        move_events.events[0].path == (root / "moved").wstring(), "directory rename paths wrong");
    std::ofstream(root / "replacement.txt") << "replacement";
    Require(catalog.Pump(error), error.c_str());
    std::filesystem::rename(root / "renamed.txt", root / "replacement.txt");
    Require(catalog.Pump(error), error.c_str());
    Require(catalog.Search("renamed", "", 10).empty(), "replacement retained source name");
    Require(catalog.Search("replacement", "", 10).size() == 1, "replacement duplicated destination");
    std::filesystem::remove(root / "moved" / "live.txt");
    Require(catalog.Pump(error), error.c_str());
    Require(catalog.Search("live", "", 10).empty(), "deleted file remains");
    Require(!catalog.Changes(position).events.empty(), "changes missing");
    Require(catalog.Reconcile(error), error.c_str());
    Require(catalog.Changes(position).expired, "reconcile did not reset history");
    std::cout << "linux_catalog_test passed\n";
}

#include "linux_store.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <set>
#include <unistd.h>
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    std::string pattern = "/home/donggua/filesearch-store-XXXXXX";
    auto temporary = mkdtemp(pattern.data());
    Require(temporary != nullptr, "fixture creation failed");
    std::filesystem::path root(temporary);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    const auto path = (root / "catalog.db").string();
    std::string error;
    std::vector<fsengine::LinuxScanEntry> loaded;
    const std::string raw_name = std::string("raw-") + char(0xff);
    fsengine::LinuxScanEntry baseline{"/" + raw_name, raw_name, UINT64_MAX, 7, false, true};
    baseline.object_key = std::string("handle\0generation", 17);
    {
        fsengine::LinuxStore store;
        Require(store.Open(path, error), error.c_str());
        std::uint64_t first = 0;
        Require(store.ReserveIds(10, first, error) && first == 1, "initial ID reservation failed");
        baseline.id = first;
        Require(store.Replace({baseline}, error, "/cached-root"), error.c_str());
        std::string cached_root;
        Require(store.CachedRoot(cached_root, error) && cached_root == "/cached-root", "cache root not committed");
        Require(!store.Replace({{"relative", "bad"}}, error), "invalid snapshot committed");
        Require(store.CachedRoot(cached_root, error) && cached_root == "/cached-root", "rollback lost cache binding");
        Require(store.Load(loaded, error), error.c_str());
        Require(loaded.size() == 1 && loaded[0].path == baseline.path, "snapshot rollback failed");
        Require(!store.Apply({{"relative", "bad"}}, {baseline.path}, error), "invalid delta committed");
        Require(store.Load(loaded, error), error.c_str());
        Require(loaded.size() == 1, "delta rollback lost deletion");
        auto duplicate = baseline;
        duplicate.path = "/duplicate-id";
        Require(!store.Apply({duplicate}, {}, error), "duplicate entry ID accepted");
        Require(store.Load(loaded, error) && loaded.size() == 1 && loaded[0].path == baseline.path,
            "duplicate ID write damaged original entry");
    }
    {
        fsengine::LinuxStore reopened;
        Require(reopened.Open(path, error), error.c_str());
        Require(reopened.Load(loaded, error), error.c_str());
        std::uint64_t next = 0;
        Require(reopened.ReserveIds(10, next, error) && next == 11, "restart reused ID reservation");
        Require(loaded.size() == 1 && loaded[0].name == raw_name && loaded[0].inode == UINT64_MAX && loaded[0].symlink && loaded[0].id == 1 &&
            loaded[0].object_key == baseline.object_key,
            "reopen lost raw bytes or metadata");
        Require(reopened.Apply({{"/new", "new", 3, 7, true, false}}, {baseline.path}, error), error.c_str());
        Require(reopened.Load(loaded, error), error.c_str());
        Require(loaded.size() == 1 && loaded[0].path == "/new" && loaded[0].directory, "incremental transaction wrong");
    }
    {
        fsengine::LinuxStore store;
        Require(store.Open((root / "snapshot.db").string(), error), "snapshot fixture open failed");
        std::vector<fsengine::LinuxScanEntry> initial{
            {"/scope", "scope", 1, 7, true, false, 1},
            {"/scope/a", "a", 2, 7, false, false, 2},
            {"/scope/b", "b", 3, 7, false, false, 3},
            {"/scope-sibling/a", "a", 4, 7, false, false, 4},
            {"/" + raw_name + "/child", "child", 5, 7, false, false, 5},
            {"/" + raw_name, raw_name, 6, 7, true, false, 6}};
        Require(store.Replace(initial, error), "snapshot fixture replacement failed");
        auto snapshot = store.Snapshot("", true, true, error);
        Require(snapshot != nullptr, "snapshot open failed");
        auto first = snapshot->NextPage(1);
        Require(first.entries.size() == 1 && !first.done, "snapshot first page wrong");
        auto renamed = initial[2]; renamed.path = "/scope/c"; renamed.name = "c";
        Require(store.Apply({renamed}, {"/scope/a", "/scope/b"}, error), "writer blocked by read snapshot");
        std::set<std::string> captured{first.entries[0].path}, expected;
        for (const auto& entry : initial) expected.insert(entry.path);
        bool done = false;
        while (!done) {
            auto page = snapshot->NextPage(1024, 700);
            Require(page.entries.size() == 1, "snapshot byte budget did not retain next row");
            captured.insert(page.entries[0].path); done = page.done;
        }
        Require(captured == expected, "snapshot changed between pages after concurrent commit");
        Require(snapshot->NextPage(1).done, "finished snapshot restarted");
        auto scoped = store.Snapshot("/scope", true, false, error);
        Require(scoped != nullptr, "scoped snapshot failed");
        auto page = scoped->NextPage(100);
        Require(page.done && page.entries.size() == 1 && page.entries[0].path == "/scope/c", "scope boundary or file filter wrong");
        auto raw = store.Snapshot("/" + raw_name, false, true, error);
        Require(raw != nullptr, "raw scope snapshot failed");
        page = raw->NextPage(100);
        Require(page.done && page.entries.size() == 1 && page.entries[0].name == raw_name, "raw bytes or directory filter lost");
        auto empty = store.Snapshot("", false, false, error);
        Require(empty != nullptr && empty->NextPage(1).entries.empty(), "empty filter returned entries");
        bool rejected = false;
        try { snapshot->NextPage(0); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "zero snapshot page size accepted");
    }
    std::ofstream(root / "corrupt.db") << "not a database";
    fsengine::LinuxStore corrupt;
    Require(!corrupt.Open((root / "corrupt.db").string(), error), "corrupt database accepted");
    std::string quarantine;
    Require(corrupt.OpenRecovering((root / "corrupt.db").string(), quarantine, error), error.c_str());
    Require(!quarantine.empty() && std::filesystem::exists(std::filesystem::path(quarantine) / "corrupt.db"), "corrupt cache not preserved");
    std::ifstream preserved(std::filesystem::path(quarantine) / "corrupt.db");
    std::string contents; std::getline(preserved, contents);
    Require(contents == "not a database", "quarantine changed corrupt bytes");
    Require(corrupt.Load(loaded, error) && loaded.empty(), "replacement database not empty");
    {
        const auto invalid = root / "directory.db";
        std::filesystem::create_directory(invalid);
        fsengine::LinuxStore unavailable;
        Require(!unavailable.OpenRecovering(invalid.string(), quarantine, error), "non-corruption failure recovered");
        Require(quarantine.empty() && std::filesystem::is_directory(invalid), "non-corruption failure moved original");
    }
    {
        const auto linked = root / "linked.db";
        std::ofstream(linked) << "corrupt linked database";
        std::filesystem::create_hard_link(linked, root / "linked-copy.db");
        fsengine::LinuxStore unsafe;
        Require(!unsafe.OpenRecovering(linked.string(), quarantine, error), "hard-linked cache quarantined");
        Require(quarantine.empty() && std::filesystem::hard_link_count(linked) == 2, "unsafe recovery changed original");
    }
    std::cout << "linux_store_test passed\n";
}

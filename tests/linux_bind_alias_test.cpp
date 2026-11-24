#include "linux_catalog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/mount.h>
#include <unistd.h>

namespace fs = std::filesystem;
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
int main() {
    if (geteuid() != 0) return 77;
    std::string pattern = "/home/donggua/filesearch-bind-XXXXXX";
    Require(mkdtemp(pattern.data()) != nullptr, "fixture creation failed");
    fs::path base(pattern), root = base / "files", source = root / "source", alias = root / "alias";
    fs::create_directories(source / "nested");
    fs::create_directory(alias);
    fs::create_directory(base / "ipc");
    std::ofstream(source / "baseline.txt") << "baseline";
    Require(mount(source.c_str(), alias.c_str(), nullptr, MS_BIND, nullptr) == 0, "bind mount failed");
    struct Cleanup {
        fs::path base, alias;
        ~Cleanup() { umount2(alias.c_str(), MNT_DETACH); fs::remove_all(base); }
    } cleanup{base, alias};
    fsengine::LinuxCatalog catalog;
    std::string error;
    Require(catalog.Start(root.string(), error, (base / "ipc/catalog.db").string()), error);
    auto baseline = catalog.Search("baseline", "", 10);
    Require(baseline.size() == 2 && baseline[0].id != baseline[1].id, "baseline aliases missing or IDs merged");
    auto verify = [&](const std::string& query, const std::vector<fs::path>& expected) {
        Require(catalog.Pump(error), error);
        std::set<std::string> paths;
        for (const auto& entry : catalog.Search(query, "", 20)) paths.insert(entry.path);
        std::set<std::string> wanted;
        for (const auto& path : expected) wanted.insert(path.string());
        Require(paths == wanted, "alias search paths incorrect");
    };
    auto events = [&](fsengine::ChangePosition cursor, fsengine::FsEventType type, std::size_t count) {
        const auto changes = catalog.Changes(cursor);
        Require(!changes.expired, "unexpected alias resync");
        std::size_t found = 0;
        for (const auto& event : changes.events) if (event.type == type) ++found;
        Require(found == count, "alias change count incorrect");
    };
    auto cursor = catalog.Position();
    std::ofstream(alias / "nested" / "live.txt") << "create through alias";
    verify("live", {source / "nested/live.txt", alias / "nested/live.txt"});
    events(cursor, fsengine::FsEventType::FileCreated, 2);
    auto original = catalog.Search("live", "", 10);
    cursor = catalog.Position();
    fs::rename(source / "nested/live.txt", source / "nested/renamed.txt");
    verify("live", {});
    verify("renamed", {source / "nested/renamed.txt", alias / "nested/renamed.txt"});
    events(cursor, fsengine::FsEventType::FileRenamed, 2);
    auto renamed = catalog.Search("renamed", "", 10);
    Require(renamed[0].id == original[0].id && renamed[1].id == original[1].id, "alias rename changed entry IDs");
    cursor = catalog.Position();
    fs::rename(alias / "nested", alias / "moved");
    verify("renamed", {source / "moved/renamed.txt", alias / "moved/renamed.txt"});
    events(cursor, fsengine::FsEventType::DirRenamed, 2);
    // New children must use remapped directory handles in both views.
    std::ofstream(source / "moved/after.txt") << "after directory rename";
    verify("after", {source / "moved/after.txt", alias / "moved/after.txt"});
    cursor = catalog.Position();
    // Physical rename leaves the bind subtree: the source view sees a rename,
    // while the alias view sees a delete, not a rename into an unrelated alias.
    fs::rename(source / "moved/renamed.txt", root / "outside.txt");
    verify("renamed", {});
    verify("outside", {root / "outside.txt"});
    events(cursor, fsengine::FsEventType::FileRenamed, 1);
    events(cursor, fsengine::FsEventType::FileDeleted, 1);
    cursor = catalog.Position();
    fs::rename(root / "outside.txt", source / "moved/returned.txt");
    verify("outside", {});
    verify("returned", {source / "moved/returned.txt", alias / "moved/returned.txt"});
    events(cursor, fsengine::FsEventType::FileRenamed, 1);
    events(cursor, fsengine::FsEventType::FileCreated, 1);
    cursor = catalog.Position();
    fs::remove(alias / "moved/returned.txt");
    verify("returned", {});
    events(cursor, fsengine::FsEventType::FileDeleted, 2);
    cursor = catalog.Position();
    std::ofstream(source / "moved/vanished.txt") << "short lived";
    fs::rename(source / "moved/vanished.txt", source / "moved/gone.txt");
    fs::remove(source / "moved/gone.txt");
    verify("gone", {});
    events(cursor, fsengine::FsEventType::FileCreated, 2);
    events(cursor, fsengine::FsEventType::FileRenamed, 2);
    events(cursor, fsengine::FsEventType::FileDeleted, 2);
    Require(catalog.Reconcile(error), error);
    verify("after", {source / "moved/after.txt", alias / "moved/after.txt"});
    // Renaming the physical bind source does not rename its mounted alias.
    cursor = catalog.Position();
    const auto relocated = root / "relocated";
    fs::rename(source, relocated);
    verify("after", {relocated / "moved/after.txt", alias / "moved/after.txt"});
    events(cursor, fsengine::FsEventType::DirRenamed, 1);
    std::ofstream(alias / "moved/final.txt") << "after source root rename";
    verify("final", {relocated / "moved/final.txt", alias / "moved/final.txt"});
    fsengine::LinuxStore stored;
    Require(stored.Open((base / "ipc/catalog.db").string(), error), error);
    std::vector<fsengine::LinuxScanEntry> durable;
    Require(stored.Load(durable, error), error);
    std::set<std::string> actual, expected;
    for (const auto& entry : durable) actual.insert(entry.path);
    for (const auto& entry : fs::recursive_directory_iterator(root)) expected.insert(entry.path().string());
    Require(actual == expected, "durable alias catalog differs from visible filesystem");
    std::cout << "linux_bind_alias_test passed\n";
}

#include "linux_scanner.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    std::ifstream mountinfo("/proc/self/mountinfo");
    const auto mounts = fsengine::ReadLinuxMounts(mountinfo);
    std::string pattern = std::string(getenv("HOME")) + "/filesearch-scanner-XXXXXX";
    char* created = mkdtemp(pattern.data());
    Require(created != nullptr, "cannot create fixture");
    const std::filesystem::path root(created);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    std::filesystem::create_directory(root / "nested");
    std::ofstream(root / "nested" / "中文.txt") << "test";
    std::filesystem::create_hard_link(root / "nested" / "中文.txt", root / "hardlink.txt");
    std::filesystem::create_symlink(root, root / "cycle");
    std::filesystem::create_symlink("/mnt/c", root / "host");
    auto scanned = fsengine::ScanLinuxTree(root.string(), mounts);
    Require(scanned.Complete(), "fixture scan incomplete");
    Require(scanned.entries.size() == 5, "wrong entry count or symlink followed");
    auto trailing = fsengine::ScanLinuxTree(root.string() + "/", mounts);
    Require(trailing.Complete() && trailing.entries.size() == scanned.entries.size(), "trailing scan root failed");
    for (std::size_t n = 0; n < trailing.entries.size(); ++n)
        Require(trailing.entries[n].path == scanned.entries[n].path, "trailing root changed canonical paths");
    for (const char* host : {"/mnt/c", "/mnt/d", "/mnt/e"}) {
        auto excluded = fsengine::ScanLinuxTree(host, mounts);
        Require(excluded.entries.empty() && excluded.excluded == 1, "host scan accepted");
    }
    auto symlink = fsengine::ScanLinuxTree((root / "cycle").string(), mounts);
    Require(!symlink.Complete(), "root symlink followed");
    auto vanished = fsengine::ScanLinuxTree((root / "vanished").string(), mounts, {}, true);
    Require(vanished.raced && vanished.errors.empty() && !vanished.Complete(), "vanished subtree not classified as retryable");
    auto replaced = fsengine::ScanLinuxTree((root / "cycle").string(), mounts, {}, true);
    Require(replaced.raced && replaced.errors.empty(), "replaced subtree not classified as retryable");
    std::cout << "linux_scanner_test passed\n";
}

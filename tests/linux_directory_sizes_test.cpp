#include "linux_directory_sizes.h"
#include <iostream>
#include <stdexcept>
using namespace fsengine;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
LinuxScanEntry Entry(std::string path, bool directory, std::uint64_t size = 0) {
    LinuxScanEntry entry; entry.path = path; entry.directory = directory;
    entry.regular = !directory; entry.metadata_known = true;
    entry.logical_size = size; entry.allocated_size = size ? 4096 : 0; entry.checked_at_ms = 10;
    return entry;
}
int main() {
    LinuxDirectorySizes sizes; sizes.Reset("/tree");
    auto a = Entry("/tree/a", true), b = Entry("/tree/b", true), nested = Entry("/tree/a/sub", true);
    sizes.Add(a); sizes.Add(b); sizes.Add(nested);
    auto file = Entry("/tree/a/sub/file", false, 100), empty = Entry("/tree/a/zero", false);
    sizes.Add(file); sizes.Add(empty);
    Require(sizes.Get("/tree")->logical_bytes == 100 && sizes.Get("/tree/a")->files == 2, "nested aggregation");
    Require(sizes.Get("/tree/b")->logical_bytes == 0, "empty directory is a known zero");
    auto link = Entry("/tree/b/link", false, 900); link.symlink = true; sizes.Add(link);
    Require(sizes.Get("/tree/b")->files == 0, "symlink must not be traversed or counted");
    sizes.Remove(file); file.logical_size = 500; sizes.Add(file);
    Require(sizes.Get("/tree")->logical_bytes == 500, "overwrite must replace old size");
    sizes.Remove(nested); sizes.Remove(file); nested.path = "/tree/b/sub"; file.path = "/tree/b/sub/file";
    sizes.Add(nested); sizes.Add(file);
    Require(sizes.Get("/tree/a")->logical_bytes == 0 && sizes.Get("/tree/b")->logical_bytes == 500 && sizes.Get("/tree")->logical_bytes == 500, "subtree move must not double-count");
    auto alias = file; alias.path = "/tree/a/hardlink"; sizes.Add(alias);
    Require(sizes.Get("/tree")->logical_bytes == 1000, "logical totals count each hardlink pathname");
    auto legacy = Entry("/tree/a/legacy", false); legacy.metadata_known = false; sizes.Add(legacy);
    Require(sizes.Get("/tree/a")->unknown == 1, "legacy unknown size must not look like a known zero");
    sizes.Remove(legacy); sizes.Remove(alias); sizes.Remove(nested); sizes.Remove(file); sizes.Remove(b);
    Require(!sizes.Get("/tree/b") && sizes.Get("/tree")->logical_bytes == 0, "removed directories must disappear");
    std::cout << "directory aggregation passed\n";
}

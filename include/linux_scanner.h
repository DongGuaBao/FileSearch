#pragma once
#include "linux_mounts.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
struct stat;

namespace fsengine {
struct LinuxScanEntry {
    std::string path;
    std::string name;
    std::uint64_t inode = 0;
    std::uint64_t device = 0;
    bool directory = false;
    bool symlink = false;
    std::uint64_t id = 0;
    std::string object_key;
    std::uint64_t logical_size = 0;
    std::uint64_t allocated_size = 0;
    std::int64_t modified_ns = 0;
    std::int64_t checked_at_ms = 0;
    bool regular = false;
    bool metadata_known = false;
};
void UpdateLinuxMetadata(LinuxScanEntry& entry, const struct stat& state);
struct LinuxScanOptions {
    std::size_t batch_entries = 0;
    unsigned pause_ms = 0;
    std::function<bool()> cancelled;
    std::function<void(const LinuxScanEntry&)> observed;
};
struct LinuxScanResult {
    std::string root_object_key;
    std::vector<LinuxScanEntry> entries;
    std::vector<std::string> errors;
    std::size_t excluded = 0;
    bool raced = false;
    bool cancelled = false;
    bool Complete() const { return errors.empty() && !raced && !cancelled; }
};
bool IsLinuxExcludedPath(const std::string& path, const std::vector<std::string>& excluded);
LinuxScanResult ScanLinuxTree(const std::string& root, const std::vector<LinuxMount>& mounts,
    const std::vector<std::string>& excluded = {}, bool allow_root_race = false, const LinuxScanOptions& options = {});
}

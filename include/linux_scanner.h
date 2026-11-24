#pragma once
#include "linux_mounts.h"
#include <cstdint>
#include <string>
#include <vector>

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
};
struct LinuxScanResult {
    std::string root_object_key;
    std::vector<LinuxScanEntry> entries;
    std::vector<std::string> errors;
    std::size_t excluded = 0;
    bool raced = false;
    bool Complete() const { return errors.empty() && !raced; }
};
bool IsLinuxExcludedPath(const std::string& path, const std::vector<std::string>& excluded);
LinuxScanResult ScanLinuxTree(const std::string& root, const std::vector<LinuxMount>& mounts,
    const std::vector<std::string>& excluded = {}, bool allow_root_race = false);
}

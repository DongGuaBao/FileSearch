#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace fsengine {

struct LinuxMount {
    std::uint64_t id = 0;
    std::string device;
    std::string root;
    std::string path;
    std::string filesystem;
    std::string options;
    bool eligible = false;
};

std::vector<LinuxMount> ReadLinuxMounts(std::istream& input);
bool IsLinuxCatalogPath(const std::vector<LinuxMount>& mounts, const std::string& absolute_path);
std::vector<LinuxMount> SelectLinuxCatalogMounts(const std::vector<LinuxMount>& mounts);

} // namespace fsengine

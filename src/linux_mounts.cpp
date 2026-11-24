#include "linux_mounts.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>

namespace fsengine {
namespace {
std::string Decode(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size() &&
            value[i+1] >= '0' && value[i+1] <= '7' &&
            value[i+2] >= '0' && value[i+2] <= '7' &&
            value[i+3] >= '0' && value[i+3] <= '7') {
            result.push_back(static_cast<char>((value[i+1]-'0')*64 + (value[i+2]-'0')*8 + value[i+3]-'0'));
            i += 3;
        } else result.push_back(value[i]);
    }
    return result;
}
bool Within(const std::string& path, const std::string& root) {
    return root == "/" || path == root ||
        (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
bool HostPath(const std::string& path) {
    return path.size() >= 6 && path.compare(0, 5, "/mnt/") == 0 &&
        path[5] >= 'a' && path[5] <= 'z' && (path.size() == 6 || path[6] == '/');
}
}

std::vector<LinuxMount> ReadLinuxMounts(std::istream& input) {
    std::vector<LinuxMount> result;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) continue;
        LinuxMount mount;
        std::uint64_t parent;
        std::istringstream before(line.substr(0, separator));
        std::istringstream after(line.substr(separator + 3));
        std::string source, super_options;
        if (!(before >> mount.id >> parent >> mount.device >> mount.root >> mount.path >> mount.options) ||
            !(after >> mount.filesystem >> source >> super_options)) continue;
        mount.path = Decode(mount.path);
        mount.root = Decode(mount.root);
        mount.eligible = (mount.filesystem == "ext4" || mount.filesystem == "xfs" || mount.filesystem == "btrfs") &&
            !HostPath(mount.path) && !Within(mount.path, "/mnt/wslg");
        result.push_back(std::move(mount));
    }
    return result;
}

bool IsLinuxCatalogPath(const std::vector<LinuxMount>& mounts, const std::string& absolute_path) {
    if (absolute_path.empty() || absolute_path.front() != '/') return false;
    const std::string path = std::filesystem::path(absolute_path).lexically_normal().string();
    if (HostPath(path) || Within(path, "/mnt/wslg")) return false;
    const LinuxMount* selected = nullptr;
    for (const auto& mount : mounts) {
        // Equal-length overmounts: the last mountinfo entry wins.
        if (Within(path, mount.path) && (!selected || mount.path.size() >= selected->path.size())) selected = &mount;
    }
    return selected && selected->eligible;
}

std::vector<LinuxMount> SelectLinuxCatalogMounts(const std::vector<LinuxMount>& mounts) {
    std::vector<LinuxMount> result;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& mount : mounts) {
        if (mount.eligible && seen.emplace(mount.device, mount.root).second) result.push_back(mount);
    }
    return result;
}
} // namespace fsengine

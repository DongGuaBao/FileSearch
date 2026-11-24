#include "linux_directory_sizes.h"
#include <filesystem>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <chrono>
namespace fsengine {
void LinuxDirectorySizes::MarkValidated(std::int64_t at) {
    for(auto& [path,total]:totals_)if(directories_.count(path))total.checked_at_ms=at;
}
void LinuxDirectorySizes::Reset(const std::string& root) {
    root_ = root; directories_.clear(); totals_.clear(); changed_.clear(); changed_.insert(root);
    directories_.insert(root); totals_[root] = {};
}
void LinuxDirectorySizes::Add(const LinuxScanEntry& entry) {
    if (entry.directory) changed_.insert(entry.path);
    if (entry.directory) { directories_.insert(entry.path); totals_.try_emplace(entry.path); }
    else Adjust(entry, true);
}
void LinuxDirectorySizes::Remove(const LinuxScanEntry& entry) {
    if (entry.directory) changed_.insert(entry.path);
    if (entry.directory) {
        directories_.erase(entry.path);
        auto found = totals_.find(entry.path);
        if (found != totals_.end() && !found->second.files && !found->second.unknown) totals_.erase(found);
    } else Adjust(entry, false);
}
void LinuxDirectorySizes::Adjust(const LinuxScanEntry& entry, bool add) {
    if (entry.symlink || (entry.metadata_known && !entry.regular)) return;
    auto path = std::filesystem::path(entry.path).parent_path().string();
    while (!path.empty() && (root_ == "/" || path == root_ || path.starts_with(root_ + "/"))) {
        changed_.insert(path);
        auto& total = totals_[path];
        auto update = [&](std::uint64_t& value, std::uint64_t amount) {
            if (add) {
                if (amount > std::numeric_limits<std::uint64_t>::max() - value) throw std::overflow_error("directory size overflow");
                value += amount;
            } else {
                if (value < amount) throw std::logic_error("directory size underflow");
                value -= amount;
            }
        };
        if (!entry.metadata_known) update(total.unknown, 1);
        else { update(total.files, 1); update(total.logical_bytes, entry.logical_size); update(total.allocated_bytes, entry.allocated_size); }
        const auto observed = add ? entry.checked_at_ms : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        total.checked_at_ms = std::max(total.checked_at_ms, static_cast<std::int64_t>(observed));
        if (!directories_.count(path) && !total.files && !total.unknown) totals_.erase(path);
        if (path == root_ || path == "/") break;
        path = std::filesystem::path(path).parent_path().string();
    }
}
std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> LinuxDirectorySizes::TakeChanges() {
    std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> result;
    for (const auto& path : changed_) { if (directories_.count(path) || !totals_.count(path)) result.emplace_back(path, Get(path)); }
    changed_.clear(); return result;
}
std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> LinuxDirectorySizes::All() const {
    std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> result;
    for (const auto& path : directories_) result.emplace_back(path, Get(path));
    return result;
}
std::optional<LinuxDirectorySize> LinuxDirectorySizes::Get(const std::string& path) const {
    if (!directories_.count(path)) return {};
    auto found = totals_.find(path);
    return found == totals_.end() ? LinuxDirectorySize{} : found->second;
}
}

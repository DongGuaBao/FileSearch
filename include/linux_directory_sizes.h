#pragma once
#include "linux_scanner.h"
#include <map>
#include <set>
#include <optional>

namespace fsengine {
struct LinuxDirectorySize {
    std::uint64_t logical_bytes = 0;
    std::uint64_t allocated_bytes = 0; // Per pathname, including hardlink aliases.
    std::uint64_t files = 0;
    std::uint64_t unknown = 0;
    std::int64_t checked_at_ms = 0;
};
// Only directory aggregates are duplicated; file facts remain in the catalog.
class LinuxDirectorySizes {
public:
    std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> TakeChanges();
    std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>> All() const;
    void MarkValidated(std::int64_t at);
    void Reset(const std::string& root);
    void Add(const LinuxScanEntry& entry);
    void Remove(const LinuxScanEntry& entry);
    std::optional<LinuxDirectorySize> Get(const std::string& path) const;
private:
    void Adjust(const LinuxScanEntry& entry, bool add);
    std::set<std::string> changed_;
    std::string root_;
    std::set<std::string> directories_;
    std::map<std::string, LinuxDirectorySize> totals_;
};
}

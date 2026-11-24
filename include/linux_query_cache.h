#pragma once
#include "linux_directory_sizes.h"
#include <mutex>
#include <map>
#include <sqlite3.h>

namespace fsengine {
// Small, independently readable cache. Never requires rebuilding the name index.
class LinuxQueryCache {
public:
    LinuxQueryCache(const std::string& database, const std::string& root);
    ~LinuxQueryCache();
    struct Size { std::optional<LinuxDirectorySize> value; bool complete = false; };
    std::vector<Size> Read(const std::vector<std::string>& paths);
    void Write(const std::vector<std::pair<std::string, std::optional<LinuxDirectorySize>>>& changes, bool complete, bool replace = false);
    void Clear();
private:
    std::string database_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};
// Bounded read-only fallback while the in-memory search index is initializing.
std::vector<LinuxScanEntry> SearchLinuxStored(const std::string& database, const std::string& root,
    const std::string& scope, const std::string& query, std::size_t limit, bool& interrupted);
}

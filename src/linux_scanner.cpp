#include "linux_scanner.h"
#include "linux_fanotify.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <limits>

namespace fsengine {
namespace {
struct CloseDirectory { void operator()(DIR* directory) const { closedir(directory); } };
int OpenDirectory(const std::string& path) {
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (const auto& part : std::filesystem::path(path).relative_path()) {
        if (part.empty() || part == ".") continue;
        const int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        const int failure = errno;
        close(fd);
        if (next < 0) { errno = failure; return -1; }
        fd = next;
    }
    return fd;
}
void Walk(int fd, const std::string& path, const std::vector<LinuxMount>& mounts,
          const std::vector<std::string>& excluded, LinuxScanResult& result, const LinuxScanOptions& options, std::size_t& visited) {
    // fdopendir takes ownership, including on all exits from this function.
    std::unique_ptr<DIR, CloseDirectory> directory(fdopendir(fd));
    if (!directory) {
        const int error = errno;
        close(fd);
        result.errors.push_back(path + ": " + std::strerror(error));
        return;
    }
    for (;;) {
        if (options.cancelled && options.cancelled()) { result.cancelled = true; return; }
        errno = 0;
        dirent* item = readdir(directory.get());
        if (!item) {
            if (errno == ENOENT) result.raced = true;
            else if (errno) result.errors.push_back(path + ": " + std::strerror(errno));
            break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") continue;
        if (options.batch_entries && ++visited % options.batch_entries == 0 && options.pause_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pause_ms));
        const std::string child = path == "/" ? path + name : path + "/" + name;
        if (!IsLinuxCatalogPath(mounts, child) || IsLinuxExcludedPath(child, excluded)) { ++result.excluded; continue; }
        // Capture metadata and generation from the same opened object, not two
        // independent path resolutions that can refer to different generations.
        const int object = openat(dirfd(directory.get()), name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
        if (object < 0) {
            if (errno != ENOENT) result.errors.push_back(child + ": " + std::strerror(errno));
            continue;
        }
        struct CloseObject { int fd; ~CloseObject() { close(fd); } } object_guard{object};
        struct stat state{};
        if (fstat(object, &state) < 0) { result.errors.push_back(child + ": " + std::strerror(errno)); continue; }
        result.entries.push_back({child, name, static_cast<std::uint64_t>(state.st_ino),
            static_cast<std::uint64_t>(state.st_dev), S_ISDIR(state.st_mode), S_ISLNK(state.st_mode),
            0, LinuxFanotify::ObjectHandle(object)});
        UpdateLinuxMetadata(result.entries.back(), state);
        if (options.observed) options.observed(result.entries.back());
        if (!S_ISDIR(state.st_mode)) continue;
        const int child_fd = openat(dirfd(directory.get()), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child_fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) result.raced = true;
            else result.errors.push_back(child + ": " + std::strerror(errno));
            continue;
        }
        struct stat opened{};
        if (fstat(child_fd, &opened) < 0) {
            const int failure = errno;
            close(child_fd);
            result.errors.push_back(child + ": " + std::strerror(failure));
            continue;
        }
        if (opened.st_ino != state.st_ino || opened.st_dev != state.st_dev) {
            close(child_fd);
            result.raced = true;
            continue;
        }
        Walk(child_fd, child, mounts, excluded, result, options, visited);
        if (result.cancelled) return;
        // The directory may have moved while its fd stayed valid. Do not
        // publish a subtree using its former pathname.
        struct stat current{};
        if (fstatat(dirfd(directory.get()), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT || errno == ENOTDIR) result.raced = true;
            else result.errors.push_back(child + ": " + std::strerror(errno));
        } else if (current.st_ino != state.st_ino || current.st_dev != state.st_dev || !S_ISDIR(current.st_mode))
            result.raced = true;
    }
}
}
void UpdateLinuxMetadata(LinuxScanEntry& entry, const struct stat& state) {
    entry.regular = S_ISREG(state.st_mode);
    entry.logical_size = entry.regular && state.st_size > 0 ? static_cast<std::uint64_t>(state.st_size) : 0;
    const auto blocks = entry.regular && state.st_blocks > 0 ? static_cast<std::uint64_t>(state.st_blocks) : 0;
    const bool allocation_overflow = blocks > std::numeric_limits<std::uint64_t>::max() / 512;
    entry.allocated_size = allocation_overflow ? 0 : blocks * 512;
    const auto seconds = static_cast<std::int64_t>(state.st_mtim.tv_sec);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (seconds > maximum / 1000000000LL) entry.modified_ns = maximum;
    else if (seconds < minimum / 1000000000LL) entry.modified_ns = minimum;
    else {
        const auto base = seconds * 1000000000LL;
        entry.modified_ns = base > maximum - state.st_mtim.tv_nsec ? maximum : base + state.st_mtim.tv_nsec;
    }
    entry.checked_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    entry.metadata_known = !allocation_overflow;
}
bool IsLinuxExcludedPath(const std::string& path, const std::vector<std::string>& excluded) {
    for (const auto& root : excluded) {
        if (root == "/" || path == root || (path.size() > root.size() &&
            path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')) return true;
    }
    return false;
}
LinuxScanResult ScanLinuxTree(const std::string& root, const std::vector<LinuxMount>& mounts,
    const std::vector<std::string>& excluded, bool allow_root_race, const LinuxScanOptions& options) {
    LinuxScanResult result;
    if (root.empty() || root.front() != '/') {
        result.errors.push_back("scan root must be absolute");
        return result;
    }
    // Reject symlink traversal in the requested root as well as descendants.
    std::string path = std::filesystem::path(root).lexically_normal().string();
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (!IsLinuxCatalogPath(mounts, path) || IsLinuxExcludedPath(path, excluded)) { ++result.excluded; return result; }
    const int fd = OpenDirectory(path);
    if (fd < 0) {
        if (allow_root_race && (errno == ENOENT || errno == ENOTDIR || errno == ELOOP)) result.raced = true;
        else result.errors.push_back(path + ": " + std::strerror(errno));
        return result;
    }
    struct stat original{};
    if (fstat(fd, &original) < 0) {
        const int failure = errno;
        close(fd);
        result.errors.push_back(path + ": " + std::strerror(failure)); return result;
    }
    result.root_object_key = LinuxFanotify::ObjectHandle(fd);
    std::size_t visited = 0;
    Walk(fd, path, mounts, excluded, result, options, visited);
    // The requested scan root is itself a movable directory, too. Reopen all
    // components without following links before trusting its captured paths.
    const int current_fd = OpenDirectory(path);
    if (current_fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) result.raced = true;
        else result.errors.push_back(path + ": " + std::strerror(errno));
    } else {
        struct stat current{};
        if (fstat(current_fd, &current) < 0) result.errors.push_back(path + ": " + std::strerror(errno));
        else if (current.st_ino != original.st_ino || current.st_dev != original.st_dev ||
            (!result.root_object_key.empty() && result.root_object_key != LinuxFanotify::ObjectHandle(current_fd)))
            result.raced = true;
        close(current_fd);
    }
    return result;
}
}

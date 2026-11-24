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
          const std::vector<std::string>& excluded, LinuxScanResult& result) {
    // fdopendir takes ownership, including on all exits from this function.
    std::unique_ptr<DIR, CloseDirectory> directory(fdopendir(fd));
    if (!directory) {
        const int error = errno;
        close(fd);
        result.errors.push_back(path + ": " + std::strerror(error));
        return;
    }
    for (;;) {
        errno = 0;
        dirent* item = readdir(directory.get());
        if (!item) {
            if (errno == ENOENT) result.raced = true;
            else if (errno) result.errors.push_back(path + ": " + std::strerror(errno));
            break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") continue;
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
        Walk(child_fd, child, mounts, excluded, result);
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
bool IsLinuxExcludedPath(const std::string& path, const std::vector<std::string>& excluded) {
    for (const auto& root : excluded) {
        if (root == "/" || path == root || (path.size() > root.size() &&
            path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')) return true;
    }
    return false;
}
LinuxScanResult ScanLinuxTree(const std::string& root, const std::vector<LinuxMount>& mounts,
    const std::vector<std::string>& excluded, bool allow_root_race) {
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
    Walk(fd, path, mounts, excluded, result);
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

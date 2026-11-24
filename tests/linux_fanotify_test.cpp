#include "linux_fanotify.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/fanotify.h>
#include <unistd.h>

int main() {
    if (geteuid() != 0) { std::cout << "root required\n"; return 77; }
    std::string pattern = "/home/donggua/filesearch-fanotify-XXXXXX";
    char* temporary = mkdtemp(pattern.data());
    if (!temporary) throw std::runtime_error("fixture creation failed");
    const std::filesystem::path root(temporary);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    fsengine::LinuxFanotify watcher;
    std::string error;
    if (!watcher.Open(root.string(), error)) throw std::runtime_error(error);
    const std::string handle = fsengine::LinuxFanotify::DirectoryHandle(root.string());
    if (handle.empty()) throw std::runtime_error("directory handle unavailable");
    std::ofstream(root / "created.txt") << "test";
    std::filesystem::rename(root / "created.txt", root / "renamed.txt");
    std::filesystem::remove(root / "renamed.txt");
    bool created = false, deleted = false, moved_from = false, moved_to = false, written = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pollfd descriptor{watcher.Descriptor(), POLLIN, 0};
        poll(&descriptor, 1, 100);
        std::vector<fsengine::LinuxNotification> events;
        if (!watcher.Read(events, error)) throw std::runtime_error(error);
        for (const auto& event : events) {
            if (event.directory_handle != handle) continue;
            if (event.object_handle.empty()) throw std::runtime_error("target FID missing from watched event");
            created |= event.name == "created.txt" && (event.mask & FAN_CREATE);
            written |= event.name == "created.txt" && (event.mask & FAN_CLOSE_WRITE);
            moved_from |= event.old_directory_handle == handle && event.old_name == "created.txt" && (event.mask & FAN_RENAME);
            moved_to |= event.name == "renamed.txt" && (event.mask & FAN_RENAME);
            deleted |= event.name == "renamed.txt" && (event.mask & FAN_DELETE);
        }
        if (created && deleted && moved_from && moved_to && written) {
            std::cout << "linux_fanotify_test passed\n"; return 0;
        }
    }
    throw std::runtime_error("missing fanotify create/write/move/delete event");
}

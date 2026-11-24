#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fsengine {
struct LinuxNotification {
    std::uint64_t mask = 0;
    std::string directory_handle;
    std::string name;
    std::string old_directory_handle;
    std::string old_name;
    std::string object_handle;
    bool overflow = false;
};
// One group can mark multiple eligible filesystems. Handle keys include fsid.
class LinuxFanotify {
public:
    ~LinuxFanotify();
    LinuxFanotify() = default;
    LinuxFanotify(const LinuxFanotify&) = delete;
    LinuxFanotify& operator=(const LinuxFanotify&) = delete;
    bool Open(const std::string& filesystem_path, std::string& error);
    bool AddFilesystem(const std::string& filesystem_path, std::string& error);
    bool Read(std::vector<LinuxNotification>& events, std::string& error);
    // The production reader and malformed-buffer tests use the same decoder.
    // It has no descriptor side effects and commits no partial batch on error.
    static bool DecodeBuffer(const void* data, std::size_t size,
        std::vector<LinuxNotification>& events, std::string& error);
    int Descriptor() const { return fd_; }
    static std::string DirectoryHandle(const std::string& path);
    static std::string ObjectHandle(int fd);
private:
    int fd_ = -1;
};
}

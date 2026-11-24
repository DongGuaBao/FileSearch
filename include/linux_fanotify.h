#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <atomic>
#include <mutex>
#include <thread>

namespace fsengine {
struct LinuxNotification {
    std::uint64_t mask = 0;
    std::string directory_handle;
    std::string name;
    std::string old_directory_handle;
    std::string old_name;
    std::string object_handle;
    bool overflow = false;
    bool userspace_overflow = false;
    int process_id = 0;
};
// Preserves namespace order; only adjacent-generation metadata is coalesced.
class LinuxNotificationBuffer {
public:
    explicit LinuxNotificationBuffer(std::size_t limit = 262144) : limit_(limit) {}
    void Push(LinuxNotification event);
    std::vector<LinuxNotification> Take();
private:
    std::size_t limit_;
    bool overflow_ = false;
    std::vector<LinuxNotification> pending_;
    std::map<std::tuple<std::string,std::string,std::string>,std::size_t> metadata_;
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
    void SetIgnoredProcess(int process) { ignored_process_ = process; }
    bool Read(std::vector<LinuxNotification>& events, std::string& error);
    // The production reader and malformed-buffer tests use the same decoder.
    // It has no descriptor side effects and commits no partial batch on error.
    static bool DecodeBuffer(const void* data, std::size_t size,
        std::vector<LinuxNotification>& events, std::string& error);
    int Descriptor() const { return fd_; }
    static std::string DirectoryHandle(const std::string& path);
    static std::string ObjectHandle(int fd);
private:
    bool Drain(std::vector<LinuxNotification>& events, std::string& error);
    void Collect();
    int fd_ = -1;
    std::atomic<int> ignored_process_{0};
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex mutex_;
    LinuxNotificationBuffer pending_;
    std::string reader_error_;
};
}

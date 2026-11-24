#include "linux_fanotify.h"
#include <cerrno>
#include <cstring>
#include <iterator>
#include <linux/magic.h>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <poll.h>
#include <stdexcept>

namespace fsengine {
namespace {
constexpr int kBtrfsWithoutParent = 0x4d;
constexpr std::uint64_t kBtrfsTopLevel = 5;
constexpr std::uint64_t kBtrfsRootInode = 256;
std::string Key(const void* fsid, int type, const unsigned char* bytes, unsigned size) {
    std::string key(static_cast<const char*>(fsid), sizeof(fsid_t));
    key.append(reinterpret_cast<const char*>(&type), sizeof(type));
    key.append(reinterpret_cast<const char*>(bytes), size);
    return key;
}
}
LinuxFanotify::~LinuxFanotify() {
    stopping_ = true;
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) close(fd_);
}
bool LinuxFanotify::Open(const std::string& path, std::string& error) {
    if (fd_ >= 0) { error = "fanotify group already open"; return false; }
    fd_ = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK |
        FAN_REPORT_FID | FAN_REPORT_DIR_FID | FAN_REPORT_NAME | FAN_REPORT_TARGET_FID, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) { error = std::strerror(errno); return false; }
    if (!AddFilesystem(path, error)) { close(fd_); fd_ = -1; return false; }
    try { reader_ = std::thread([this] { Collect(); }); }
    catch (const std::exception& failure) { error=failure.what();close(fd_);fd_=-1;return false; }
    return true;
}
bool LinuxFanotify::AddFilesystem(const std::string& path, std::string& error) {
    const std::uint64_t mask = FAN_CREATE | FAN_DELETE | FAN_RENAME |
        FAN_MODIFY | FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_ONDIR;
    if (fanotify_mark(fd_, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, path.c_str()) < 0) {
        const int failure = errno;
        // FID-mode fanotify rejects a Btrfs subvolume path with EXDEV because
        // its statfs fsid differs from the superblock root. Locate the fixed
        // top-level root by a privileged kernel handle, without mounting or
        // changing the filesystem, then mark that same superblock instead.
        if (failure == EXDEV) {
            // open_by_handle_at's mount descriptor cannot be an O_PATH fd.
            const int mount = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (mount >= 0) {
                struct Close { int fd; ~Close() { close(fd); } } mount_guard{mount};
                struct statfs filesystem{};
                if (fstatfs(mount, &filesystem) == 0 && filesystem.f_type == BTRFS_SUPER_MAGIC) {
                    std::vector<unsigned char> storage(sizeof(file_handle) + 20);
                    auto* handle = reinterpret_cast<file_handle*>(storage.data());
                    handle->handle_type = kBtrfsWithoutParent;
                    handle->handle_bytes = 20;
                    std::memcpy(handle->f_handle, &kBtrfsRootInode, sizeof(kBtrfsRootInode));
                    std::memcpy(handle->f_handle + 8, &kBtrfsTopLevel, sizeof(kBtrfsTopLevel));
                    // A zero generation is a locator, not an identity. The
                    // kernel validates the directory; real identities always
                    // come from ObjectHandle on the opened object.
                    const int top = open_by_handle_at(mount, handle, O_PATH | O_CLOEXEC);
                    if (top >= 0) {
                        struct Close top_guard{top};
                        const auto descriptor_path = "/proc/self/fd/" + std::to_string(top);
                        if (fanotify_mark(fd_, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask,
                            AT_FDCWD, descriptor_path.c_str()) == 0) return true;
                    }
                    error = "cannot mark Btrfs top-level filesystem: " + std::string(std::strerror(errno));
                    return false;
                }
            }
        }
        error = std::strerror(failure); return false;
    }
    return true;
}
std::string LinuxFanotify::DirectoryHandle(const std::string& path) {
    const int object = open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (object < 0) return {};
    struct Close { int fd; ~Close() { close(fd); } } guard{object};
    return ObjectHandle(object);
}
std::string LinuxFanotify::ObjectHandle(int object) {
    std::vector<unsigned char> storage(sizeof(file_handle) + 128);
    auto* handle = reinterpret_cast<file_handle*>(storage.data());
    handle->handle_bytes = 128;
    int mount = 0;
    if (name_to_handle_at(object, "", handle, &mount, AT_EMPTY_PATH) < 0) return {};
    struct statfs filesystem{};
    if (fstatfs(object, &filesystem) < 0) return {};
    if (filesystem.f_type == BTRFS_SUPER_MAGIC) {
        if (handle->handle_type < kBtrfsWithoutParent || handle->handle_type > kBtrfsWithoutParent + 2 || handle->handle_bytes < 20) return {};
        std::uint64_t root = 0;
        std::memcpy(&root, handle->f_handle + 8, sizeof(root));
        // btrfs_statfs XORs the subvolume ID into fsid; fanotify always uses
        // the superblock root's cached fsid. The FID itself still retains the
        // real subvolume ID and generation, so equal inode numbers stay distinct.
        static_assert(sizeof(fsid_t) == 8);
        std::uint32_t words[2];
        std::memcpy(words, &filesystem.f_fsid, sizeof(words));
        words[0] ^= static_cast<std::uint32_t>(root >> 32);
        words[1] ^= static_cast<std::uint32_t>(root) ^ static_cast<std::uint32_t>(kBtrfsTopLevel);
        std::memcpy(&filesystem.f_fsid, words, sizeof(words));
    }
    return Key(&filesystem.f_fsid, handle->handle_type, handle->f_handle, handle->handle_bytes);
}
bool LinuxFanotify::DecodeBuffer(const void* data, std::size_t size,
    std::vector<LinuxNotification>& events, std::string& error) {
        if (!data || !size) { error = "empty fanotify buffer"; return false; }
        const auto* buffer = static_cast<const unsigned char*>(data);
        std::vector<LinuxNotification> decoded;
        std::size_t offset = 0;
        while (size - offset >= sizeof(fanotify_event_metadata)) {
            // Information lengths are kernel-controlled. Copy headers rather
            // than dereferencing potentially unaligned records in a bad batch.
            fanotify_event_metadata metadata_value{};
            std::memcpy(&metadata_value, buffer + offset, sizeof(metadata_value));
            const auto* metadata = &metadata_value;
            if (metadata->vers != FANOTIFY_METADATA_VERSION || metadata->event_len < sizeof(*metadata) ||
                metadata->event_len > size - offset || metadata->metadata_len < sizeof(*metadata) ||
                metadata->metadata_len > metadata->event_len || metadata->fd != FAN_NOFD) {
                error = "invalid fanotify metadata"; return false;
            }
            LinuxNotification event;
            event.process_id = metadata->pid;
            event.mask = metadata->mask;
            event.overflow = (metadata->mask & FAN_Q_OVERFLOW) != 0;
            const std::size_t end = offset + metadata->event_len;
            std::size_t info_offset = offset + metadata->metadata_len;
            bool target_seen = false, old_seen = false, new_seen = false;
            while (end - info_offset >= sizeof(fanotify_event_info_header)) {
                fanotify_event_info_header header_value{};
                std::memcpy(&header_value, buffer + info_offset, sizeof(header_value));
                const auto* header = &header_value;
                if (header->len < sizeof(*header) || header->len > end - info_offset) {
                    error = "invalid fanotify information length"; return false;
                }
                const bool target = header->info_type == FAN_EVENT_INFO_TYPE_FID;
                const bool named = header->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
                    header->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME ||
                    header->info_type == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME;
                if (target || named) {
                    const std::size_t handle_offset = info_offset + offsetof(fanotify_event_info_fid, handle);
                    if (handle_offset + sizeof(file_handle) > info_offset + header->len) {
                        error = "truncated fanotify target handle"; return false;
                    }
                    file_handle handle_value{};
                    std::memcpy(&handle_value, buffer + handle_offset, sizeof(file_handle));
                    const auto* handle = &handle_value;
                    if (!handle->handle_bytes || handle->handle_bytes > info_offset + header->len - handle_offset - sizeof(file_handle)) {
                        error = "invalid fanotify target handle length"; return false;
                    }
                    fanotify_event_info_fid fid{};
                    std::memcpy(&fid, buffer + info_offset, offsetof(fanotify_event_info_fid, handle));
                    const auto* handle_bytes = buffer + handle_offset + sizeof(file_handle);
                    if (target) {
                        if (target_seen) { error = "duplicate fanotify target handle"; return false; }
                        target_seen = true;
                        event.object_handle = Key(&fid.fsid, handle->handle_type, handle_bytes, handle->handle_bytes);
                        info_offset += header->len;
                        continue;
                    }
                    const std::size_t name_offset = handle_offset + sizeof(file_handle) + handle->handle_bytes;
                    if (name_offset >= info_offset + header->len) { error = "truncated fanotify name"; return false; }
                    const char* name = reinterpret_cast<const char*>(buffer + name_offset);
                    const std::size_t available = info_offset + header->len - name_offset;
                    const void* terminator = std::memchr(name, 0, available);
                    if (!terminator) { error = "unterminated fanotify name"; return false; }
                    const auto name_size = static_cast<const char*>(terminator) - name;
                    if (!name_size || name_size > 255 || std::memchr(name, '/', name_size)) {
                        error = "invalid fanotify entry name"; return false;
                    }
                    if (header->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME) {
                        if (old_seen) { error = "duplicate fanotify old parent"; return false; }
                        old_seen = true;
                        event.old_directory_handle = Key(&fid.fsid, handle->handle_type, handle_bytes, handle->handle_bytes);
                        event.old_name.assign(name, name_size);
                    } else {
                        if (new_seen) { error = "duplicate fanotify parent"; return false; }
                        new_seen = true;
                        event.directory_handle = Key(&fid.fsid, handle->handle_type, handle_bytes, handle->handle_bytes);
                        event.name.assign(name, name_size);
                    }
                }
                info_offset += header->len;
            }
            if (info_offset != end) { error = "trailing fanotify information bytes"; return false; }
            decoded.push_back(std::move(event));
            offset = end;
        }
        if (offset != size) { error = "trailing fanotify bytes"; return false; }
        events.insert(events.end(), std::make_move_iterator(decoded.begin()), std::make_move_iterator(decoded.end()));
        error.clear();
        return true;
}
bool LinuxFanotify::Drain(std::vector<LinuxNotification>& events, std::string& error) {
    alignas(fanotify_event_metadata) unsigned char buffer[65536];
    for (unsigned batch = 0; batch < 16; ++batch) {
        const ssize_t count = read(fd_, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            error = std::strerror(errno); return false;
        }
        if (!count) { error = "fanotify stream closed"; return false; }
        if (!DecodeBuffer(buffer, static_cast<std::size_t>(count), events, error)) return false;
    }
    return true;
}
void LinuxNotificationBuffer::Push(LinuxNotification event) {
    if (overflow_) return;
    if (event.overflow) {
        const bool local = !event.overflow || event.userspace_overflow;
        pending_.clear(); metadata_.clear(); overflow_=true;
        LinuxNotification marker;marker.overflow=true;marker.userspace_overflow=local;
        pending_.push_back(std::move(marker));return;
    }
    constexpr std::uint64_t metadata_mask=FAN_MODIFY|FAN_CLOSE_WRITE|FAN_ATTRIB;
    if ((event.mask&metadata_mask) && !(event.mask&~(metadata_mask|FAN_ONDIR))) {
        const auto key=std::make_tuple(event.directory_handle,event.name,event.object_handle);
        auto found=metadata_.find(key);
        if(found!=metadata_.end()) {pending_[found->second].mask|=event.mask;return;}
        metadata_[key]=pending_.size();
    } else metadata_.clear(); // Never merge metadata across rename/create/delete.
    if (pending_.size() >= limit_) {
        LinuxNotification marker;marker.overflow=true;marker.userspace_overflow=true;
        Push(std::move(marker));return;
    }
    pending_.push_back(std::move(event));
}
std::vector<LinuxNotification> LinuxNotificationBuffer::Take() {
    std::vector<LinuxNotification> result;
    pending_.swap(result);metadata_.clear();overflow_=false;return result;
}
void LinuxFanotify::Collect() {
    try {
        while (!stopping_) {
            pollfd notification{fd_,POLLIN,0};
            const int result=poll(&notification,1,20);
            if(result<0) {if(errno==EINTR)continue;throw std::runtime_error(std::strerror(errno));}
            if(!result)continue;
            if(notification.revents&(POLLERR|POLLHUP|POLLNVAL))throw std::runtime_error("fanotify descriptor unavailable");
            std::vector<LinuxNotification> batch;std::string error;
            if(!Drain(batch,error))throw std::runtime_error(error);
            std::lock_guard lock(mutex_);
            for(auto& event:batch) {
                if(!event.overflow && ignored_process_>0 && event.process_id==ignored_process_)continue;
                pending_.Push(std::move(event));
            }
        }
    } catch(const std::exception& failure) { std::lock_guard lock(mutex_);reader_error_=failure.what(); }
}
bool LinuxFanotify::Read(std::vector<LinuxNotification>& events,std::string& error) {
    std::lock_guard lock(mutex_);
    if(!reader_error_.empty()){error=reader_error_;return false;}
    auto batch=pending_.Take();
    events.insert(events.end(),std::make_move_iterator(batch.begin()),std::make_move_iterator(batch.end()));
    error.clear();return true;
}

}

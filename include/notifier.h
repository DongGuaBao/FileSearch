#pragma once

#include "types.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fsengine {

enum class FsEventType {
    FileCreated,
    FileDeleted,
    FileModified,
    FileRenamed,
    DirCreated,
    DirDeleted,
    DirRenamed,
};

struct FsEvent {
    FsEventType type = FsEventType::FileModified;
    Frn frn = 0;
    Frn parent_frn = 0;
    std::wstring name;
};

struct ChangePosition {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
};

inline bool operator==(const ChangePosition& left, const ChangePosition& right) {
    return left.epoch == right.epoch && left.sequence == right.sequence;
}

inline bool operator!=(const ChangePosition& left, const ChangePosition& right) {
    return !(left == right);
}

struct FileChangeEvent {
    ChangePosition position;
    FsEventType type = FsEventType::FileModified;
    Frn frn = 0;
    Frn parent_frn = 0;
    Frn old_parent_frn = 0;
    std::wstring name;
    std::wstring old_name;
    std::wstring path;
    std::wstring old_path;
#if !defined(_WIN32)
    // Linux filenames are byte strings, not necessarily UTF-8. Display fields
    // may contain replacement characters; these fields retain exact identity.
    std::string path_bytes;
    std::string old_path_bytes;
    std::string name_bytes;
    std::string old_name_bytes;
#endif
    bool is_directory = false;
};

struct FileChangePullResult {
    bool expired = false;
    ChangePosition position;
    std::vector<FileChangeEvent> events;
};

// A bounded, process-local multi-reader event stream. Readers own independent
// cursors, so a slow subscriber never drains events needed by another one.
class FSENGINE_API ChangeEventRing {
public:
    explicit ChangeEventRing(std::size_t capacity = 65536);

    ChangePosition CurrentPosition() const;
    ChangePosition Push(FileChangeEvent event);
    FileChangePullResult PullSince(ChangePosition cursor,
                                   std::size_t max_events = 1024) const;
    ChangePosition WaitForChange(ChangePosition cursor,
                                 std::uint32_t timeout_ms) const;
    void Reset();

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<FileChangeEvent> events_;
    std::size_t capacity_ = 0;
    std::uint64_t epoch_ = 1;
    std::uint64_t next_sequence_ = 1;
};

class FSENGINE_API Notifier {
public:
    void Push(FsEvent event);
    std::vector<FsEvent> Drain();
    void Flush();

private:
    std::mutex mutex_;
    std::vector<FsEvent> pending_;
};

FSENGINE_API const wchar_t* FsEventTypeName(FsEventType type);

}  // namespace fsengine

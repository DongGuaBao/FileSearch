#include "notifier.h"

#include <algorithm>
#include <chrono>
#include <random>

namespace fsengine {

ChangeEventRing::ChangeEventRing(std::size_t capacity)
    : capacity_((std::max<std::size_t>)(capacity, 1)) {
    // Process-local history must never accept a cursor from a prior instance.
    std::random_device random;
    epoch_ = (static_cast<std::uint64_t>(random()) << 32) ^ random() ^
             static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    if (epoch_ == 0) epoch_ = 1;
}

ChangePosition ChangeEventRing::CurrentPosition() const {
    std::lock_guard lock(mutex_);
    return ChangePosition{epoch_, next_sequence_ - 1};
}

ChangePosition ChangeEventRing::Push(FileChangeEvent event) {
    std::lock_guard lock(mutex_);
    event.position = ChangePosition{epoch_, next_sequence_++};
    const ChangePosition position = event.position;
    events_.push_back(std::move(event));
    while (events_.size() > capacity_) {
        events_.pop_front();
    }
    changed_.notify_all();
    return position;
}

FileChangePullResult ChangeEventRing::PullSince(ChangePosition cursor,
                                                std::size_t max_events) const {
    std::lock_guard lock(mutex_);
    FileChangePullResult result;
    result.position = ChangePosition{epoch_, next_sequence_ - 1};
    if (cursor.epoch != epoch_ || cursor.sequence >= next_sequence_) {
        result.expired = true;
        return result;
    }
    if (!events_.empty() && cursor.sequence < events_.front().position.sequence - 1) {
        result.expired = true;
        return result;
    }
    if (max_events == 0) {
        result.position = cursor;
        return result;
    }
    result.events.reserve((std::min)(max_events, events_.size()));
    for (const FileChangeEvent& event : events_) {
        if (event.position.sequence <= cursor.sequence) {
            continue;
        }
        if (result.events.size() >= max_events) {
            break;
        }
        result.events.push_back(event);
        result.position = event.position;
    }
    if (result.events.empty()) {
        result.position = ChangePosition{epoch_, next_sequence_ - 1};
    }
    return result;
}

ChangePosition ChangeEventRing::WaitForChange(ChangePosition cursor,
                                              std::uint32_t timeout_ms) const {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return cursor.epoch != epoch_ || cursor.sequence != next_sequence_ - 1;
    });
    return ChangePosition{epoch_, next_sequence_ - 1};
}

void ChangeEventRing::Reset() {
    std::lock_guard lock(mutex_);
    events_.clear();
    ++epoch_;
    if (epoch_ == 0) {
        epoch_ = 1;
    }
    next_sequence_ = 1;
    changed_.notify_all();
}

void Notifier::Push(FsEvent event) {
    std::lock_guard lock(mutex_);
    pending_.push_back(std::move(event));
}

std::vector<FsEvent> Notifier::Drain() {
    std::lock_guard lock(mutex_);
    std::vector<FsEvent> out;
    out.swap(pending_);
    return out;
}

void Notifier::Flush() {
    std::lock_guard lock(mutex_);
    pending_.clear();
}

const wchar_t* FsEventTypeName(FsEventType type) {
    switch (type) {
    case FsEventType::FileCreated:
        return L"FILE_CREATED";
    case FsEventType::FileDeleted:
        return L"FILE_DELETED";
    case FsEventType::FileModified:
        return L"FILE_MODIFIED";
    case FsEventType::FileRenamed:
        return L"FILE_RENAMED";
    case FsEventType::DirCreated:
        return L"DIR_CREATED";
    case FsEventType::DirDeleted:
        return L"DIR_DELETED";
    case FsEventType::DirRenamed:
        return L"DIR_RENAMED";
    default:
        return L"UNKNOWN";
    }
}

}  // namespace fsengine

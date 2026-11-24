#include "mft_service.h"
#include <chrono>
#include <future>
#include <stdexcept>

namespace fsengine {
struct MftChangeHealthTestAccess {
    static void Fail(MftService& service) { service.InvalidateChangeStream(); }
    static void Push(MftService& service) { service.change_events_.Push(FileChangeEvent{}); }
    static bool AppendWithoutWal(MftService& service) {
        std::lock_guard lock(service.change_snapshot_mutex_);
        return service.AppendStreamWalChangeLocked(IndexChange{});
    }
};
}
int main() {
    using namespace fsengine;
    auto require = [](bool value) { if (!value) throw std::runtime_error("change health regression"); };
    MftService service; // No configured volumes, journal operations or cache IO.
    auto before = service.CurrentChangePosition();
    MftChangeHealthTestAccess::Push(service);
    require(service.PullFileChanges(before).events.size() == 1);
    before = service.CurrentChangePosition();
    auto waiter = std::async(std::launch::async, [&] { return service.WaitFileChanges(before, 10000); });
    MftChangeHealthTestAccess::Fail(service);
    require(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto failed = waiter.get();
    require(failed.epoch != before.epoch);
    require(service.PullFileChanges(before).expired);
    require(service.PullFileChanges(failed).expired); // A fresh cursor cannot bypass unhealthy state.
    MftSnapshot snapshot;
    ChangePosition position;
    std::wstring error;
    require(!service.CaptureChangeSnapshot(&snapshot, &position, &error));
    require(error.find(L"continuity") != std::wstring::npos);
    MftChangeHealthTestAccess::Fail(service);
    require(service.CurrentChangePosition() == failed); // One transition, not an epoch storm.
    MftService wal_failure;
    const auto wal_before = wal_failure.CurrentChangePosition();
    require(!MftChangeHealthTestAccess::AppendWithoutWal(wal_failure));
    require(wal_failure.CurrentChangePosition().epoch != wal_before.epoch);
    require(wal_failure.PullFileChanges(wal_failure.CurrentChangePosition()).expired);
    require(!wal_failure.CaptureChangeSnapshot(&snapshot, &position, &error));
}

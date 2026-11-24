#include "notifier.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

fsengine::FileChangeEvent Event(fsengine::Frn id) {
    fsengine::FileChangeEvent event;
    event.type = fsengine::FsEventType::FileCreated;
    event.frn = id;
    event.name = L"test";
    return event;
}

}  // namespace

int main() {
    fsengine::ChangeEventRing ring(3);
    const fsengine::ChangePosition start = ring.CurrentPosition();
    Require(start.epoch != 0 && start.sequence == 0, "invalid initial cursor");

    ring.Push(Event(1));
    ring.Push(Event(2));
    auto zero = ring.PullSince(start, 0);
    Require(zero.events.empty() && zero.position == start, "zero-size pull advanced cursor");
    auto future = ring.PullSince({start.epoch, 100}, 8);
    Require(future.expired, "future cursor was accepted");
    auto maximum = ring.PullSince({start.epoch, UINT64_MAX}, 8);
    Require(maximum.expired, "maximum cursor was accepted");
    fsengine::ChangeEventRing another;
    Require(another.CurrentPosition().epoch != start.epoch, "separate streams share an epoch");
    auto first = ring.PullSince(start, 1);
    Require(!first.expired, "fresh cursor expired");
    Require(first.events.size() == 1 && first.events[0].frn == 1, "bounded pull returned wrong event");
    auto second = ring.PullSince(first.position, 8);
    Require(second.events.size() == 1 && second.events[0].frn == 2, "second pull returned wrong event");

    ring.Push(Event(3));
    ring.Push(Event(4));
    auto expired = ring.PullSince(start, 8);
    Require(expired.expired, "overwritten cursor was not expired");

    const fsengine::ChangePosition wait_cursor = ring.CurrentPosition();
    std::thread producer([&ring]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        ring.Push(Event(5));
    });
    const fsengine::ChangePosition waited = ring.WaitForChange(wait_cursor, 1000);
    producer.join();
    Require(waited.epoch == wait_cursor.epoch && waited.sequence == wait_cursor.sequence + 1,
            "wait did not observe the next event");

    ring.Reset();
    auto reset = ring.PullSince(waited, 8);
    Require(reset.expired && reset.position.epoch != waited.epoch, "reset did not expire old epoch");

    std::cout << "change_event_ring_test passed\n";
    return 0;
}

#include "linux_fanotify.h"
#include <sys/fanotify.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using Bytes = std::vector<unsigned char>;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> static void Set(Bytes& bytes, std::size_t offset, const T& value) {
    Require(offset + sizeof(T) <= bytes.size(), "test fixture write out of range");
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
static Bytes Info(unsigned char type, const std::string& name = {}) {
    const bool target = type == FAN_EVENT_INFO_TYPE_FID;
    const auto length = offsetof(fanotify_event_info_fid, handle) + sizeof(file_handle) + 12 + (target ? 0 : name.size() + 1);
    Bytes bytes((length + 7) & ~std::size_t(7));
    fanotify_event_info_header header{};
    header.info_type = type; header.len = static_cast<unsigned short>(bytes.size());
    Set(bytes, 0, header);
    file_handle handle{}; handle.handle_bytes = 12; handle.handle_type = 1;
    Set(bytes, offsetof(fanotify_event_info_fid, handle), handle);
    const auto data = offsetof(fanotify_event_info_fid, handle) + sizeof(file_handle);
    for (std::size_t i = 0; i < 12; ++i) bytes[data + i] = static_cast<unsigned char>(i + 1);
    if (!target) std::memcpy(bytes.data() + data + 12, name.c_str(), name.size() + 1);
    return bytes;
}
static Bytes Event(std::uint64_t mask, const std::vector<Bytes>& information) {
    Bytes bytes(sizeof(fanotify_event_metadata));
    for (const auto& info : information) bytes.insert(bytes.end(), info.begin(), info.end());
    fanotify_event_metadata metadata{};
    metadata.event_len = static_cast<unsigned>(bytes.size());
    metadata.metadata_len = sizeof(metadata); metadata.vers = FANOTIFY_METADATA_VERSION;
    metadata.mask = mask; metadata.fd = FAN_NOFD;
    Set(bytes, 0, metadata);
    return bytes;
}
static bool Decode(const Bytes& bytes, std::vector<fsengine::LinuxNotification>& events) {
    std::string error;
    return fsengine::LinuxFanotify::DecodeBuffer(bytes.data(), bytes.size(), events, error);
}
static void Reject(const Bytes& bytes) {
    std::vector<fsengine::LinuxNotification> events(1);
    events[0].name = "sentinel";
    std::string error;
    Require(!fsengine::LinuxFanotify::DecodeBuffer(bytes.data(), bytes.size(), events, error), "malformed buffer accepted");
    Require(!error.empty() && events.size() == 1 && events[0].name == "sentinel", "partial batch committed on error");
}
int main() {
    const auto named = Info(FAN_EVENT_INFO_TYPE_DFID_NAME, std::string("raw_") + char(255));
    const auto target = Info(FAN_EVENT_INFO_TYPE_FID);
    const auto good = Event(FAN_CREATE, {named, target});
    std::vector<fsengine::LinuxNotification> events;
    Require(Decode(good, events) && events.size() == 1, "valid create rejected");
    Require(events[0].name == std::string("raw_") + char(255) && !events[0].object_handle.empty(), "raw name or generation lost");
    const auto rename = Event(FAN_RENAME, {Info(FAN_EVENT_INFO_TYPE_OLD_DFID_NAME, "old"),
        Info(FAN_EVENT_INFO_TYPE_NEW_DFID_NAME, "new"), target});
    events.clear();
    Require(Decode(rename, events) && events[0].old_name == "old" && events[0].name == "new", "rename endpoints lost");
    events.clear();
    Require(Decode(Event(FAN_Q_OVERFLOW, {}), events) && events[0].overflow, "overflow rejected");
    // Forward-compatible unknown info with an odd length deliberately makes
    // the next known header unaligned. The production decoder must use copies.
    Bytes unknown(5);
    fanotify_event_info_header extension{}; extension.info_type = 250; extension.len = 5;
    Set(unknown, 0, extension);
    events.clear();
    Require(Decode(Event(FAN_CREATE, {unknown, named, target}), events), "unaligned extension rejected");
    for (std::size_t i = 0; i < good.size(); ++i) Reject(Bytes(good.begin(), good.begin() + i));
    auto bad = good;
    Set(bad, offsetof(fanotify_event_metadata, event_len), unsigned(0)); Reject(bad);
    bad = good; Set(bad, offsetof(fanotify_event_metadata, event_len), unsigned(good.size() + 1)); Reject(bad);
    bad = good; Set(bad, offsetof(fanotify_event_metadata, metadata_len), static_cast<unsigned short>(sizeof(fanotify_event_metadata) - 1)); Reject(bad);
    bad = good; Set(bad, offsetof(fanotify_event_metadata, vers), static_cast<unsigned char>(0)); Reject(bad);
    bad = good; Set(bad, offsetof(fanotify_event_metadata, fd), int(1)); Reject(bad); // must not close stdout
    const auto info_start = sizeof(fanotify_event_metadata);
    for (unsigned short length : {0, 2, 65535}) {
        bad = good; Set(bad, info_start + offsetof(fanotify_event_info_header, len), length); Reject(bad);
    }
    for (unsigned length : {0u, std::numeric_limits<unsigned>::max()}) {
        bad = good; Set(bad, info_start + offsetof(fanotify_event_info_fid, handle), length); Reject(bad);
    }
    auto unterminated = named;
    const auto name_start = offsetof(fanotify_event_info_fid, handle) + sizeof(file_handle) + 12;
    std::fill(unterminated.begin() + name_start, unterminated.end(), 'x');
    Reject(Event(FAN_CREATE, {unterminated, target}));
    Reject(Event(FAN_CREATE, {Info(FAN_EVENT_INFO_TYPE_DFID_NAME, "bad/name"), target}));
    Reject(Event(FAN_CREATE, {named, target, target}));
    Reject(Event(FAN_CREATE, {named, named, target}));
    bad = good; bad.push_back(1); Reject(bad);
    bad = good; bad.push_back(1); Set(bad, 0, static_cast<unsigned>(bad.size())); Reject(bad);
    bad = good; bad.insert(bad.end(), good.begin(), good.end() - 1); Reject(bad);
    // Deterministic malformed-buffer sweep, including every byte/bit mutation
    // of a real-shaped batch. ASan/UBSan exercises exactly the production path.
    for (std::size_t i = 0; i < good.size(); ++i) for (unsigned bit = 0; bit < 8; ++bit) {
        bad = good; bad[i] ^= static_cast<unsigned char>(1u << bit);
        events.clear(); Decode(bad, events);
    }
    std::uint32_t random = 0xabc123u;
    for (unsigned i = 0; i < 20000; ++i) {
        random = random * 1664525u + 1013904223u;
        bad.resize(random % 512);
        for (auto& byte : bad) { random = random * 1664525u + 1013904223u; byte = static_cast<unsigned char>(random >> 24); }
        events.clear(); Decode(bad, events);
    }
    std::cout << "linux_fanotify_decode_test passed\n";
}

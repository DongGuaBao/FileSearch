#include "linux_catalog.h"
#include "json.hpp"
#include "utf8.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <set>
#include <poll.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using Json = nlohmann::json;
namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};
std::atomic<unsigned> snapshot_clients{0};
void Signal(int) { stopping = 1; }
bool Transfer(int fd, void* bytes, std::size_t size, bool write) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (size && !stopping) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        const ssize_t count = write ? send(fd, cursor, size, MSG_NOSIGNAL | MSG_DONTWAIT) : recv(fd, cursor, size, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd peer{fd, static_cast<short>(write ? POLLOUT : POLLIN), 0};
            if (poll(&peer, 1, static_cast<int>(remaining)) < 0 && errno == EINTR) continue;
            if (!peer.revents) return false;
            continue;
        }
        if (count <= 0) return false;
        cursor += count; size -= static_cast<std::size_t>(count);
    }
    return size == 0;
}
bool Send(int fd, const Json& json) {
    std::string body = json.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (body.size() > 1024 * 1024) return false;
    std::uint32_t size = static_cast<std::uint32_t>(body.size());
    return Transfer(fd, &size, sizeof(size), true) && Transfer(fd, body.data(), body.size(), true);
}
Json Receive(int fd) {
    std::uint32_t size = 0;
    if (!Transfer(fd, &size, sizeof(size), false) || size == 0 || size > 1024 * 1024)
        throw std::runtime_error("invalid request frame");
    std::string body(size, '\0');
    if (!Transfer(fd, body.data(), body.size(), false)) throw std::runtime_error("incomplete request");
    return Json::parse(body);
}
int Listen(const std::string& path) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("socket path too long");
    struct stat existing{};
    if (lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid())
            throw std::runtime_error("refusing to replace non-service socket: " + path);
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe < 0) throw std::runtime_error(std::strerror(errno));
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const int connected = connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        const int failure = errno;
        close(probe);
        if (connected == 0 || failure != ECONNREFUSED)
            throw std::runtime_error("socket is active or cannot be safely reclaimed: " + path);
        if (unlink(path.c_str()) < 0) throw std::runtime_error(std::strerror(errno));
    } else if (errno != ENOENT) throw std::runtime_error(std::strerror(errno));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || chmod(path.c_str(), 0600) < 0 || listen(fd, 32) < 0) {
        const std::string error = std::strerror(errno); close(fd); throw std::runtime_error(error);
    }
    return fd;
}
Json Position(fsengine::ChangePosition position) { return {{"epoch", position.epoch}, {"sequence", position.sequence}}; }
std::string Hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}
bool WithinScope(const std::string& path, const std::string& scope) {
    return scope.empty() || scope == "/" || path == scope ||
        (path.size() > scope.size() && path.compare(0, scope.size(), scope) == 0 && path[scope.size()] == '/');
}
std::string RequestScope(const Json& request) {
    if (request.contains("scope_path") && request.contains("scope_path_bytes_hex"))
        throw std::runtime_error("specify only one scope encoding");
    auto scope = request.value("scope_path", std::string{});
    if (request.contains("scope_path_bytes_hex")) {
        const auto encoded = request.at("scope_path_bytes_hex").get<std::string>();
        if (encoded.size() % 2 || encoded.size() > 8192) throw std::runtime_error("invalid scope hex length");
        auto digit = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            throw std::runtime_error("invalid scope hex digit");
        };
        scope.clear(); scope.reserve(encoded.size() / 2);
        for (std::size_t i = 0; i < encoded.size(); i += 2)
            scope.push_back(static_cast<char>((digit(encoded[i]) << 4) | digit(encoded[i+1])));
    }
    if (scope.size() > 4096 || scope.find('\0') != std::string::npos) throw std::runtime_error("invalid scope path");
    if (!scope.empty() && !std::filesystem::path(scope).is_absolute()) throw std::runtime_error("scope_path must be absolute");
    auto normalized = std::filesystem::path(scope).lexically_normal().string();
    while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
    return normalized;
}
Json EntryJson(const fsengine::LinuxScanEntry& entry) {
    return {{"name", fsengine::WideToUtf8(fsengine::Utf8ToWide(entry.name))},
        {"path", fsengine::WideToUtf8(fsengine::Utf8ToWide(entry.path))},
        {"name_bytes_hex", Hex(entry.name)}, {"path_bytes_hex", Hex(entry.path)},
        {"id", entry.id}, {"is_directory", entry.directory}, {"is_symlink", entry.symlink}};
}
std::size_t PageSize(const Json& request) {
    if (!request.contains("page_size")) return 256;
    const auto& value = request.at("page_size");
    if (!value.is_number_integer() || value < 1 || value > 1024)
        throw std::runtime_error("page_size must be an integer from 1 to 1024");
    return value.get<std::size_t>();
}
void SnapshotClient(int fd, const Json& request, fsengine::LinuxCatalog& catalog) {
    const unsigned previous = snapshot_clients.fetch_add(1);
    struct CountGuard { ~CountGuard() { --snapshot_clients; } } count_guard;
    if (previous >= 4) {
        Send(fd, {{"ok", false}, {"code", "snapshot_busy"}, {"error", "four snapshots already active"}}); return;
    }
    if (request.value("version", 1) != 1) throw std::runtime_error("unsupported snapshot version");
    if (request.contains("actions")) throw std::runtime_error("snapshot does not support action filters");
    auto page_size = PageSize(request);
    fsengine::ChangePosition position;
    auto snapshot = catalog.Snapshot(RequestScope(request), request.value("include_files", true),
        request.value("include_directories", true), position);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::uint64_t page_index = 0;
    while (!stopping) {
        if (!catalog.CanResume(position)) {
            Send(fd, {{"ok", false}, {"type", "resync_required"}, {"position", Position(catalog.Position())}}); return;
        }
        auto page = snapshot->NextPage(page_size);
        if (std::chrono::steady_clock::now() >= deadline) {
            Send(fd, {{"ok", false}, {"code", "snapshot_expired"}, {"error", "snapshot exceeded 30 seconds"}}); return;
        }
        if (!catalog.CanResume(position)) {
            Send(fd, {{"ok", false}, {"type", "resync_required"}, {"position", Position(catalog.Position())}}); return;
        }
        Json entries = Json::array();
        for (const auto& entry : page.entries) entries.push_back(EntryJson(entry));
        if (!Send(fd, {{"ok", true}, {"type", "snapshot_page"}, {"version", 1},
            {"position", Position(position)}, {"page_index", page_index++},
            {"count", entries.size()}, {"entries", std::move(entries)}, {"done", page.done}})) return;
        if (page.done) return;
        const auto next = Receive(fd);
        if (next.value("command", "") != "snapshot_next") throw std::runtime_error("expected snapshot_next");
        for (const auto& item : next.items())
            if (item.key() != "command" && item.key() != "page_size") throw std::runtime_error("snapshot filters cannot change between pages");
        if (next.contains("page_size")) page_size = PageSize(next);
    }
}
void Client(int fd, bool subscription, fsengine::LinuxCatalog& catalog, std::atomic<unsigned>& clients) {
    struct Guard { int fd; std::atomic<unsigned>& count; ~Guard() { close(fd); --count; } } guard{fd, clients};
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    try {
        const Json request = Receive(fd);
        if (!subscription) {
            const std::string command = request.value("command", "search");
            if (command == "status") { Send(fd, {{"ok", true}, {"ready", catalog.Ready()},
                {"search_ready", catalog.SearchReady()}, {"validating", !catalog.Ready()},
                {"scan_retries", catalog.ScanRetryCount()},
                {"fanotify_overflows", catalog.OverflowCount()}, {"recovered_database", catalog.RecoveredDatabase()},
                {"position", Position(catalog.Position())}}); return; }
            if (command == "snapshot") { SnapshotClient(fd, request, catalog); return; }
            if (command != "search") throw std::runtime_error("unknown command");
            const std::string query = request.value("query", "");
            if (query.empty()) throw std::runtime_error("query is required");
            if (query.size() > 1024) throw std::runtime_error("query exceeds 1024 bytes");
            const auto phase_name = request.value("phase", "exact_only");
            auto phase = fsengine::LinuxSearchPhase::ExactOnly;
            if (phase_name == "fuzzy_only") phase = fsengine::LinuxSearchPhase::FuzzyOnly;
            else if (phase_name == "exact_then_fuzzy") phase = fsengine::LinuxSearchPhase::ExactThenFuzzy;
            else if (phase_name != "exact_only") throw std::runtime_error("unknown search phase");
            bool stale = false;
            const auto entries = catalog.Search(query, RequestScope(request),
                std::min<std::size_t>(request.value("max_results", std::size_t{50}), 10000), phase, &stale);
            Json results = Json::array();
            for (const auto& entry : entries) results.push_back(EntryJson(entry));
            Send(fd, {{"ok", true}, {"stale", stale}, {"count", results.size()}, {"results", std::move(results)}});
            return;
        }
        if (request.value("command", "") != "subscribe_changes" || request.value("version", 1) != 1)
            throw std::runtime_error("invalid subscription");
        const auto scope = RequestScope(request);
        const bool include_files = request.value("include_files", true);
        const bool include_directories = request.value("include_directories", true);
        std::set<std::string> actions;
        if (request.contains("actions")) {
            for (const auto& action : request.at("actions").get<std::vector<std::string>>()) {
                if (action != "create" && action != "delete" && action != "modify" && action != "rename")
                    throw std::runtime_error("unknown subscription action");
                actions.insert(action);
            }
        }
        auto cursor = catalog.Position();
        if (request.contains("since")) cursor = {request.at("since").at("epoch").get<std::uint64_t>(), request.at("since").at("sequence").get<std::uint64_t>()};
        if (!Send(fd, {{"type", "subscribed"}, {"position", Position(cursor)}})) return;
        while (!stopping) {
            const auto changes = catalog.Changes(cursor);
            if (changes.expired || !catalog.Ready()) { Send(fd, {{"type", "resync_required"}, {"position", Position(changes.position)}}); return; }
            for (const auto& event : changes.events) {
                const char* action = "modify";
                switch (event.type) {
                case fsengine::FsEventType::FileCreated: case fsengine::FsEventType::DirCreated: action = "create"; break;
                case fsengine::FsEventType::FileDeleted: case fsengine::FsEventType::DirDeleted: action = "delete"; break;
                case fsengine::FsEventType::FileRenamed: case fsengine::FsEventType::DirRenamed: action = "rename"; break;
                default: break;
                }
                if ((!include_directories && event.is_directory) || (!include_files && !event.is_directory) ||
                    (!actions.empty() && !actions.count(action)) ||
                    (!WithinScope(event.path_bytes, scope) && (event.old_path_bytes.empty() || !WithinScope(event.old_path_bytes, scope)))) continue;
                if (!Send(fd, {{"type", "change"}, {"action", action}, {"position", Position(event.position)},
                    {"name", fsengine::WideToUtf8(event.name)}, {"path", fsengine::WideToUtf8(event.path)},
                    {"old_name", fsengine::WideToUtf8(event.old_name)}, {"old_path", fsengine::WideToUtf8(event.old_path)},
                    {"name_bytes_hex", Hex(event.name_bytes)}, {"path_bytes_hex", Hex(event.path_bytes)},
                    {"old_name_bytes_hex", Hex(event.old_name_bytes)}, {"old_path_bytes_hex", Hex(event.old_path_bytes)},
                    {"id", event.frn},
                    {"is_directory", event.is_directory}})) return;
            }
            cursor = changes.position;
            if (changes.events.empty()) catalog.WaitForChanges(cursor, 100);
            pollfd peer{fd, POLLIN | POLLHUP, 0};
            if (poll(&peer, 1, 0) > 0) return;
        }
    } catch (const fsengine::LinuxCatalogNotReady& error) {
        Send(fd, {{"ok", false}, {"code", "not_ready"}, {"error", error.what()}});
    } catch (const std::exception& error) { Send(fd, {{"ok", false}, {"error", error.what()}}); }
}
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: FileSearchServiceLinux ROOT SOCKET_DIRECTORY\n"; return 2; }
    signal(SIGTERM, Signal); signal(SIGINT, Signal);
    umask(0077);
    int search = -1, events = -1, instance_lock = -1;
    std::string search_path, events_path;
    std::atomic<unsigned> clients{0};
    struct Worker { std::thread thread; std::shared_ptr<std::atomic<bool>> finished; };
    std::vector<Worker> workers;
    std::thread collector;
    std::string collector_error;
    fsengine::LinuxCatalog catalog;
    int result = 0;
    try {
        workers.reserve(32);
        std::string error;
        const std::filesystem::path socket_directory(argv[2]);
        if (!socket_directory.is_absolute()) throw std::runtime_error("socket directory must be absolute");
        std::filesystem::create_directories(socket_directory);
        struct stat directory{};
        if (lstat(argv[2], &directory) < 0 || !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() || (directory.st_mode & 0022))
            throw std::runtime_error("socket directory must be owned by service user and not writable by others");
        const auto lock_path = (socket_directory / "service.lock").string();
        instance_lock = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat lock_state{};
        if (instance_lock < 0 || fstat(instance_lock, &lock_state) < 0 || !S_ISREG(lock_state.st_mode) ||
            lock_state.st_uid != geteuid() || lock_state.st_nlink != 1 || (lock_state.st_mode & 0022))
            throw std::runtime_error("invalid service lock");
        if (flock(instance_lock, LOCK_EX | LOCK_NB) < 0)
            throw std::runtime_error("another service owns this IPC directory");
        const auto database = (socket_directory / "catalog.db").string();
        struct stat database_state{};
        if (lstat(database.c_str(), &database_state) == 0 &&
            (!S_ISREG(database_state.st_mode) || database_state.st_uid != geteuid() || database_state.st_nlink != 1))
            throw std::runtime_error("invalid catalog database file");
        if (!catalog.Start(argv[1], error, database, true)) throw std::runtime_error(error);
        search_path = (socket_directory / "search.sock").string(); events_path = (socket_directory / "events.sock").string();
        search = Listen(search_path); events = Listen(events_path);
        // Same service process, separate worker: scans must not block accepting
        // search/status clients. Only this worker owns fanotify and calls Pump.
        collector = std::thread([&]() {
            try {
                while (!stopping) {
                    if (!catalog.Pump(collector_error)) { stopping = true; return; }
                    pollfd notification{catalog.EventDescriptor(), POLLIN, 0};
                    if (poll(&notification, 1, 20) < 0 && errno != EINTR)
                        throw std::runtime_error(std::strerror(errno));
                }
            } catch (const std::exception& failure) { collector_error = failure.what(); stopping = true; }
        });
        while (!stopping) {
            for (auto worker = workers.begin(); worker != workers.end();) {
                if (worker->finished->load()) { worker->thread.join(); worker = workers.erase(worker); }
                else ++worker;
            }
            pollfd listeners[]{{search, POLLIN, 0}, {events, POLLIN, 0}};
            if (poll(listeners, 2, 100) < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
            for (int i = 0; i < 2; ++i) {
                if (!(listeners[i].revents & POLLIN)) continue;
                int fd = accept4(listeners[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) continue;
                if (clients >= 32) { close(fd); continue; }
                ++clients;
                auto finished = std::make_shared<std::atomic<bool>>(false);
                try {
                    workers.push_back({std::thread([&, fd, subscription = i == 1, finished]() {
                        Client(fd, subscription, catalog, clients);
                        finished->store(true);
                    }), finished});
                } catch (...) { close(fd); --clients; throw; }
            }
        }
        collector.join();
        if (!collector_error.empty()) throw std::runtime_error(collector_error);
        for (auto& worker : workers) worker.thread.join();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1; stopping = 1;
        if (collector.joinable()) collector.join();
        for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
    }
    if (search >= 0) { close(search); unlink(search_path.c_str()); }
    if (events >= 0) { close(events); unlink(events_path.c_str()); }
    if (instance_lock >= 0) close(instance_lock);
    return result;
}

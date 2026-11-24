#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <sddl.h>
#include <winsvc.h>

#include "auto_file_search.h"
#include "json.hpp"
#include "logger.h"
#include "utf8.h"
#include "windows_snapshot_view.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cwctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace {

constexpr wchar_t kServiceName[] = L"FileSearch";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FileSearch";
constexpr wchar_t kEventsPipeName[] = L"\\\\.\\pipe\\FileSearch.Events";
constexpr DWORD kPipeBufSize = 65536;
constexpr DWORD kPipeIoTimeoutMs = 30000;
constexpr std::size_t kDefaultMaxResults = 50;
constexpr std::uint64_t kPipeMaxMessageBytes = 1024 * 1024;
constexpr wchar_t kInstallDirsFileName[] = L"komi-install-dirs.txt";
constexpr auto kInstallDirsCheckInterval = std::chrono::minutes(1);

SERVICE_STATUS g_svc_status = {};
SERVICE_STATUS_HANDLE g_svc_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::wstring g_crash_directory;
std::atomic_flag g_crash_dump_started = ATOMIC_FLAG_INIT;
std::atomic<int> g_events_clients{0};
constexpr int kMaxEventsClients = 32;

using json = nlohmann::json;

struct LocalFreeDeleter {
    void operator()(void* value) const {
        if (value != nullptr) {
            LocalFree(value);
        }
    }
};

using LocalSecurityDescriptor = std::unique_ptr<void, LocalFreeDeleter>;

struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes = {};
    LocalSecurityDescriptor descriptor;
};

std::filesystem::path ModuleDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = 0;
    while (true) {
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::filesystem::current_path();
        }
        if (size < buffer.size() - 1) {
            buffer.resize(size);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string TrimAsciiWhitespace(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<std::filesystem::path> ReadInstallDirsFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<std::filesystem::path> dirs;
    std::string line;
    bool first_line = true;
    while (std::getline(in, line)) {
        std::string trimmed = TrimAsciiWhitespace(std::move(line));
        if (first_line && trimmed.size() >= 3 &&
            static_cast<unsigned char>(trimmed[0]) == 0xef &&
            static_cast<unsigned char>(trimmed[1]) == 0xbb &&
            static_cast<unsigned char>(trimmed[2]) == 0xbf) {
            trimmed.erase(0, 3);
            trimmed = TrimAsciiWhitespace(std::move(trimmed));
        }
        first_line = false;
        if (!trimmed.empty()) {
            dirs.emplace_back(fsengine::Utf8ToWide(trimmed));
        }
    }
    return dirs;
}

bool DirectoryExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

bool ShouldSelfUninstall(const std::filesystem::path& module_dir) {
    const std::filesystem::path sentinel_path = module_dir / kInstallDirsFileName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sentinel_path, ec)) {
        return false;
    }

    const std::vector<std::filesystem::path> dirs = ReadInstallDirsFile(sentinel_path);
    if (dirs.empty()) {
        return false;
    }
    return std::all_of(dirs.begin(), dirs.end(), [](const std::filesystem::path& dir) {
        return !DirectoryExists(dir);
    });
}

bool DeleteInstalledService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(scm, kServiceName, DELETE);
    if (service == nullptr) {
        CloseServiceHandle(scm);
        return false;
    }
    const bool ok = DeleteService(service) != 0;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ok;
}

void CheckInstallDirsAndSelfUninstall(const std::filesystem::path& module_dir) {
    if (!ShouldSelfUninstall(module_dir)) {
        return;
    }
    DeleteInstalledService();
    SetEvent(g_stop_event);
}

PipeSecurity CreatePipeSecurity() {
    PipeSecurity security;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t kPipeSecurity[] = L"D:"
                                        L"(A;;GA;;;SY)"
                                        L"(A;;GA;;;BA)"
                                        L"(A;;GRGW;;;BU)";
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSecurity,
                                                             SDDL_REVISION_1,
                                                             &descriptor,
                                                             nullptr)) {
        security.descriptor.reset(descriptor);
        security.attributes.nLength = sizeof(security.attributes);
        security.attributes.lpSecurityDescriptor = security.descriptor.get();
        security.attributes.bInheritHandle = FALSE;
    }
    return security;
}

bool WaitForPipeIo(HANDLE pipe, OVERLAPPED* overlapped, DWORD* transferred, DWORD timeout = kPipeIoTimeoutMs) {
    HANDLE handles[] = {g_stop_event, overlapped->hEvent};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, timeout);
    if (wait != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, overlapped, &ignored, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, overlapped, transferred, FALSE) != 0;
}

bool ConnectPipe(HANDLE pipe) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return false;
    }

    const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (connected) {
        CloseHandle(overlapped.hEvent);
        return true;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        CloseHandle(overlapped.hEvent);
        return true;
    }
    if (err != ERROR_IO_PENDING) {
        CloseHandle(overlapped.hEvent);
        return false;
    }

    DWORD transferred = 0;
    const bool ok = WaitForPipeIo(pipe, &overlapped, &transferred);
    CloseHandle(overlapped.hEvent);
    return ok;
}

bool ReadPipeExact(HANDLE pipe, void* buffer, DWORD size, DWORD timeout = kPipeIoTimeoutMs) {
    char* cursor = static_cast<char*>(buffer);
    DWORD total = 0;
    const auto started = std::chrono::steady_clock::now();
    while (total < size) {
        DWORD remaining = timeout;
        if (timeout != kPipeIoTimeoutMs) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (elapsed >= timeout) return false;
            remaining -= static_cast<DWORD>(elapsed);
        }
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            return false;
        }

        DWORD chunk = 0;
        const BOOL ok = ReadFile(pipe, cursor + total, size - total, &chunk, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (!WaitForPipeIo(pipe, &overlapped, &chunk, remaining)) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        } else if (!ok) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        CloseHandle(overlapped.hEvent);
        if (chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

bool WritePipeExact(HANDLE pipe, const void* buffer, DWORD size, DWORD timeout = kPipeIoTimeoutMs) {
    const char* cursor = static_cast<const char*>(buffer);
    DWORD total = 0;
    const auto started = std::chrono::steady_clock::now();
    while (total < size) {
        DWORD remaining = timeout;
        if (timeout != kPipeIoTimeoutMs) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (elapsed >= timeout) return false;
            remaining -= static_cast<DWORD>(elapsed);
        }
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            return false;
        }

        DWORD chunk = 0;
        const BOOL ok = WriteFile(pipe, cursor + total, size - total, &chunk, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (!WaitForPipeIo(pipe, &overlapped, &chunk, remaining)) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        } else if (!ok) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        CloseHandle(overlapped.hEvent);
        if (chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

bool WriteFramedJson(HANDLE pipe, const json& value, DWORD timeout = kPipeIoTimeoutMs) {
    const std::string body = value.dump();
    if (body.size() > kPipeMaxMessageBytes) {
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(body.size());
    return WritePipeExact(pipe, &size, sizeof(size), timeout) &&
           WritePipeExact(pipe, body.data(), size, timeout);
}

const char* ChangeTypeName(fsengine::FsEventType type) {
    switch (type) {
    case fsengine::FsEventType::FileCreated:
        return "create";
    case fsengine::FsEventType::FileDeleted:
        return "delete";
    case fsengine::FsEventType::FileModified:
        return "modify";
    case fsengine::FsEventType::FileRenamed:
        return "rename";
    case fsengine::FsEventType::DirCreated:
        return "create";
    case fsengine::FsEventType::DirDeleted:
        return "delete";
    case fsengine::FsEventType::DirRenamed:
        return "rename";
    default:
        return "unknown";
    }
}

std::wstring NormalizeEventScope(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return path;
}

bool EventMatchesScope(const fsengine::FileChangeEvent& event,
                       const std::wstring& normalized_scope) {
    if (normalized_scope.empty()) {
        return true;
    }
    auto matches = [&](std::wstring path) {
        path = NormalizeEventScope(std::move(path));
        return path == normalized_scope ||
               (path.size() > normalized_scope.size() &&
                path.rfind(normalized_scope, 0) == 0 &&
                path[normalized_scope.size()] == L'\\');
    };
    return matches(event.path) || matches(event.old_path);
}

json ChangeEventJson(const fsengine::FileChangeEvent& event) {
    json item;
    item["type"] = "change";
    item["position"] = {
        {"epoch", event.position.epoch},
        {"sequence", event.position.sequence},
    };
    item["action"] = ChangeTypeName(event.type);
    item["id"] = event.frn;
    item["parent_id"] = event.parent_frn;
    item["is_directory"] = event.is_directory;
    item["name"] = fsengine::WideToUtf8(event.name);
    if (!event.path.empty()) {
        item["path"] = fsengine::WideToUtf8(event.path);
    }
    if (!event.old_name.empty()) {
        item["old_name"] = fsengine::WideToUtf8(event.old_name);
    }
    if (!event.old_path.empty()) {
        item["old_path"] = fsengine::WideToUtf8(event.old_path);
    }
    if (event.old_parent_frn != 0) {
        item["old_parent_id"] = event.old_parent_frn;
    }
    return item;
}

std::atomic<unsigned> active_windows_snapshots{0};

std::size_t SnapshotPageSize(const json& request) {
    const auto value = request.value("page_size", json(256));
    if (!value.is_number_integer() || value < 1 || value > 1024)
        throw std::runtime_error("page_size must be an integer from 1 to 1024");
    return value.get<std::size_t>();
}

void HandleWindowsSnapshot(HANDLE pipe, const json& request, fsengine::AutoFileSearch& search, bool& final_delivery) {
    final_delivery = false;
    // Millions of NTFS entries require an owned capture. Permit one capture,
    // not several per-client copies competing with the deployed search index.
    const auto previous = active_windows_snapshots.fetch_add(1);
    struct CountGuard {
        bool active = true;
        void Release() { if (active) { --active_windows_snapshots; active = false; } }
        ~CountGuard() { Release(); }
    } guard;
    auto fail = [&](const char* code, const std::string& error) {
        final_delivery = WriteFramedJson(pipe, json{{"ok", false}, {"type", "error"}, {"code", code}, {"error", error}}, 2000);
    };
    if (previous >= 1) { fail("snapshot_busy", "one Windows snapshot already active"); return; }
    try {
        if (request.value("version", 1) != 1) throw std::runtime_error("unsupported snapshot version");
        if (request.contains("actions") || request.contains("scope_path_bytes_hex"))
            throw std::runtime_error("unsupported Windows snapshot filter");
        const auto scope = fsengine::WindowsSnapshotView::NormalizeScope(
            fsengine::Utf8ToWide(request.value("scope_path", std::string{})));
        const bool files = request.value("include_files", true);
        const bool directories = request.value("include_directories", true);
        auto page_size = SnapshotPageSize(request);
        fsengine::MftSnapshot captured;
        fsengine::ChangePosition position;
        std::wstring error;
        if (!search.CaptureChangeSnapshot(&captured, &position, &error)) {
            fail("snapshot_unavailable", fsengine::WideToUtf8(error)); return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        auto view = std::make_unique<fsengine::WindowsSnapshotView>(captured);
        view->SetScope(scope);
        std::size_t next_entry = 0;
        std::uint64_t page_index = 0;
        auto resume_valid = [&] {
            if (!search.PullFileChanges(position, 0).expired) return true;
            const auto latest = search.CurrentChangePosition();
            final_delivery = WriteFramedJson(pipe, json{{"ok", false}, {"type", "resync_required"}, {"reason", "cursor_expired"},
                {"position", {{"epoch", latest.epoch}, {"sequence", latest.sequence}}}}, 2000);
            return false;
        };
        while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
            if (!resume_valid()) return;
            json entries = json::array();
            std::size_t page_bytes = 0;
            while (next_entry < captured.entries.size() && entries.size() < page_size) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    fail("snapshot_expired", "snapshot exceeded 120 seconds"); return;
                }
                const auto& entry = captured.entries[next_entry];
                if (!(entry.is_directory ? directories : files) || !view->Matches(entry, next_entry)) {
                    ++next_entry; continue;
                }
                json item{{"id", entry.frn}, {"parent_id", entry.parent_frn},
                    {"name", captured.NameString(entry)}, {"path", fsengine::WideToUtf8(view->Path(entry))},
                    {"is_directory", entry.is_directory}};
                const auto item_bytes = item.dump().size() + 1;
                if (item_bytes > 512 * 1024) throw std::runtime_error("snapshot entry exceeds frame budget");
                if (!entries.empty() && page_bytes + item_bytes > 512 * 1024) break;
                page_bytes += item_bytes;
                entries.push_back(std::move(item));
                ++next_entry;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                fail("snapshot_expired", "snapshot exceeded 120 seconds"); return;
            }
            if (!resume_valid()) return;
            const bool done = next_entry == captured.entries.size();
            const auto count = entries.size();
            if (done) {
                // Release the large copy and quota before the peer receives
                // done, so its immediately following request cannot race the
                // previous snapshot's destructor and spuriously see busy.
                view.reset();
                captured = {};
                guard.Release();
            }
            if (!WriteFramedJson(pipe, json{{"ok", true}, {"type", "snapshot_page"}, {"version", 1},
                {"position", {{"epoch", position.epoch}, {"sequence", position.sequence}}},
                {"page_index", page_index++}, {"count", count}, {"entries", std::move(entries)}, {"done", done}}, 2000)) return;
            if (done) { final_delivery = true; return; }
            std::uint32_t size = 0;
            if (!ReadPipeExact(pipe, &size, sizeof(size), 2000) || !size || size > kPipeMaxMessageBytes) return;
            std::string body(size, '\0');
            if (!ReadPipeExact(pipe, body.data(), size, 2000)) return;
            const auto next = json::parse(body);
            if (next.value("command", "") != "snapshot_next") throw std::runtime_error("expected snapshot_next");
            for (const auto& field : next.items())
                if (field.key() != "command" && field.key() != "page_size")
                    throw std::runtime_error("snapshot filters cannot change between pages");
            if (next.contains("page_size")) page_size = SnapshotPageSize(next);
        }
    } catch (const std::exception& e) { fail("snapshot_failed", e.what()); }
}

void HandleEventsPipeClient(HANDLE pipe,
                            fsengine::AutoFileSearch& search,
                            std::mutex& active_mutex,
                            std::condition_variable& active_cv,
                            std::atomic<int>& active_clients) {
    struct ActiveClientGuard {
        std::mutex& mutex;
        std::condition_variable& cv;
        std::atomic<int>& count;
        ~ActiveClientGuard() {
            --g_events_clients;
            --count;
            std::lock_guard lock(mutex);
            cv.notify_all();
        }
    } active_guard{active_mutex, active_cv, active_clients};

    std::uint32_t request_size = 0;
    if (!ReadPipeExact(pipe, &request_size, sizeof(request_size), 2000) ||
        request_size == 0 || request_size > kPipeMaxMessageBytes) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    std::string request(request_size, '\0');
    if (!ReadPipeExact(pipe, request.data(), request_size, 2000)) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }

    fsengine::ChangePosition cursor = search.CurrentChangePosition();
    std::wstring scope;
    std::set<std::string> actions;
    bool include_files = true;
    bool include_directories = true;
    try {
        const json req = json::parse(request);
        if (req.value("command", "") == "snapshot") {
            bool final_delivery = false;
            HandleWindowsSnapshot(pipe, req, search, final_delivery);
            // DisconnectNamedPipe discards unread outbound bytes. Keep the
            // final page/error available until the reader closes, bounded by
            // the snapshot idle timeout. Never FlushFileBuffers here: that
            // synchronous call can block indefinitely on an unresponsive peer.
            std::uint32_t ignored = 0;
            if (final_delivery) ReadPipeExact(pipe, &ignored, sizeof(ignored), 2000);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }
        if (req.value("command", "") != "subscribe_changes" || req.value("version", 1) != 1) {
            if (WriteFramedJson(pipe, json{{"type", "error"}, {"error", "Invalid subscription request"}}, 2000)) {
                std::uint32_t ignored = 0;
                ReadPipeExact(pipe, &ignored, sizeof(ignored), 2000);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }
        if (req.contains("since") && req["since"].is_object()) {
            cursor.epoch = req["since"].value("epoch", std::uint64_t{0});
            cursor.sequence = req["since"].value("sequence", std::uint64_t{0});
        }
        if (req.contains("scope_path")) {
            scope = NormalizeEventScope(fsengine::Utf8ToWide(req["scope_path"].get<std::string>()));
        }
        include_files = req.value("include_files", true);
        include_directories = req.value("include_directories", true);
        if (req.contains("actions")) {
            for (const auto& action : req["actions"].get<std::vector<std::string>>()) {
                if (action != "create" && action != "delete" && action != "modify" && action != "rename")
                    throw std::runtime_error("Unknown subscription action");
                actions.insert(action);
            }
        }
    } catch (const std::exception& e) {
        if (WriteFramedJson(pipe, json{{"type", "error"}, {"error", std::string("Invalid subscription JSON: ") + e.what()}}, 2000)) {
            std::uint32_t ignored = 0;
            ReadPipeExact(pipe, &ignored, sizeof(ignored), 2000);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }

    const fsengine::ChangePosition current = search.CurrentChangePosition();
    if (!WriteFramedJson(pipe, json{
            {"type", "subscribed"},
            {"position", {{"epoch", current.epoch}, {"sequence", current.sequence}}},
        }, 2000)) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available != 0) break;
        fsengine::FileChangePullResult pulled = search.PullFileChanges(cursor, 256);
        if (pulled.expired) {
            const fsengine::ChangePosition latest = search.CurrentChangePosition();
            if (WriteFramedJson(pipe, json{
                {"type", "resync_required"},
                {"reason", "cursor_expired"},
                {"position", {{"epoch", latest.epoch}, {"sequence", latest.sequence}}},
            }, 2000)) {
                std::uint32_t ignored = 0;
                ReadPipeExact(pipe, &ignored, sizeof(ignored), 2000);
            }
            break;
        }
        for (const fsengine::FileChangeEvent& event : pulled.events) {
            const bool matches_type = event.is_directory ? include_directories : include_files;
            const bool matches_action = actions.empty() || actions.count(ChangeTypeName(event.type)) != 0;
            if (matches_type && matches_action && EventMatchesScope(event, scope) && !WriteFramedJson(pipe, ChangeEventJson(event), 2000)) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                return;
            }
        }
        cursor = pulled.position;
        if (pulled.events.empty()) {
            search.WaitFileChanges(cursor, 500);
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

void RunEventsPipeServer(fsengine::AutoFileSearch& search,
                         std::mutex& active_mutex,
                         std::condition_variable& active_cv,
                         std::atomic<int>& active_clients) {
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        PipeSecurity pipe_security = CreatePipeSecurity();
        HANDLE pipe = CreateNamedPipeW(kEventsPipeName,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       kPipeBufSize,
                                       kPipeBufSize,
                                       30000,
                                       pipe_security.attributes.lpSecurityDescriptor != nullptr
                                           ? &pipe_security.attributes
                                           : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(g_stop_event, 1000) != WAIT_TIMEOUT) {
                break;
            }
            continue;
        }
        if (!ConnectPipe(pipe)) {
            CloseHandle(pipe);
            continue;
        }
        if (g_events_clients.load() >= kMaxEventsClients) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        try {
            ++g_events_clients;
            ++active_clients;
            std::thread(HandleEventsPipeClient,
                        pipe,
                        std::ref(search),
                        std::ref(active_mutex),
                        std::ref(active_cv),
                        std::ref(active_clients)).detach();
        } catch (...) {
            --g_events_clients;
            --active_clients;
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }
}

bool ReadPipeExactSync(HANDLE pipe, void* buffer, DWORD size) {
    char* cursor = static_cast<char*>(buffer);
    DWORD total = 0;
    while (total < size) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, cursor + total, size - total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

bool WritePipeExactSync(HANDLE pipe, const void* buffer, DWORD size) {
    const char* cursor = static_cast<const char*>(buffer);
    DWORD total = 0;
    while (total < size) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, cursor + total, size - total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

bool SendServicePipeRequest(const std::string& request, std::string* response) {
    if (!WaitNamedPipeW(kPipeName, 5000)) {
        return false;
    }
    HANDLE pipe = CreateFileW(kPipeName,
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::uint32_t request_length = static_cast<std::uint32_t>(request.size());
    std::uint32_t response_length = 0;
    bool ok = WritePipeExactSync(pipe, &request_length, sizeof(request_length)) &&
              WritePipeExactSync(pipe, request.data(), request_length) &&
              ReadPipeExactSync(pipe, &response_length, sizeof(response_length)) &&
              response_length <= kPipeMaxMessageBytes;
    if (ok) {
        std::string body(response_length, '\0');
        ok = ReadPipeExactSync(pipe, body.data(), response_length);
        if (ok && response != nullptr) {
            *response = std::move(body);
        }
    }
    CloseHandle(pipe);
    return ok;
}

fsengine::AutoFileSearchConfig DefaultSearchConfig() {
    fsengine::AutoFileSearchConfig config;
    config.cache_directory = (ModuleDirectory() / L"cache").wstring();
    config.search_mode = fsengine::SearchMode::Ngram;
    config.filter64_strategy = fsengine::Filter64Strategy::DynamicUnicodeHash;
    config.monitor_enabled = true;
    return config;
}

std::string ErrorResponse(std::string message) {
    json resp;
    resp["ok"] = false;
    resp["error"] = std::move(message);
    return resp.dump();
}

fsengine::SearchMode ParseSearchMode(const json& req, fsengine::SearchMode fallback) {
    if (!req.contains("mode")) {
        return fallback;
    }
    const std::string mode = req["mode"].get<std::string>();
    if (mode == "filter") {
        return fsengine::SearchMode::Filter;
    }
    if (mode == "ngram") {
        return fsengine::SearchMode::Ngram;
    }
    return fallback;
}

std::string HandleStatusRequest(const fsengine::AutoFileSearch& search) {
    const fsengine::AutoFileSearchStatus status = search.Status();
    json resp;
    resp["ok"] = true;
    resp["running"] = status.running;
    resp["mft_running"] = status.mft_running;
    resp["search_running"] = status.search_running;
    resp["mft_building"] = status.mft_building;
    resp["search_building"] = status.search_building;
    resp["ready"] = status.ready;
    resp["drives_ready"] = status.ready_drive_count;
    resp["total_files"] = status.total_file_count;
    resp["total_directories"] = status.total_directory_count;
    if (!status.mft_error.empty()) {
        resp["mft_error"] = fsengine::WideToUtf8(status.mft_error);
    }
    if (!status.search_error.empty()) {
        resp["search_error"] = fsengine::WideToUtf8(status.search_error);
    }
    json drives = json::array();
    for (const auto& drive : status.drives) {
        json item;
        item["drive"] = std::string(1, static_cast<char>(drive.drive));
        item["ready"] = drive.ready;
        item["files"] = drive.file_count;
        item["directories"] = drive.directory_count;
        if (!drive.error.empty()) {
            item["error"] = fsengine::WideToUtf8(drive.error);
        }
        drives.push_back(std::move(item));
    }
    resp["drives"] = std::move(drives);
    return resp.dump();
}

void ParseDrives(const json& req, fsengine::AutoFileSearchConfig* config) {
    if (!req.contains("drives") || !req["drives"].is_array()) {
        return;
    }
    config->drives.clear();
    for (const auto& value : req["drives"]) {
        const std::string text = value.get<std::string>();
        if (!text.empty()) {
            config->drives.push_back(static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(text[0]))));
        }
    }
}

void ApplyMftConfig(const json& req, fsengine::AutoFileSearchConfig* config) {
    if (req.contains("cache_directory")) {
        config->cache_directory = fsengine::Utf8ToWide(req["cache_directory"].get<std::string>());
    }
    if (req.contains("monitor_enabled")) {
        config->monitor_enabled = req["monitor_enabled"].get<bool>();
    }
    ParseDrives(req, config);
}

void ApplySearchConfig(const json& req, fsengine::AutoFileSearchConfig* config) {
    config->search_mode = ParseSearchMode(req, config->search_mode);
    if (req.contains("use_128_bit_filters")) {
        config->use_128_bit_filters = req["use_128_bit_filters"].get<bool>();
    }
    if (req.contains("deferred_filter_merge_threshold")) {
        config->deferred_filter_merge_threshold = req["deferred_filter_merge_threshold"].get<std::size_t>();
    }
    if (req.contains("filter64_strategy")) {
        const std::string strategy = req["filter64_strategy"].get<std::string>();
        config->filter64_strategy = strategy == "utf8-base36"
            ? fsengine::Filter64Strategy::Utf8Base36
            : fsengine::Filter64Strategy::DynamicUnicodeHash;
    }
}

std::string HandleSearchRequest(const json& req, fsengine::AutoFileSearch& search) {
    if (!search.Running()) {
        return ErrorResponse("Search is not running");
    }
    const std::string query_text = req.value("query", "");
    if (query_text.empty()) {
        return ErrorResponse("Missing 'query' field");
    }

    std::size_t max_results = req.value("max_results", kDefaultMaxResults);
    if (max_results == 0) {
        max_results = kDefaultMaxResults;
    }

    fsengine::AutoFileSearchOptions options;
    options.max_results = max_results;
    options.phase = fsengine::SearchPhase::ExactThenFuzzy;
    // Preserve the deployed default when omitted; phase selection is additive
    // for clients that explicitly opt into the shared cross-platform contract.
    if (req.contains("phase")) {
        const auto phase = req["phase"].get<std::string>();
        if (phase == "exact_only") options.phase = fsengine::SearchPhase::ExactOnly;
        else if (phase == "fuzzy_only") options.phase = fsengine::SearchPhase::FuzzyOnly;
        else if (phase != "exact_then_fuzzy") return ErrorResponse("Unknown search phase");
    }
    if (req.contains("scope_path")) {
        options.scope_path = fsengine::Utf8ToWide(req["scope_path"].get<std::string>());
    }

    const std::wstring query = fsengine::Utf8ToWide(query_text);
    if (query.empty()) {
        return ErrorResponse("Query is not valid UTF-8");
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<fsengine::SearchResult> results = req.contains("phase")
        ? search.SearchWithPhase(query, options) : search.Search(query, options);
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json resp;
    resp["ok"] = true;
    resp["count"] = results.size();
    resp["elapsed_ms"] = elapsed_ms;
    json results_json = json::array();
    for (const auto& result : results) {
        json item;
        item["name"] = fsengine::WideToUtf8(result.name);
        item["path"] = fsengine::WideToUtf8(result.path);
        item["frn"] = result.frn;
        results_json.push_back(std::move(item));
    }
    resp["results"] = std::move(results_json);
    return resp.dump();
}

std::string HandleRequest(const std::string& json_text,
                          fsengine::AutoFileSearch& search,
                          fsengine::AutoFileSearchConfig& config) {
    try {
        const json req = json::parse(json_text);
        if (req.contains("command")) {
            const std::string command = req["command"].get<std::string>();
            if (command == "status") {
                return HandleStatusRequest(search);
            }
            if (command == "shutdown") {
                SetEvent(g_stop_event);
                json resp;
                resp["ok"] = true;
                resp["message"] = "Shutting down";
                return resp.dump();
            }
            if (command == "self_uninstall") {
                const bool deleted = DeleteInstalledService();
                SetEvent(g_stop_event);
                json resp;
                resp["ok"] = deleted;
                resp["message"] = deleted ? "Service uninstall requested" : "Service delete failed";
                return resp.dump();
            }
            if (command == "config_mft") {
                ApplyMftConfig(req, &config);
                std::wstring error;
                const bool ok = search.Configure(config, &error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                json resp;
                resp["ok"] = true;
                return resp.dump();
            }
            if (command == "config_search") {
                ApplySearchConfig(req, &config);
                std::wstring error;
                const bool ok = search.ConfigureSearch(config, &error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                json resp;
                resp["ok"] = true;
                return resp.dump();
            }
            if (command == "release_facts") { 
                search.ReleaseFacts();
                json resp;
                resp["ok"] = true;
                return resp.dump();
            }
            if (command == "rebuild_ngram") {
                search.RebuildNgramIndexesIfNeeded();
                return HandleStatusRequest(search);
            }
            if (command == "start_mft") {
                std::wstring error;
                const bool ok = search.StartMft(&error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                return HandleStatusRequest(search);
            }
            if (command == "start_search") {
                std::wstring error;
                const bool ok = search.StartSearch(&error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                return HandleStatusRequest(search);
            }
            if (command == "start_mft_async") {
                std::wstring error;
                const bool ok = search.StartMftAsync(&error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                return HandleStatusRequest(search);
            }
            if (command == "start_search_async") {
                std::wstring error;
                const bool ok = search.StartSearchAsync(&error);
                if (!ok) {
                    return ErrorResponse(fsengine::WideToUtf8(error));
                }
                return HandleStatusRequest(search);
            }
            if (command == "stop_search") {
                search.StopSearch();
                return HandleStatusRequest(search);
            }
            if (command == "stop_mft") {
                search.StopMft();
                return HandleStatusRequest(search);
            }
        }
        return HandleSearchRequest(req, search);
    } catch (const json::exception& e) {
        return ErrorResponse(std::string("JSON parse error: ") + e.what());
    }
}

bool RequestRequiresExclusiveHandling(const std::string& json_text) {
    try {
        const json req = json::parse(json_text);
        if (!req.contains("command")) {
            return false;
        }
        const std::string command = req["command"].get<std::string>();
        return command != "status";
    } catch (const json::exception&) {
        return false;
    }
}

void HandlePipeClient(HANDLE pipe,
                      fsengine::AutoFileSearch& search,
                      fsengine::AutoFileSearchConfig& config,
                      std::mutex& request_mutex,
                      std::mutex& active_mutex,
                      std::condition_variable& active_cv,
                      std::atomic<int>& active_clients) {
    struct ActiveClientGuard {
        std::mutex& mutex;
        std::condition_variable& cv;
        std::atomic<int>& count;
        ~ActiveClientGuard() {
            --count;
            std::lock_guard lock(mutex);
            cv.notify_all();
        }
    } active_guard{active_mutex, active_cv, active_clients};

    try {

    std::uint32_t message_length = 0;
    if (!ReadPipeExact(pipe, &message_length, sizeof(message_length)) ||
        message_length == 0 ||
        message_length > kPipeMaxMessageBytes) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }

    std::string request(static_cast<std::size_t>(message_length), '\0');
    if (ReadPipeExact(pipe, request.data(), message_length)) {
        std::string response;
        const bool exclusive = RequestRequiresExclusiveHandling(request);
        if (exclusive)
        {
            std::lock_guard lock(request_mutex);
            response = HandleRequest(request, search, config);
        }
        else
        {
            response = HandleRequest(request, search, config);
        }
        const std::uint32_t response_length = static_cast<std::uint32_t>(response.size());
        WritePipeExact(pipe, &response_length, sizeof(response_length));
        WritePipeExact(pipe, response.data(), response_length);
        FlushFileBuffers(pipe);
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    } catch (const std::exception& ex) {
        fsengine::LogLine(std::string("[service] Pipe client exception: ") + ex.what());
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    } catch (...) {
        fsengine::LogLine("[service] Pipe client unknown exception");
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

std::wstring InitializeCrashDirectory() {
    std::filesystem::path directory = ModuleDirectory() / L"crash";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (!ec) {
        return directory.wstring();
    }
    std::wstring temp(MAX_PATH, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (length != 0 && length < temp.size()) {
        temp.resize(length);
        directory = std::filesystem::path(temp) / L"FileSearch-crash";
        ec.clear();
        std::filesystem::create_directories(directory, ec);
        if (!ec) {
            return directory.wstring();
        }
    }
    return ModuleDirectory().wstring();
}

void WriteCrashArtifacts(EXCEPTION_POINTERS* exception, const char* reason) noexcept {
    if (g_crash_dump_started.test_and_set()) {
        return;
    }
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    const DWORD process_id = GetCurrentProcessId();
    const DWORD thread_id = GetCurrentThreadId();
    wchar_t stem[160] = {};
    _snwprintf_s(stem, _TRUNCATE,
                 L"FileSearchService-crash-%04u%02u%02u-%02u%02u%02u-%lu-%lu",
                 time.wYear, time.wMonth, time.wDay,
                 time.wHour, time.wMinute, time.wSecond,
                 process_id, thread_id);
    const std::filesystem::path base = std::filesystem::path(g_crash_directory) / stem;

    const std::wstring dump_path = base.wstring() + L".dmp";
    HANDLE dump = CreateFileW(dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception_info = {};
        exception_info.ThreadId = thread_id;
        exception_info.ExceptionPointers = exception;
        exception_info.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), process_id, dump,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                     MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpScanMemory),
                          exception != nullptr ? &exception_info : nullptr,
                          nullptr, nullptr);
        FlushFileBuffers(dump);
        CloseHandle(dump);
    }

    const DWORD exception_code = exception != nullptr && exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionCode : 0;
    const void* exception_address = exception != nullptr && exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    char details[1024] = {};
    const int detail_length = _snprintf_s(details, _TRUNCATE,
        "reason=%s\r\nexception_code=0x%08lX\r\nexception_address=%p\r\n"
        "process_id=%lu\r\nthread_id=%lu\r\nlast_error=%lu\r\n",
        reason != nullptr ? reason : "unknown",
        exception_code, exception_address, process_id, thread_id, GetLastError());
    const std::wstring text_path = base.wstring() + L".txt";
    HANDLE text_file = CreateFileW(text_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(text_file, details,
                  detail_length > 0 ? static_cast<DWORD>(detail_length) : 0,
                  &written, nullptr);
        FlushFileBuffers(text_file);
        CloseHandle(text_file);
    }
}

LONG WINAPI ServiceUnhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    WriteCrashArtifacts(exception, "unhandled_exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void ServiceTerminateHandler() noexcept {
    WriteCrashArtifacts(nullptr, "std_terminate");
    TerminateProcess(GetCurrentProcess(), 3);
    std::abort();
}

void InstallServiceCrashHandler() {
    g_crash_directory = InitializeCrashDirectory();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(ServiceUnhandledExceptionFilter);
    std::set_terminate(ServiceTerminateHandler);
}

void SearchEngineMain() {
    fsengine::AutoFileSearchConfig config = DefaultSearchConfig();
    // Enumerate all fixed drives for automatic indexing
    {
        const DWORD drives_mask = GetLogicalDrives();
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            const int bit = static_cast<int>(drive - L'A');
            if ((drives_mask & (1u << bit)) == 0) {
                continue;
            }
            const wchar_t root[] = {drive, L':', L'\\', L'\0'};
            if (GetDriveTypeW(root) == DRIVE_FIXED) {
                config.drives.push_back(drive);
            }
        }
    }
    fsengine::AutoFileSearch search(config);

    // Configure and auto-start MFT on all fixed drives
    {
        std::wstring error;
        search.Configure(config, &error);
        search.StartMftAsync(&error);
    }

    // Background thread: wait for MFT → start search → wait for search → ReleaseFacts
    std::thread startup_thread([&search, stop = g_stop_event]() {
        bool search_started = false;
        bool facts_released = false;
        try {
            while (WaitForSingleObject(stop, 500) == WAIT_TIMEOUT) {
                const auto s = search.Status();
                if (!search_started && s.mft_running && !s.mft_building) {
                    std::wstring error;
                    if (!search.StartSearchAsync(&error)) {
                        fsengine::LogLine("[service] Failed to start search asynchronously: " +
                                          fsengine::WideToUtf8(error));
                        break;
                    }
                    search_started = true;
                }
                if (search_started && !facts_released &&
                    !s.mft_building && !s.search_building &&
                    s.mft_running && s.search_running) {
                    search.ReleaseFacts();
                    facts_released = true;
                    break;
                }
            }
        } catch (const std::exception& ex) {
            fsengine::LogLine(std::string("[service] Startup worker exception: ") + ex.what());
        } catch (...) {
            fsengine::LogLine("[service] Startup worker unknown exception");
        }
    });

    const std::filesystem::path module_dir = ModuleDirectory();
    auto next_install_dirs_check = std::chrono::steady_clock::now();
    std::mutex request_mutex;
    std::mutex active_mutex;
    std::condition_variable active_cv;
    std::atomic<int> active_clients{0};
    std::thread events_server(RunEventsPipeServer,
                              std::ref(search),
                              std::ref(active_mutex),
                              std::ref(active_cv),
                              std::ref(active_clients));

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_install_dirs_check) {
            CheckInstallDirsAndSelfUninstall(module_dir);
            next_install_dirs_check = now + kInstallDirsCheckInterval;
            if (WaitForSingleObject(g_stop_event, 0) != WAIT_TIMEOUT) {
                break;
            }
        }
        PipeSecurity pipe_security = CreatePipeSecurity();
        HANDLE pipe = CreateNamedPipeW(kPipeName,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       kPipeBufSize,
                                       kPipeBufSize,
                                       30000,
                                       pipe_security.attributes.lpSecurityDescriptor != nullptr
                                           ? &pipe_security.attributes
                                           : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) {
                break;
            }
            continue;
        }

        if (!ConnectPipe(pipe)) {
            CloseHandle(pipe);
            continue;
        }

        try {
            ++active_clients;
            std::thread(HandlePipeClient,
                        pipe,
                        std::ref(search),
                        std::ref(config),
                        std::ref(request_mutex),
                        std::ref(active_mutex),
                        std::ref(active_cv),
                        std::ref(active_clients)).detach();
        } catch (...) {
            --active_clients;
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    fsengine::LogLine("[service] Shutdown begin active_clients=" +
                      std::to_string(active_clients.load()));
    if (events_server.joinable()) {
        events_server.join();
    }

    std::unique_lock active_lock(active_mutex);
    active_cv.wait(active_lock, [&active_clients]() {
        return active_clients.load() == 0;
    });
    active_lock.unlock();
    fsengine::LogLine("[service] Shutdown clients drained");
    if (startup_thread.joinable()) {
        startup_thread.join();
    }
    fsengine::LogLine("[service] Shutdown startup worker joined");
    search.Stop();
    fsengine::LogLine("[service] Shutdown search core stopped");
}

bool WaitServiceRunning(SC_HANDLE service, DWORD timeout_ms) {
    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        if (!QueryServiceStatusEx(service,
                                  SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status),
                                  sizeof(status),
                                  &needed)) {
            return false;
        }
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return true;
        }
        if (status.dwCurrentState != SERVICE_START_PENDING) {
            return false;
        }
        Sleep(300);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool StartServiceIfNeeded(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service,
                              SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status),
                              sizeof(status),
                              &needed)) {
        std::wcerr << L"QueryServiceStatusEx failed: " << GetLastError() << std::endl;
        return false;
    }
    if (status.dwCurrentState == SERVICE_RUNNING) {
        return true;
    }
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        return WaitServiceRunning(service, 60000);
    }
    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            std::wcerr << L"StartService failed: " << err << std::endl;
            return false;
        }
    }
    return WaitServiceRunning(service, 60000);
}

bool InstallService(SC_HANDLE* installed_service = nullptr) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << std::endl;
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SC_HANDLE service = CreateServiceW(scm,
                                       kServiceName,
                                       L"FileSearch Engine",
                                       SERVICE_ALL_ACCESS,
                                       SERVICE_WIN32_OWN_PROCESS,
                                       SERVICE_AUTO_START,
                                       SERVICE_ERROR_NORMAL,
                                       path,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);
    bool ok = false;
    if (service == nullptr) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::wcout << L"Service already installed." << std::endl;
            service = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
            ok = service != nullptr;
            if (!ok) {
                std::wcerr << L"OpenService failed: " << GetLastError() << std::endl;
            }
        } else {
            std::wcerr << L"CreateService failed: " << err << std::endl;
        }
    } else {
        std::wcout << L"Service installed successfully." << std::endl;
        ok = true;
    }

    // Grant SERVICE_START + SERVICE_QUERY_STATUS to Authenticated Users
    // so that non-admin callers can start the service without UAC.
    if (ok) {
        // LC=SERVICE_QUERY_STATUS(0x0004) RP=SERVICE_START(0x0010)
        constexpr wchar_t kServiceSddl[] =
            L"D:(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
            L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)"
            L"(A;;LCRP;;;AU)";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kServiceSddl, SDDL_REVISION_1, &sd, nullptr)) {
            SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, sd);
            LocalFree(sd);
        }
    }

    if (ok && installed_service != nullptr) {
        *installed_service = service;
    } else if (service != nullptr) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool EnsureService() {
    SC_HANDLE service = nullptr;
    if (!InstallService(&service)) {
        return false;
    }
    const bool running = StartServiceIfNeeded(service);
    CloseServiceHandle(service);
    if (running) {
        std::wcout << L"Service is running." << std::endl;
    }
    return running;
}

void UninstallService() {
    std::string response;
    if (SendServicePipeRequest(R"({"command":"self_uninstall"})", &response)) {
        try {
            const json resp = json::parse(response);
            if (resp.value("ok", false)) {
                std::wcout << L"Service uninstall requested." << std::endl;
                return;
            }
        } catch (const json::exception&) {
        }
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << std::endl;
        return;
    }
    SC_HANDLE service = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP);
    if (service == nullptr) {
        std::wcerr << L"Service not found." << std::endl;
        CloseServiceHandle(scm);
        return;
    }
    SERVICE_STATUS status = {};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    if (DeleteService(service)) {
        std::wcout << L"Service uninstalled." << std::endl;
    } else {
        std::wcerr << L"DeleteService failed: " << GetLastError() << std::endl;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

void RunConsole() {
    std::wcout << L"Running in console mode..." << std::endl;
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try {
        SearchEngineMain();
    } catch (const std::exception& ex) {
        fsengine::LogLine(std::string("[service] Console main exception: ") + ex.what());
        WriteCrashArtifacts(nullptr, "console_main_exception");
    } catch (...) {
        fsengine::LogLine("[service] Console main unknown exception");
        WriteCrashArtifacts(nullptr, "console_main_unknown_exception");
    }
    CloseHandle(g_stop_event);
}

void ReportSvcStatus(DWORD current, DWORD exit_code, DWORD wait_hint) {
    g_svc_status.dwCurrentState = current;
    g_svc_status.dwWin32ExitCode = exit_code;
    g_svc_status.dwWaitHint = wait_hint;
    g_svc_status.dwControlsAccepted = current == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_svc_handle, &g_svc_status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP) {
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        SetEvent(g_stop_event);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_svc_handle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (g_svc_handle == nullptr) {
        return;
    }

    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwServiceSpecificExitCode = 0;
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 30000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
    DWORD service_exit_code = NO_ERROR;
    try {
        SearchEngineMain();
    } catch (const std::exception& ex) {
        fsengine::LogLine(std::string("[service] Service main exception: ") + ex.what());
        WriteCrashArtifacts(nullptr, "service_main_exception");
        service_exit_code = ERROR_UNHANDLED_EXCEPTION;
    } catch (...) {
        fsengine::LogLine("[service] Service main unknown exception");
        WriteCrashArtifacts(nullptr, "service_main_unknown_exception");
        service_exit_code = ERROR_UNHANDLED_EXCEPTION;
    }

    CloseHandle(g_stop_event);
    ReportSvcStatus(SERVICE_STOPPED, service_exit_code, 0);
}

void StartService() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        fsengine::LogLine("[service] StartServiceCtrlDispatcher failed error=" +
                          std::to_string(GetLastError()));
    }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    InstallServiceCrashHandler();
    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    fsengine::LogLine("[service] Process start pid=" + std::to_string(GetCurrentProcessId()) +
                      " crash_dir=" + fsengine::WideToUtf8(g_crash_directory));

    if (argc >= 2) {
        std::wstring command = argv[1];
        for (wchar_t& ch : command) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        if (command == L"install") {
            return EnsureService() ? 0 : 1;
        }
        if (command == L"ensure") {
            return EnsureService() ? 0 : 1;
        }
        if (command == L"uninstall") {
            UninstallService();
            return 0;
        }
        if (command == L"dump") {
            WriteCrashArtifacts(nullptr, "manual_dump");
            std::wcout << L"Diagnostic dump written to: " << g_crash_directory << std::endl;
            return 0;
        }
        if (command == L"run") {
            RunConsole();
            return 0;
        }
    }

    StartService();
    return 0;
}

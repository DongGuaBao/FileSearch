#include "auto_file_search.h"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace fsengine;
static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Arguments name a fresh, test-owned directory and a result file. No existing
// service cache is reused, and fixture cleanup is deliberately left to caller.
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path root(argv[1]);
    std::ofstream result{fs::path(argv[2])};
    try {
        Require(root.is_absolute() && !fs::exists(root), "fixture must be a fresh absolute directory");
        fs::create_directories(root);
        AutoFileSearchConfig config;
        config.cache_directory = (root / L"cache").wstring();
        config.drives = {root.wstring().front()};
        AutoFileSearch service(config);
        std::wstring error;
        Require(service.StartMft(&error), "StartMft failed");
        auto cursor = service.CurrentChangePosition();
        auto await = [&](const fs::path& path, FsEventType type) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < end) {
                const auto batch = service.PullFileChanges(cursor);
                Require(!batch.expired, "probe cursor expired");
                cursor = batch.position;
                for (const auto& event : batch.events)
                    if (event.type == type && _wcsicmp(event.path.c_str(), path.c_str()) == 0)
                        return event;
                service.WaitFileChanges(cursor, 250);
            }
            throw std::runtime_error("expected fixture event timed out");
        };
        auto verify = [&](const FileChangeEvent& event, const char* name, bool present) {
            MftSnapshot snapshot;
            ChangePosition position;
            Require(service.CaptureChangeSnapshot(&snapshot, &position, &error), "capture failed");
            Require(position.epoch == event.position.epoch && position.sequence >= event.position.sequence,
                    "snapshot cursor precedes observed change");
            bool found = false;
            for (const auto& entry : snapshot.entries) if (entry.frn == event.frn) {
                found = true;
                Require(snapshot.Name(entry) == name, "snapshot name differs from latest WAL");
                Require(!entry.is_directory, "file type lost");
            }
            Require(found == present, "snapshot membership differs from observed change");
        };
        const auto original = root / L"snapshot_probe_original.txt";
        const auto renamed = root / L"snapshot_probe_renamed.txt";
        { std::ofstream file(original); file << "probe"; }
        verify(await(original, FsEventType::FileCreated), "snapshot_probe_original.txt", true);
        service.ReleaseFacts();
        fs::rename(original, renamed);
        verify(await(renamed, FsEventType::FileRenamed), "snapshot_probe_renamed.txt", true);
        fs::remove(renamed);
        verify(await(renamed, FsEventType::FileDeleted), "", false);
        service.Stop();
        result << "ok: create, released-facts rename, delete, fenced cursor\n";
        return 0;
    } catch (const std::exception& exception) {
        result << "failed: " << exception.what() << '\n';
        return 1;
    }
}

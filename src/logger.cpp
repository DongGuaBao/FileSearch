#include "logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace fsengine {
namespace {

constexpr std::uintmax_t kMaxLogFileBytes = 16ull * 1024ull * 1024ull;
constexpr int kRetainedLogFiles = 5;

std::filesystem::path ModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory),
                            &module)) {
        module = nullptr;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
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

struct LogState {
    std::ofstream file;
    std::filesystem::path path;
    std::uintmax_t size = 0;
    bool initialized = false;
};

void RotateLogFiles(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path.wstring() + L"." + std::to_wstring(kRetainedLogFiles), ec);
    for (int index = kRetainedLogFiles - 1; index >= 1; --index) {
        const std::filesystem::path source = path.wstring() + L"." + std::to_wstring(index);
        const std::filesystem::path target = path.wstring() + L"." + std::to_wstring(index + 1);
        ec.clear();
        if (std::filesystem::exists(source, ec) && !ec) {
            std::filesystem::rename(source, target, ec);
        }
    }
    ec.clear();
    if (std::filesystem::exists(path, ec) && !ec) {
        std::filesystem::rename(path, path.wstring() + L".1", ec);
    }
}

bool OpenLogFile(LogState& state, const std::filesystem::path& path) {
    try {
        std::error_code ec;
        const std::uintmax_t existing_size = std::filesystem::file_size(path, ec);
        if (!ec && existing_size >= kMaxLogFileBytes) {
            RotateLogFiles(path);
        }
        state.file.clear();
        state.file.open(path, std::ios::out | std::ios::app | std::ios::binary);
        if (!state.file) {
            return false;
        }
        state.path = path;
        ec.clear();
        state.size = std::filesystem::file_size(path, ec);
        if (ec) {
            state.size = 0;
        }
        return true;
    } catch (...) {
        return false;
    }
}

LogState& GetLogState() {
    static LogState state;
    if (!state.initialized) {
        state.initialized = true;
        if (!OpenLogFile(state, ModuleDirectory() / L"FileSearch.log")) {
            try {
                std::wstring temp(MAX_PATH, L'\0');
                const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
                if (length != 0 && length < temp.size()) {
                    temp.resize(length);
                    const std::filesystem::path directory = std::filesystem::path(temp) / L"FileSearch";
                    std::filesystem::create_directories(directory);
                    OpenLogFile(state, directory / L"FileSearch.log");
                }
            } catch (...) {
            }
        }
    }
    return state;
}

void RotateIfNeeded(LogState& state, std::uintmax_t incoming_bytes) {
    if (!state.file || state.size + incoming_bytes <= kMaxLogFileBytes) {
        return;
    }
    state.file.flush();
    state.file.close();
    RotateLogFiles(state.path);
    OpenLogFile(state, state.path);
}

std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

void WriteLogLine(const std::string& line) {
    std::lock_guard lock(LogMutex());
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char prefix[128] = {};
    _snprintf_s(prefix, _TRUNCATE,
                "[%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu] ",
                time.wYear, time.wMonth, time.wDay,
                time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId());
    std::cout << prefix << line << std::endl;
    LogState& state = GetLogState();
    const std::uintmax_t incoming_bytes = std::char_traits<char>::length(prefix) + line.size() + 1;
    RotateIfNeeded(state, incoming_bytes);
    if (state.file) {
        state.file << prefix << line << '\n';
        state.file.flush();
        state.size += incoming_bytes;
    }
}

}  // namespace

void LogLine(const std::string& line) {
    WriteLogLine(line);
}

void LogLine(std::string&& line) {
    WriteLogLine(line);
}

}  // namespace fsengine

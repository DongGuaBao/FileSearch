#pragma once

#if defined(_WIN32)
#  ifdef FILESEARCH_EXPORTS
#    define FSENGINE_API __declspec(dllexport)
#  else
#    define FSENGINE_API __declspec(dllimport)
#  endif
#else
#  define FSENGINE_API __attribute__((visibility("default")))
#endif

// 抑制 C4251: STL 模板成员"需要有 dll 接口"
// DLL 和所有消费者使用同一工具链和动态 CRT，安全忽略
#if defined(_MSC_VER)
#pragma warning(disable: 4251)
#endif

#include <cstdint>
#include <string>

namespace fsengine {

using Frn = std::uint64_t;

constexpr Frn kRootParentFrn = 0;

struct RawEntry {
    Frn frn = 0;
    Frn parent_frn = 0;
    std::wstring name;
    bool is_directory = false;
};

struct IndexChange {
    RawEntry entry;
    bool remove = false;
};

enum class ChangeMessageType : std::uint8_t {
    FileChange = 0,
    WalThreshold = 1,
};

struct ChangeMessage {
    ChangeMessageType type = ChangeMessageType::FileChange;
    IndexChange change;
};

struct WalPosition {
    std::uint64_t generation = 0;
    std::uint64_t offset = 0;
};

struct SearchScope {
    bool enabled = false;
    wchar_t drive = L'\0';
    Frn root_frn = 0;
    std::wstring normalized_prefix;
};

struct SearchResult {
    Frn frn = 0;
    std::wstring name;
    std::wstring path;
};

}  // namespace fsengine

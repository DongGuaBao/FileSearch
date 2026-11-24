#pragma once

#include "string_pool.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fsengine {

    struct MftSnapshotVolume {
        wchar_t drive = L'\0';
        std::wstring volume_root;
        std::uint64_t usn_journal_id = 0;
        std::int64_t next_usn = 0;
    };

    struct MftEntryRecord {
        Frn frn = 0;
        Frn parent_frn = kRootParentFrn;
        StringRef name_ref;
        bool is_directory = false;
    };

    struct MftSnapshot {
        // Legacy single-volume metadata. Kept for reading old cache files.
        std::wstring volume_root;
        std::uint64_t mutation_version = 0;
        std::uint64_t usn_journal_id = 0;
        std::int64_t next_usn = 0;
        std::vector<MftSnapshotVolume> volumes;
        StringPool names;
        std::vector<MftEntryRecord> entries;

        std::string_view Name(const MftEntryRecord& entry) const {
            return names.View(entry.name_ref);
        }

        std::string NameString(const MftEntryRecord& entry) const {
            return names.ToString(entry.name_ref);
        }
    };
    bool SaveMftSnapshotFile(const MftSnapshot& snapshot,
                         const std::wstring& path,
                         std::wstring* error);
    bool LoadMftSnapshotFile(const std::wstring& path,
                             MftSnapshot* snapshot,
                             std::wstring* error);

    // 导出 FRN 分布：排序 → 等距采样 sample_count 个 → 16进制补零到64bit → 写入文本文件
    bool ExportFrnDistribution(const std::wstring& snapshot_path,
                               const std::wstring& output_path,
                               std::size_t sample_count,
                               std::wstring* error);
}


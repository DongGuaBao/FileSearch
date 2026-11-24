#pragma once

#include "mft_snapshot.h"

#include <Windows.h>
#include <winioctl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fsengine {

struct EnumerateMftResult {
    bool ok = false;
    std::wstring error;
    MftSnapshot snapshot;
};

struct UsnReadCursor {
    std::int64_t next_usn = 0;
};

struct UsnReadBatch {
    bool ok = false;
    bool empty = false;
    bool cancelled = false;
    std::wstring error;
    std::int64_t next_usn = 0;
    std::vector<IndexChange> changes;
    struct Record {
        RawEntry entry;
        std::uint32_t reason = 0;
    };
    std::vector<Record> records;
};

std::wstring LastWin32ErrorMessage(const wchar_t* prefix, DWORD error = GetLastError());
bool EnsureUsnJournal(HANDLE volume, USN_JOURNAL_DATA_V0* journal, std::wstring* error);
bool QueryCurrentJournal(wchar_t drive, USN_JOURNAL_DATA_V0* journal, std::wstring* error);
bool ParseMftEnumRecord(const BYTE* record_ptr, RawEntry* out);
bool ParseUsnChangeRecord(const BYTE* record_ptr, RawEntry* out, std::uint32_t* reason);
EnumerateMftResult EnumerateMftVolume(wchar_t drive_letter);
UsnReadBatch ReadUsnChanges(HANDLE volume,
                            std::uint64_t journal_id,
                            UsnReadCursor* cursor);

}  // namespace fsengine

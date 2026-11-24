#include "usn_utils.h"

#include "utf8.h"
#include "win_handle.h"

#include <cstddef>
#include <cwctype>
#include <limits>
#include <sstream>
#include <vector>

namespace fsengine {

namespace {

constexpr DWORD kMftEnumBufferSize = 16 * 1024 * 1024;
constexpr DWORD kUsnReadBufferSize = 1024 * 1024;

std::uint64_t FileId128ToUInt64(const FILE_ID_128& id) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | id.Identifier[i];
    }
    return value;
}

}  // namespace

std::wstring LastWin32ErrorMessage(const wchar_t* prefix, DWORD error) {
    LPWSTR message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstringstream stream;
    stream << prefix << L" Error " << error;
    if (length != 0 && message != nullptr) {
        stream << L": " << message;
        LocalFree(message);
    }
    return stream.str();
}

bool EnsureUsnJournal(HANDLE volume, USN_JOURNAL_DATA_V0* journal, std::wstring* error) {
    DWORD bytes_returned = 0;
    if (DeviceIoControl(volume,
                        FSCTL_QUERY_USN_JOURNAL,
                        nullptr,
                        0,
                        journal,
                        sizeof(*journal),
                        &bytes_returned,
                        nullptr)) {
        return true;
    }

    CREATE_USN_JOURNAL_DATA create_data = {};
    create_data.MaximumSize = 128ull * 1024ull * 1024ull;
    create_data.AllocationDelta = 16ull * 1024ull * 1024ull;
    if (!DeviceIoControl(volume,
                         FSCTL_CREATE_USN_JOURNAL,
                         &create_data,
                         sizeof(create_data),
                         nullptr,
                         0,
                         &bytes_returned,
                         nullptr)) {
        if (error != nullptr) {
            *error = LastWin32ErrorMessage(L"Failed to create USN journal.");
        }
        return false;
    }

    if (!DeviceIoControl(volume,
                         FSCTL_QUERY_USN_JOURNAL,
                         nullptr,
                         0,
                         journal,
                         sizeof(*journal),
                         &bytes_returned,
                         nullptr)) {
        if (error != nullptr) {
            *error = LastWin32ErrorMessage(L"Failed to query USN journal after creation.");
        }
        return false;
    }

    return true;
}

bool QueryCurrentJournal(wchar_t drive, USN_JOURNAL_DATA_V0* journal, std::wstring* error) {
    const std::wstring volume_path = std::wstring(L"\\\\.\\") + drive + L":";
    UniqueHandle volume(CreateFileW(volume_path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_READONLY,
                                    nullptr));
    if (!volume.Valid()) {
        if (error != nullptr) {
            *error = LastWin32ErrorMessage(L"Failed to open volume.");
        }
        return false;
    }
    return EnsureUsnJournal(volume.Get(), journal, error);
}

bool ParseMftEnumRecord(const BYTE* record_ptr, RawEntry* out) {
    const auto* header = reinterpret_cast<const USN_RECORD*>(record_ptr);
    if (header->RecordLength < sizeof(USN_RECORD)) {
        return false;
    }

    if (header->MajorVersion == 2) {
        if (header->RecordLength < sizeof(USN_RECORD_V2)) {
            return false;
        }
        const auto* record = reinterpret_cast<const USN_RECORD_V2*>(record_ptr);
        if (record->FileNameOffset < offsetof(USN_RECORD_V2, FileName) ||
            record->FileNameOffset > record->RecordLength ||
            record->FileNameLength > record->RecordLength - record->FileNameOffset) {
            return false;
        }
        const auto* name = reinterpret_cast<const wchar_t*>(record_ptr + record->FileNameOffset);
        out->frn = static_cast<Frn>(record->FileReferenceNumber);
        out->parent_frn = static_cast<Frn>(record->ParentFileReferenceNumber);
        out->name.assign(name, record->FileNameLength / sizeof(wchar_t));
        out->is_directory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }

#if defined(USN_RECORD_V3)
    if (header->MajorVersion == 3) {
        if (header->RecordLength < sizeof(USN_RECORD_V3)) {
            return false;
        }
        const auto* record = reinterpret_cast<const USN_RECORD_V3*>(record_ptr);
        if (record->FileNameOffset < offsetof(USN_RECORD_V3, FileName) ||
            record->FileNameOffset > record->RecordLength ||
            record->FileNameLength > record->RecordLength - record->FileNameOffset) {
            return false;
        }
        const auto* name = reinterpret_cast<const wchar_t*>(record_ptr + record->FileNameOffset);
        out->frn = FileId128ToUInt64(record->FileReferenceNumber);
        out->parent_frn = FileId128ToUInt64(record->ParentFileReferenceNumber);
        out->name.assign(name, record->FileNameLength / sizeof(wchar_t));
        out->is_directory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }
#endif

    return false;
}

bool ParseUsnChangeRecord(const BYTE* record_ptr, RawEntry* out, std::uint32_t* reason) {
    if (!ParseMftEnumRecord(record_ptr, out)) {
        return false;
    }
    const auto* header = reinterpret_cast<const USN_RECORD*>(record_ptr);
    if (header->MajorVersion == 2) {
        *reason = reinterpret_cast<const USN_RECORD_V2*>(record_ptr)->Reason;
        return true;
    }
#if defined(USN_RECORD_V3)
    if (header->MajorVersion == 3) {
        *reason = reinterpret_cast<const USN_RECORD_V3*>(record_ptr)->Reason;
        return true;
    }
#endif
    return false;
}

EnumerateMftResult EnumerateMftVolume(wchar_t drive_letter) {
    EnumerateMftResult result;
    drive_letter = static_cast<wchar_t>(std::towupper(drive_letter));
    if (drive_letter < L'A' || drive_letter > L'Z') {
        result.error = L"Drive letter must be A-Z.";
        return result;
    }

    const std::wstring volume_path = std::wstring(L"\\\\.\\") + drive_letter + L":";
    UniqueHandle volume(CreateFileW(
        volume_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_READONLY,
        nullptr));

    if (!volume.Valid()) {
        result.error = LastWin32ErrorMessage(L"Failed to open volume.");
        return result;
    }

    USN_JOURNAL_DATA_V0 journal = {};
    if (!EnsureUsnJournal(volume.Get(), &journal, &result.error)) {
        return result;
    }

    result.snapshot.volume_root = std::wstring(1, drive_letter) + L":";

    MFT_ENUM_DATA_V0 enum_data = {};
    enum_data.StartFileReferenceNumber = 0;
    enum_data.LowUsn = 0;
    // Enumerate every live MFT record. NextUsn belongs to the current journal
    // generation and can be lower than USNs stored in records created before a
    // journal reset, which would silently omit old files and directories.
    enum_data.HighUsn = (std::numeric_limits<USN>::max)();

    std::vector<BYTE> buffer(kMftEnumBufferSize);
    DWORD last_error = ERROR_SUCCESS;
    DWORD last_bytes_returned = 0;
    std::uint64_t ioctl_calls = 0;
    std::uint64_t total_records = 0;
    std::uint64_t parsed_records = 0;
    std::uint64_t invalid_records = 0;
    std::uint16_t first_major = 0;
    std::uint16_t first_minor = 0;

    for (;;) {
        DWORD bytes_returned = 0;
        const BOOL ok = DeviceIoControl(volume.Get(),
                                        FSCTL_ENUM_USN_DATA,
                                        &enum_data,
                                        sizeof(enum_data),
                                        buffer.data(),
                                        static_cast<DWORD>(buffer.size()),
                                        &bytes_returned,
                                        nullptr);
        last_bytes_returned = bytes_returned;
        if (!ok) {
            last_error = GetLastError();
            break;
        }
        ++ioctl_calls;
        last_error = ERROR_SUCCESS;
        if (bytes_returned <= sizeof(DWORDLONG)) {
            break;
        }

        enum_data.StartFileReferenceNumber = *reinterpret_cast<const DWORDLONG*>(buffer.data());
        BYTE* cursor = buffer.data() + sizeof(DWORDLONG);
        BYTE* end = buffer.data() + bytes_returned;

        while (cursor < end) {
            const auto* header = reinterpret_cast<const USN_RECORD*>(cursor);
            if (header->RecordLength == 0 || cursor + header->RecordLength > end) {
                break;
            }
            ++total_records;
            if (first_major == 0) {
                first_major = header->MajorVersion;
                first_minor = header->MinorVersion;
            }

            RawEntry entry;
            if (ParseMftEnumRecord(cursor, &entry) && !entry.name.empty()) {
                if (entry.is_directory && (entry.frn == entry.parent_frn || entry.name == L".")) {
                    entry.parent_frn = kRootParentFrn;
                    entry.name = std::wstring(1, drive_letter) + L":";
                }
                const StringRef name_ref = result.snapshot.names.Add(WideToLowerUtf8(entry.name));
                result.snapshot.entries.push_back(MftEntryRecord{
                    entry.frn,
                    entry.parent_frn,
                    name_ref,
                    entry.is_directory,
                });
                ++parsed_records;
            } else {
                ++invalid_records;
            }

            cursor += header->RecordLength;
        }
    }

    if (last_error != ERROR_HANDLE_EOF && last_error != ERROR_SUCCESS) {
        result.error = LastWin32ErrorMessage(L"Failed while enumerating MFT.", last_error);
        return result;
    }
    if (parsed_records == 0) {
        std::wstringstream message;
        message << L"MFT enumeration parsed no usable records."
                << L" ioctl_calls=" << ioctl_calls
                << L" total_records=" << total_records
                << L" invalid_records=" << invalid_records
                << L" first_major=" << first_major
                << L" first_minor=" << first_minor
                << L" last_bytes=" << last_bytes_returned
                << L" last_error=" << last_error;
        result.error = message.str();
        return result;
    }

    result.snapshot.mutation_version = parsed_records + 1;
    result.snapshot.usn_journal_id = journal.UsnJournalID;
    result.snapshot.next_usn = journal.NextUsn;
    result.snapshot.volumes.push_back(MftSnapshotVolume{
        drive_letter,
        result.snapshot.volume_root,
        journal.UsnJournalID,
        journal.NextUsn,
    });
    result.ok = true;
    return result;
}

UsnReadBatch ReadUsnChanges(HANDLE volume,
                            std::uint64_t journal_id,
                            UsnReadCursor* cursor) {
    UsnReadBatch result;
    if (cursor == nullptr) {
        result.error = L"USN read cursor is null.";
        return result;
    }

    READ_USN_JOURNAL_DATA_V0 read_data = {};
    read_data.StartUsn = cursor->next_usn > 0 ? static_cast<USN>(cursor->next_usn) : 0;
    read_data.ReasonMask = 0xFFFFFFFF;
    read_data.ReturnOnlyOnClose = FALSE;
    read_data.Timeout = 0;
    read_data.BytesToWaitFor = 0;
    read_data.UsnJournalID = journal_id;

    std::vector<BYTE> buffer(kUsnReadBufferSize);
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(volume,
                                    FSCTL_READ_USN_JOURNAL,
                                    &read_data,
                                    sizeof(read_data),
                                    buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    &bytes_returned,
                                    nullptr);
    if (!ok) {
        const DWORD error_code = GetLastError();
        result.cancelled = error_code == ERROR_OPERATION_ABORTED || error_code == ERROR_INVALID_HANDLE;
        if (!result.cancelled) {
            result.error = LastWin32ErrorMessage(L"Failed while reading USN journal.", error_code);
        }
        return result;
    }

    if (bytes_returned <= sizeof(USN)) {
        result.ok = true;
        result.empty = true;
        result.next_usn = cursor->next_usn;
        return result;
    }

    const USN next_usn = *reinterpret_cast<USN*>(buffer.data());
    cursor->next_usn = static_cast<std::int64_t>(next_usn);
    result.next_usn = cursor->next_usn;

    BYTE* record_cursor = buffer.data() + sizeof(USN);
    BYTE* end = buffer.data() + bytes_returned;
    while (record_cursor < end) {
        const auto* header = reinterpret_cast<const USN_RECORD*>(record_cursor);
        if (header->RecordLength == 0 || record_cursor + header->RecordLength > end) {
            break;
        }
        RawEntry entry;
        std::uint32_t reason = 0;
        if (ParseUsnChangeRecord(record_cursor, &entry, &reason) && !entry.name.empty()) {
            result.records.push_back(UsnReadBatch::Record{entry, reason});
            const bool old_name = (reason & USN_REASON_RENAME_OLD_NAME) != 0;
            const bool remove = old_name || (reason & USN_REASON_FILE_DELETE) != 0;
            if (remove ||
                (reason & (USN_REASON_FILE_CREATE |
                           USN_REASON_RENAME_NEW_NAME |
                           USN_REASON_BASIC_INFO_CHANGE |
                           USN_REASON_DATA_EXTEND |
                           USN_REASON_DATA_OVERWRITE |
                           USN_REASON_DATA_TRUNCATION)) != 0) {
                result.changes.push_back(IndexChange{entry, remove});
            }
        }
        record_cursor += header->RecordLength;
    }

    result.ok = true;
    result.empty = result.changes.empty();
    return result;
}

}  // namespace fsengine

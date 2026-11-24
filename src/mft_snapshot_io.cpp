#include "mft_snapshot.h"
#include "file_id_codec.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace fsengine {

namespace {

constexpr char kMftSnapshotMagic[8] = {'F', 'S', 'E', 'M', 'F', 'T', '1', '\0'};
constexpr std::uint32_t kMftSnapshotFormatVersionV1 = 1;
constexpr std::uint32_t kMftSnapshotFormatVersionV2 = 2;
constexpr std::uint32_t kMftSnapshotFormatVersionV3 = 3;
constexpr std::uint32_t kMftSnapshotFormatVersionV4 = 4;
constexpr std::uint32_t kMftSnapshotFormatVersionV5 = 5;
constexpr std::uint32_t kMftSnapshotFormatVersionV6 = 6;
constexpr std::uint32_t kMftSnapshotFormatVersion = 7;

struct PackedFrnHeader {
    std::uint8_t record_bits = 0;
    std::uint8_t drive_bits = 0;
    std::uint8_t sequence_bits = 0;
    std::uint8_t reserved = 0;
};

void SetError(std::wstring* error, const wchar_t* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
bool WritePod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return out.good();
}

template <typename T>
bool ReadPod(std::ifstream& in, T* value) {
    in.read(reinterpret_cast<char*>(value), sizeof(T));
    return in.good();
}

bool WriteString(std::ofstream& out, const std::wstring& value) {
    const std::uint64_t size = static_cast<std::uint64_t>(value.size());
    if (!WritePod(out, size)) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(size * sizeof(wchar_t)));
    return out.good();
}

bool ReadString(std::ifstream& in, std::wstring* value) {
    std::uint64_t size = 0;
    if (!ReadPod(in, &size) || size > 32768) {
        return false;
    }
    value->assign(static_cast<std::size_t>(size), L'\0');
    in.read(reinterpret_cast<char*>(value->data()), static_cast<std::streamsize>(size * sizeof(wchar_t)));
    return in.good();
}

bool WriteBytes(std::ofstream& out, const std::string& value) {
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    if (!WritePod(out, size)) {
        return false;
    }
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    return out.good();
}

bool WriteRawBytes(std::ofstream& out, const char* data, std::uint64_t size) {
    if (!WritePod(out, size)) {
        return false;
    }
    if (size != 0) {
        out.write(data, static_cast<std::streamsize>(size));
    }
    return out.good();
}

bool ReadBytes(std::ifstream& in, std::string* value) {
    std::uint32_t size = 0;
    if (!ReadPod(in, &size) || size > 131072) {
        return false;
    }
    value->assign(size, '\0');
    in.read(value->data(), static_cast<std::streamsize>(size));
    return in.good();
}

bool ReadRawBytes(std::ifstream& in, std::vector<char>* value, std::uint64_t max_size) {
    std::uint64_t size = 0;
    if (!ReadPod(in, &size) || size > max_size) {
        return false;
    }
    value->assign(static_cast<std::size_t>(size), '\0');
    if (size != 0) {
        in.read(value->data(), static_cast<std::streamsize>(size));
    }
    return in.good();
}

template <typename T>
bool WriteArray(std::ofstream& out, const std::vector<T>& values) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(values.size()) * sizeof(T);
    return WriteRawBytes(out,
                         values.empty() ? nullptr : reinterpret_cast<const char*>(values.data()),
                         bytes);
}

std::uint8_t BitsRequired(std::uint64_t value) {
    std::uint8_t bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1u;
    }
    return bits;
}

void WritePackedBits(std::vector<std::uint8_t>* output,
                     std::uint64_t* bit_offset,
                     std::uint64_t value,
                     std::uint8_t width) {
    while (width != 0) {
        const std::size_t byte_index = static_cast<std::size_t>(*bit_offset / 8u);
        const std::uint8_t bit_index = static_cast<std::uint8_t>(*bit_offset % 8u);
        const std::uint8_t take = static_cast<std::uint8_t>((std::min)(static_cast<unsigned>(width), 8u - bit_index));
        const std::uint64_t mask = (std::uint64_t{1} << take) - 1u;
        (*output)[byte_index] |= static_cast<std::uint8_t>((value & mask) << bit_index);
        value >>= take;
        width -= take;
        *bit_offset += take;
    }
}

std::uint64_t ReadPackedBits(const std::uint8_t* input,
                             std::uint64_t* bit_offset,
                             std::uint8_t width) {
    std::uint64_t value = 0;
    std::uint8_t value_offset = 0;
    while (width != 0) {
        const std::size_t byte_index = static_cast<std::size_t>(*bit_offset / 8u);
        const std::uint8_t bit_index = static_cast<std::uint8_t>(*bit_offset % 8u);
        const std::uint8_t take = static_cast<std::uint8_t>((std::min)(static_cast<unsigned>(width), 8u - bit_index));
        const std::uint8_t mask = static_cast<std::uint8_t>((1u << take) - 1u);
        value |= static_cast<std::uint64_t>((input[byte_index] >> bit_index) & mask) << value_offset;
        value_offset += take;
        width -= take;
        *bit_offset += take;
    }
    return value;
}

bool PackFrnColumn(const std::vector<Frn>& values,
                   PackedFrnHeader* header,
                   std::vector<std::uint8_t>* packed) {
    std::uint64_t max_record = 0;
    std::uint64_t max_drive = 0;
    std::uint64_t max_sequence = 0;
    for (Frn value : values) {
        if ((value & ~(kFrnSequenceMask | kEncodedFrnDriveMask | kEncodedFrnRecordMask)) != 0) {
            return false;
        }
        max_record = (std::max)(max_record, value & kEncodedFrnRecordMask);
        max_drive = (std::max)(max_drive, (value & kEncodedFrnDriveMask) >> kEncodedFrnDriveShift);
        max_sequence = (std::max)(max_sequence, value >> 48u);
    }
    header->record_bits = BitsRequired(max_record);
    header->drive_bits = BitsRequired(max_drive);
    header->sequence_bits = BitsRequired(max_sequence);
    const std::uint64_t bits_per_value = header->record_bits + header->drive_bits + header->sequence_bits;
    const std::uint64_t total_bits = bits_per_value * values.size();
    packed->assign(static_cast<std::size_t>((total_bits + 7u) / 8u), 0);
    std::uint64_t bit_offset = 0;
    for (Frn value : values) {
        WritePackedBits(packed, &bit_offset, value & kEncodedFrnRecordMask, header->record_bits);
        WritePackedBits(packed, &bit_offset,
                        (value & kEncodedFrnDriveMask) >> kEncodedFrnDriveShift,
                        header->drive_bits);
        WritePackedBits(packed, &bit_offset, value >> 48u, header->sequence_bits);
    }
    return true;
}

bool UnpackFrnColumn(const PackedFrnHeader& header,
                     const std::vector<char>& packed,
                     std::size_t count,
                     std::vector<Frn>* values) {
    if (header.record_bits > 41 || header.drive_bits > 6 || header.sequence_bits > 16) {
        return false;
    }
    const std::uint64_t bits_per_value = header.record_bits + header.drive_bits + header.sequence_bits;
    const std::uint64_t expected_bytes = (bits_per_value * count + 7u) / 8u;
    if (packed.size() != expected_bytes) {
        return false;
    }
    values->resize(count);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(packed.data());
    std::uint64_t bit_offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t record = ReadPackedBits(bytes, &bit_offset, header.record_bits);
        const std::uint64_t drive = ReadPackedBits(bytes, &bit_offset, header.drive_bits);
        const std::uint64_t sequence = ReadPackedBits(bytes, &bit_offset, header.sequence_bits);
        (*values)[i] = record | (drive << kEncodedFrnDriveShift) | (sequence << 48u);
    }
    return true;
}

}  // namespace

bool SaveMftSnapshotFile(const MftSnapshot& snapshot,
                         const std::wstring& path,
                         std::wstring* error) {
    try {
        const std::filesystem::path cache_path(path);
        if (!cache_path.parent_path().empty()) {
            std::filesystem::create_directories(cache_path.parent_path());
        }
    } catch (...) {
        SetError(error, L"Failed to create MFT snapshot directory.");
        return false;
    }

    const std::filesystem::path target_path(path);
    std::filesystem::path temp_path = target_path;
    temp_path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(GetCurrentThreadId()) +
                 L"-" + std::to_wstring(GetTickCount64());
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        SetError(error, L"Failed to open MFT snapshot for writing.");
        return false;
    }

    out.write(kMftSnapshotMagic, sizeof(kMftSnapshotMagic));
    const std::uint64_t count = static_cast<std::uint64_t>(snapshot.entries.size());
    if (!WritePod(out, kMftSnapshotFormatVersion) ||
        !WritePod(out, snapshot.mutation_version) ||
        !WritePod(out, std::uint64_t{0}) ||
        !WritePod(out, std::int64_t{0}) ||
        !WriteString(out, std::wstring{}) ||
        !WritePod(out, count) ||
        !WriteRawBytes(out, snapshot.names.Data(), static_cast<std::uint64_t>(snapshot.names.CharCount()))) {
        SetError(error, L"Failed to write MFT snapshot header.");
        return false;
    }

    const std::uint64_t volume_count = static_cast<std::uint64_t>(snapshot.volumes.size());
    if (!WritePod(out, volume_count)) {
        SetError(error, L"Failed to write MFT snapshot volumes.");
        return false;
    }
    for (const MftSnapshotVolume& volume : snapshot.volumes) {
        if (!WritePod(out, volume.drive) ||
            !WriteString(out, volume.volume_root) ||
            !WritePod(out, volume.usn_journal_id) ||
            !WritePod(out, volume.next_usn)) {
            SetError(error, L"Failed to write MFT snapshot volume.");
            return false;
        }
    }

    std::vector<Frn> frns;
    std::vector<Frn> parent_frns;
    std::vector<std::uint32_t> name_offsets;
    std::vector<std::uint8_t> directory_bits((snapshot.entries.size() + 7u) / 8u, 0);
    if (snapshot.names.CharCount() > (std::numeric_limits<std::uint32_t>::max)()) {
        SetError(error, L"MFT snapshot string pool exceeds the supported size.");
        return false;
    }
    frns.reserve(snapshot.entries.size());
    parent_frns.reserve(snapshot.entries.size());
    name_offsets.reserve(snapshot.entries.size() + 1u);
    std::uint32_t expected_offset = 0;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        const MftEntryRecord& entry = snapshot.entries[i];
        const std::uint64_t name_end = static_cast<std::uint64_t>(entry.name_ref.offset) + entry.name_ref.length;
        if (entry.name_ref.offset != expected_offset || name_end > snapshot.names.CharCount()) {
            SetError(error, L"MFT snapshot string pool is not contiguous.");
            return false;
        }
        frns.push_back(entry.frn);
        parent_frns.push_back(entry.parent_frn);
        name_offsets.push_back(entry.name_ref.offset);
        expected_offset += entry.name_ref.length;
        if (entry.is_directory) {
            directory_bits[i / 8u] |= static_cast<std::uint8_t>(1u << (i % 8u));
        }
    }
    if (expected_offset != snapshot.names.CharCount()) {
        SetError(error, L"MFT snapshot string pool has unused bytes.");
        return false;
    }
    name_offsets.push_back(static_cast<std::uint32_t>(snapshot.names.CharCount()));
    PackedFrnHeader frn_header;
    PackedFrnHeader parent_header;
    std::vector<std::uint8_t> packed_frns;
    std::vector<std::uint8_t> packed_parents;
    if (!PackFrnColumn(frns, &frn_header, &packed_frns) ||
        !PackFrnColumn(parent_frns, &parent_header, &packed_parents)) {
        SetError(error, L"MFT snapshot contains an unsupported FRN value.");
        return false;
    }
    if (!WritePod(out, frn_header) || !WriteArray(out, packed_frns) ||
        !WritePod(out, parent_header) || !WriteArray(out, packed_parents) ||
        !WriteArray(out, name_offsets) || !WriteArray(out, directory_bits)) {
        SetError(error, L"Failed to write MFT entry columns.");
        return false;
    }
    out.flush();
    if (!out.good()) {
        SetError(error, L"Failed to flush MFT snapshot.");
        return false;
    }
    out.close();
    if (!MoveFileExW(temp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SetError(error, L"Failed to replace MFT snapshot.");
        return false;
    }
    return true;
}

bool LoadMftSnapshotFile(const std::wstring& path,
                         MftSnapshot* snapshot,
                         std::wstring* error) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        SetError(error, L"MFT snapshot does not exist.");
        return false;
    }

    char magic[sizeof(kMftSnapshotMagic)] = {};
    in.read(magic, sizeof(magic));
    std::uint32_t version = 0;
    MftSnapshot loaded;
    std::uint64_t count = 0;
    if (!in.good() || std::memcmp(magic, kMftSnapshotMagic, sizeof(kMftSnapshotMagic)) != 0 ||
        !ReadPod(in, &version) ||
        (version != kMftSnapshotFormatVersion &&
         version != kMftSnapshotFormatVersionV5 &&
         version != kMftSnapshotFormatVersionV4 &&
         version != kMftSnapshotFormatVersionV3 &&
         version != kMftSnapshotFormatVersionV2 &&
         version != kMftSnapshotFormatVersionV1) ||
        !ReadPod(in, &loaded.mutation_version) ||
        !ReadPod(in, &loaded.usn_journal_id) ||
        !ReadPod(in, &loaded.next_usn) ||
        !ReadString(in, &loaded.volume_root) ||
        !ReadPod(in, &count) ||
        count > 10000000ull) {
        SetError(error, L"MFT snapshot has an unsupported format.");
        return false;
    }

    if (version >= kMftSnapshotFormatVersionV2) {
        std::vector<char> raw_names;
        if (!ReadRawBytes(in, &raw_names, 8ull * 1024ull * 1024ull * 1024ull)) {
            SetError(error, L"Failed to read MFT string pool.");
            return false;
        }
        loaded.names.AssignRaw(raw_names.data(), raw_names.size());
    }

    if (version >= kMftSnapshotFormatVersionV3) {
        std::uint64_t volume_count = 0;
        if (!ReadPod(in, &volume_count) || volume_count > 64) {
            SetError(error, L"Failed to read MFT snapshot volumes.");
            return false;
        }
        loaded.volumes.reserve(static_cast<std::size_t>(volume_count));
        for (std::uint64_t i = 0; i < volume_count; ++i) {
            MftSnapshotVolume volume;
            if (!ReadPod(in, &volume.drive) ||
                !ReadString(in, &volume.volume_root) ||
                !ReadPod(in, &volume.usn_journal_id) ||
                !ReadPod(in, &volume.next_usn)) {
                SetError(error, L"Failed to read MFT snapshot volume.");
                return false;
            }
            loaded.volumes.push_back(std::move(volume));
        }
    } else if (!loaded.volume_root.empty()) {
        MftSnapshotVolume volume;
        volume.drive = loaded.volume_root[0];
        volume.volume_root = loaded.volume_root;
        volume.usn_journal_id = loaded.usn_journal_id;
        volume.next_usn = loaded.next_usn;
        loaded.volumes.push_back(std::move(volume));
    }

    loaded.entries.reserve(static_cast<std::size_t>(count));
    if (version == kMftSnapshotFormatVersion ||
        version == kMftSnapshotFormatVersionV6 ||
        version == kMftSnapshotFormatVersionV5) {
        std::vector<char> raw_frns;
        std::vector<char> raw_parents;
        std::vector<char> raw_offsets;
        std::vector<char> raw_directory_bits;
        const std::uint64_t offset_bytes = (count + 1ull) * sizeof(std::uint32_t);
        const std::uint64_t bit_bytes = (count + 7ull) / 8ull;
        std::vector<Frn> unpacked_frns;
        std::vector<Frn> unpacked_parents;
        if (version == kMftSnapshotFormatVersion || version == kMftSnapshotFormatVersionV6) {
            PackedFrnHeader frn_header;
            PackedFrnHeader parent_header;
            if (!ReadPod(in, &frn_header) ||
                !ReadRawBytes(in, &raw_frns, count * sizeof(Frn)) ||
                !ReadPod(in, &parent_header) ||
                !ReadRawBytes(in, &raw_parents, count * sizeof(Frn)) ||
                !UnpackFrnColumn(frn_header, raw_frns, static_cast<std::size_t>(count), &unpacked_frns) ||
                !UnpackFrnColumn(parent_header, raw_parents, static_cast<std::size_t>(count), &unpacked_parents)) {
                SetError(error, L"Failed to read packed MFT FRN columns.");
                return false;
            }
        } else {
            const std::uint64_t frn_bytes = count * sizeof(Frn);
            if (!ReadRawBytes(in, &raw_frns, frn_bytes) || raw_frns.size() != frn_bytes ||
                !ReadRawBytes(in, &raw_parents, frn_bytes) || raw_parents.size() != frn_bytes) {
                SetError(error, L"Failed to read MFT FRN columns.");
                return false;
            }
        }
        if (!ReadRawBytes(in, &raw_offsets, offset_bytes) || raw_offsets.size() != offset_bytes ||
            !ReadRawBytes(in, &raw_directory_bits, bit_bytes) || raw_directory_bits.size() != bit_bytes) {
            SetError(error, L"Failed to read MFT entry columns.");
            return false;
        }
        const auto* frns = (version == kMftSnapshotFormatVersion || version == kMftSnapshotFormatVersionV6)
            ? unpacked_frns.data()
            : reinterpret_cast<const Frn*>(raw_frns.data());
        const auto* parents = (version == kMftSnapshotFormatVersion || version == kMftSnapshotFormatVersionV6)
            ? unpacked_parents.data()
            : reinterpret_cast<const Frn*>(raw_parents.data());
        const auto* offsets = reinterpret_cast<const std::uint32_t*>(raw_offsets.data());
        const auto* directory_bits = reinterpret_cast<const std::uint8_t*>(raw_directory_bits.data());
        for (std::uint64_t i = 0; i < count; ++i) {
            if (offsets[i] > offsets[i + 1] || offsets[i + 1] > loaded.names.CharCount()) {
                SetError(error, L"MFT snapshot string reference is invalid.");
                return false;
            }
            MftEntryRecord entry;
            entry.frn = frns[i];
            entry.parent_frn = parents[i];
            entry.name_ref.offset = offsets[i];
            entry.name_ref.length = offsets[i + 1] - offsets[i];
            entry.is_directory = (directory_bits[i / 8u] & static_cast<std::uint8_t>(1u << (i % 8u))) != 0;
            loaded.entries.push_back(entry);
        }
    } else for (std::uint64_t i = 0; i < count; ++i) {
        MftEntryRecord entry;
        std::uint8_t flags = 0;
        if (version == kMftSnapshotFormatVersionV1) {
            std::string name;
            if (!ReadPod(in, &entry.frn) || !ReadPod(in, &entry.parent_frn) ||
                !ReadPod(in, &flags) || !ReadBytes(in, &name)) {
                SetError(error, L"Failed to read MFT entry.");
                return false;
            }
            entry.name_ref = loaded.names.Add(name);
        } else {
            if (!ReadPod(in, &entry.frn) || !ReadPod(in, &entry.parent_frn) ||
                !ReadPod(in, &entry.name_ref.offset) || !ReadPod(in, &entry.name_ref.length) ||
                !ReadPod(in, &flags)) {
                SetError(error, L"Failed to read MFT entry.");
                return false;
            }
            if (loaded.names.View(entry.name_ref).size() != entry.name_ref.length) {
                SetError(error, L"MFT snapshot string reference is invalid.");
                return false;
            }
        }
        entry.is_directory = (flags & 1u) != 0;
        loaded.entries.push_back(std::move(entry));
    }

    *snapshot = std::move(loaded);
    return true;
}

bool ExportFrnDistribution(const std::wstring& snapshot_path,
                           const std::wstring& output_path,
                           std::size_t sample_count,
                           std::wstring* error) {
    // 先读取 snapshot 文件，只提取 FRN
    MftSnapshot snapshot;
    if (!LoadMftSnapshotFile(snapshot_path, &snapshot, error)) {
        return false;
    }

    if (snapshot.entries.empty()) {
        SetError(error, L"Snapshot contains no entries to export.");
        return false;
    }

    const std::size_t total = snapshot.entries.size();

    // 拆分：高48-bit MFT record 和 低16-bit 序列号
    std::vector<uint64_t> mft_records;
    std::vector<uint16_t> seq_nums;
    mft_records.reserve(total);
    seq_nums.reserve(total);

    for (const MftEntryRecord& entry : snapshot.entries) {
        if (entry.frn == 0) continue;
        mft_records.push_back(static_cast<uint64_t>(entry.frn & 0xFFFFFFFFFFFFULL));
        seq_nums.push_back(static_cast<uint64_t>(entry.frn >> 48));
    }

    // 各自独立排序
    std::sort(mft_records.begin(), mft_records.end());
    std::sort(seq_nums.begin(), seq_nums.end());

    // 等距采样 lambda
    auto even_sample_48 = [](const std::vector<uint64_t>& data, std::size_t cnt) -> std::vector<uint64_t> {
        const std::size_t sz = data.size();
        const std::size_t k = (std::min)(cnt, sz);
        std::vector<uint64_t> r;
        r.reserve(k);
        if (k == 0) return r;
        if (k == 1) { r.push_back(data[sz / 2]); return r; }
        for (std::size_t i = 0; i < k; ++i)
            r.push_back(data[i * (sz - 1) / (k - 1)]);
        return r;
    };
    auto even_sample_16 = [](const std::vector<uint16_t>& data, std::size_t cnt) -> std::vector<uint16_t> {
        const std::size_t sz = data.size();
        const std::size_t k = (std::min)(cnt, sz);
        std::vector<uint16_t> r;
        r.reserve(k);
        if (k == 0) return r;
        if (k == 1) { r.push_back(data[sz / 2]); return r; }
        for (std::size_t i = 0; i < k; ++i)
            r.push_back(data[i * (sz - 1) / (k - 1)]);
        return r;
    };

    const std::size_t sm = (std::min)(sample_count, mft_records.size());
    const std::size_t ss = (std::min)(sample_count, seq_nums.size());
    const auto sampled_mft = even_sample_48(mft_records, sm);
    const auto sampled_seq = even_sample_16(seq_nums, ss);

    try {
        const std::filesystem::path out_path(output_path);
        if (!out_path.parent_path().empty()) {
            std::filesystem::create_directories(out_path.parent_path());
        }
    } catch (...) {
        SetError(error, L"Failed to create FRN export directory.");
        return false;
    }

    std::ofstream out(std::filesystem::path(output_path), std::ios::trunc);
    if (!out) {
        SetError(error, L"Failed to open FRN export file for writing.");
        return false;
    }
    out.imbue(std::locale::classic());
    out << std::hex << std::uppercase << std::setfill('0');
    const std::size_t n = (std::max)(sm, ss);
    for (std::size_t i = 0; i < n; ++i) {
        if (i < sm)
            out << std::setw(16) << snapshot.entries[i].frn << ' ' << std::setw(12) << sampled_mft[i];
        else
            out << std::string(12, ' ');
        out << " ";
        if (i < ss)
            out << std::setw(4) << sampled_seq[i];
        else
            out << std::string(4, ' ');
        out << "\n";
    }

    if (!out.good()) {
        SetError(error, L"Failed to write FRN export data.");
        return false;
    }

    return true;
}

}  // namespace fsengine

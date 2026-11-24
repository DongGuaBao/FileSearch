#include "mft_snapshot.h"
#include "file_id_codec.h"
#include "flat_map.h"
#include <iostream>
#include <map>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    fsengine::MftSnapshot snapshot;
    std::wstring error;
    if (!fsengine::LoadMftSnapshotFile(argv[1], &snapshot, &error)) { std::wcerr << error; return 1; }
    fsengine::FlatMap<fsengine::Frn, std::size_t> dirs;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i)
        if (snapshot.entries[i].is_directory) dirs[snapshot.entries[i].frn] = i;
    std::size_t missing = 0;
    std::map<fsengine::Frn, std::size_t> missing_parents;
    for (const auto& entry : snapshot.entries) {
        if ((fsengine::DecodeRawFrn(entry.frn) & fsengine::kFrnRecordMask) == 5)
            std::cout << "root frn=" << entry.frn << " parent=" << entry.parent_frn << " dir=" << entry.is_directory << " name=" << snapshot.Name(entry) << '\n';
        if (entry.parent_frn && dirs.find(entry.parent_frn) == dirs.end()) {
            ++missing_parents[entry.parent_frn];
            if (missing++ < 10) std::cout << "orphan frn=" << entry.frn << " parent=" << entry.parent_frn << " name=" << snapshot.Name(entry) << '\n';
        }
    }
    std::cout << "entries=" << snapshot.entries.size() << " orphan_parents=" << missing << '\n';
    for (const auto& [parent, count] : missing_parents)
        std::cout << "missing parent " << parent << " record=" << (fsengine::DecodeRawFrn(parent) & fsengine::kFrnRecordMask) << " children=" << count << '\n';
}

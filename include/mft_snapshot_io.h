#pragma once

#include "mft_snapshot.h"

#include <string>

namespace fsengine {

bool SaveMftSnapshotFile(const MftSnapshot& snapshot,
                         const std::wstring& path,
                         std::wstring* error);
bool LoadMftSnapshotFile(const std::wstring& path,
                         MftSnapshot* snapshot,
                         std::wstring* error);

bool ExportFrnDistribution(const std::wstring& snapshot_path,
                           const std::wstring& output_path,
                           std::size_t sample_count,
                           std::wstring* error);

}  // namespace fsengine

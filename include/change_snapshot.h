#pragma once
#include "mft_snapshot.h"
#include "flat_map.h"
#include <limits>
#include <stdexcept>

namespace fsengine {
// Mutates only an owned export copy, never the live Windows store/cache/WAL.
inline void ApplyChangeSnapshotDelta(MftSnapshot& snapshot, const std::vector<IndexChange>& changes) {
    FlatMap<Frn, std::size_t> indexes;
    indexes.reserve(snapshot.entries.size());
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        if (indexes.find(snapshot.entries[i].frn) != indexes.end())
            throw std::runtime_error("duplicate FRN in change snapshot");
        indexes[snapshot.entries[i].frn] = i;
    }
    for (const auto& change : changes) {
        const auto found = indexes.find(change.entry.frn);
        if (change.remove) {
            if (found == indexes.end()) continue;
            const auto index = found->value;
            indexes.erase(change.entry.frn);
            if (index + 1 != snapshot.entries.size()) {
                snapshot.entries[index] = snapshot.entries.back();
                indexes[snapshot.entries[index].frn] = index;
            }
            snapshot.entries.pop_back();
            continue;
        }
        if (!change.entry.frn || change.entry.name.empty()) continue;
        const auto name = WideToLowerUtf8(change.entry.name);
        if (snapshot.names.CharCount() > std::numeric_limits<std::uint32_t>::max() ||
            name.size() > std::numeric_limits<std::uint32_t>::max() - snapshot.names.CharCount())
            throw std::runtime_error("change snapshot string pool exceeds format capacity");
        MftEntryRecord entry{change.entry.frn, change.entry.parent_frn,
            snapshot.names.Add(name), change.entry.is_directory};
        if (found == indexes.end()) {
            indexes[entry.frn] = snapshot.entries.size();
            snapshot.entries.push_back(entry);
        } else snapshot.entries[found->value] = entry;
    }
}
}

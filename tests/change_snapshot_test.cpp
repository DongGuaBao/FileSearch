#include "change_snapshot.h"
#include <iostream>
#include <map>
#include <stdexcept>
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    using namespace fsengine;
    MftSnapshot baseline;
    baseline.mutation_version = 77;
    baseline.entries = {{1, 0, baseline.names.Add("root"), true},
        {2, 1, baseline.names.Add("original"), false},
        {3, 1, baseline.names.Add("last"), false}};
    auto exported = baseline;
    // Exercise deletion's swap-with-last, then mutate that swapped entry.
    ApplyChangeSnapshotDelta(exported, {{{2, 1, L"", false}, true},
        {{3, 1, L"RENAMED", true}, false}, {{4, 3, L"中文", false}, false},
        {{99, 1, L"missing", false}, true}, {{0, 1, L"invalid", false}, false}});
    Require(baseline.entries.size() == 3 && baseline.NameString(baseline.entries[1]) == "original", "live baseline was mutated");
    std::map<Frn, MftEntryRecord> entries;
    for (const auto& entry : exported.entries) entries[entry.frn] = entry;
    Require(entries.size() == 3 && !entries.count(2) && !entries.count(0), "delta identity set wrong");
    Require(exported.NameString(entries.at(3)) == "renamed" && entries.at(3).is_directory, "swapped row update lost metadata");
    Require(exported.NameString(entries.at(4)) == "中文" && entries.at(4).parent_frn == 3, "Unicode name or parent lost");
    Require(exported.mutation_version == 77, "export changed baseline version metadata");
    ApplyChangeSnapshotDelta(exported, {{{4, 3, L"", false}, true}, {{4, 1, L"RECYCLED", false}, false},
        {{3, 1, L"", true}, true}, {{4, 1, L"", false}, true}, {{1, 0, L"", true}, true}});
    Require(exported.entries.empty(), "delete/reinsert sequence retained entries");
    ApplyChangeSnapshotDelta(exported, {{{5, 0, L"NEW", true}, false}});
    Require(exported.entries.size() == 1 && exported.NameString(exported.entries[0]) == "new", "empty export insert failed");
    auto invalid = baseline;
    invalid.entries.push_back(invalid.entries[0]);
    bool rejected = false;
    try { ApplyChangeSnapshotDelta(invalid, {}); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "duplicate baseline identity accepted");
    std::cout << "change_snapshot_test passed\n";
}

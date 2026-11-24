#include "windows_snapshot_view.h"
#include <iostream>
using namespace fsengine;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    MftSnapshot snapshot;
    snapshot.volumes.push_back({L'D', L"D:"});
    auto id = [](Frn raw) { Frn encoded = 0; EncodeFrn(L'D', raw, &encoded); return encoded; };
    auto add = [&](Frn raw, Frn parent, const char* name, bool directory) {
        snapshot.entries.push_back({id(raw), parent ? id(parent) : 0, snapshot.names.Add(name), directory});
    };
    add(5, 0, "d:", true);
    add(6, 5, "folder", true);
    add(7, 6, "中文.txt", false);
    add(8, 5, "folder_other", true);
    add(9, 8, "not-in-scope.txt", false);
    WindowsSnapshotView view(snapshot);
    Require(view.Path(snapshot.entries[0]) == L"D:\\", "volume root duplicated");
    Require(view.Path(snapshot.entries[2]) == L"D:\\folder\\中文.txt", "Unicode path lost");
    view.SetScope(WindowsSnapshotView::NormalizeScope(L"d:/FOLDER/"));
    Require(view.Matches(snapshot.entries[1], 1) && view.Matches(snapshot.entries[2], 2), "scope excluded descendants");
    Require(!view.Matches(snapshot.entries[3], 3) && !view.Matches(snapshot.entries[4], 4), "scope boundary broken");
    view.SetScope(WindowsSnapshotView::NormalizeScope(L"D:\\folder\\中文.txt"));
    Require(view.Matches(snapshot.entries[2], 2) && !view.Matches(snapshot.entries[1], 1), "exact file scope broken");
    for (const auto* scope : {L"relative", L"D:\\folder\\..\\other", L"D:relative", L"D:\\folder\\\\other"}) {
        bool failed = false;
        try { WindowsSnapshotView::NormalizeScope(scope); } catch (const std::exception&) { failed = true; }
        Require(failed, "invalid scope accepted");
    }
    snapshot.entries[1].parent_frn = id(6);
    bool cycle = false;
    try { view.Path(snapshot.entries[2]); } catch (const std::exception&) { cycle = true; }
    Require(cycle, "cycle fabricated a path");
    snapshot.entries[1].parent_frn = id(100);
    bool orphan = false;
    try { view.Path(snapshot.entries[2]); } catch (const std::exception&) { orphan = true; }
    Require(orphan, "orphan fabricated a path");
    std::cout << "windows_snapshot_view_test passed\n";
}

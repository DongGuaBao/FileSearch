#pragma once
#include "mft_snapshot.h"
#include "file_id_codec.h"
#include "flat_map.h"
#include "utf8.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <vector>

namespace fsengine {
// References one immutable owned capture. Index only directories, and resolve
// paths on demand; never allocate a second full vector of file paths.
class WindowsSnapshotView {
public:
    explicit WindowsSnapshotView(const MftSnapshot& snapshot) : snapshot_(snapshot) {
        std::size_t count = 0;
        for (const auto& entry : snapshot.entries) count += entry.is_directory;
        directories_.reserve(count);
        for (std::size_t i = 0; i < snapshot.entries.size(); ++i)
            if (snapshot.entries[i].is_directory) directories_[snapshot.entries[i].frn] = i;
    }
    static std::wstring NormalizeScope(std::wstring scope) {
        std::replace(scope.begin(), scope.end(), L'/', L'\\');
        while (scope.size() > 3 && scope.back() == L'\\') scope.pop_back();
        if (scope.empty()) return scope;
        if (scope.size() < 2 || scope[1] != L':' || !DriveToCode(scope[0], nullptr) ||
            (scope.size() > 2 && scope[2] != L'\\') || scope.size() > 32767 ||
            scope.find(L'\0') != std::wstring::npos)
            throw std::runtime_error("scope_path must be an absolute drive path");
        if (scope.size() == 3) scope.pop_back();
        std::transform(scope.begin(), scope.end(), scope.begin(), [](wchar_t c) { return std::towlower(c); });
        std::size_t start = 3;
        while (start < scope.size()) {
            const auto end = scope.find(L'\\', start);
            const auto part = scope.substr(start, end == std::wstring::npos ? end : end - start);
            if (part.empty() || part == L"." || part == L"..")
                throw std::runtime_error("scope_path must be normalized");
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return scope;
    }
    static bool Within(const std::wstring& path, const std::wstring& scope) {
        if (scope.empty()) return true;
        auto normalized = path;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c) { return std::towlower(c); });
        return normalized == scope || (normalized.size() > scope.size() &&
            normalized.compare(0, scope.size(), scope) == 0 && normalized[scope.size()] == L'\\');
    }
    std::wstring Path(const MftEntryRecord& entry) const {
        const auto drive = DecodeFrnDrive(entry.frn);
        bool volume_found = false;
        for (const auto& volume : snapshot_.volumes) if (std::towupper(volume.drive) == drive) volume_found = true;
        if (!volume_found) throw std::runtime_error("snapshot entry has no ready volume");
        std::vector<const MftEntryRecord*> chain;
        const auto* current = &entry;
        while (true) {
            if (DecodeFrnDrive(current->frn) != drive)
                throw std::runtime_error("snapshot parent crosses volumes");
            if (std::find(chain.begin(), chain.end(), current) != chain.end() || chain.size() >= 32767)
                throw std::runtime_error("snapshot directory cycle");
            chain.push_back(current);
            if (current->parent_frn == kRootParentFrn) {
                const auto root_name = snapshot_.Name(*current);
                if (!current->is_directory || root_name.size() != 2 || root_name[1] != ':' ||
                    std::towupper(static_cast<unsigned char>(root_name[0])) != drive)
                    throw std::runtime_error("snapshot has no valid volume root ancestor");
                break;
            }
            const auto parent = directories_.find(current->parent_frn);
            if (parent == directories_.end()) throw std::runtime_error("snapshot has orphaned entry " +
                std::to_string(current->frn) + " parent " + std::to_string(current->parent_frn) + " name " + snapshot_.NameString(*current));
            current = &snapshot_.entries[parent->value];
        }
        std::wstring path(1, drive);
        path += L":\\";
        for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
            const auto name = Utf8ToWide(snapshot_.Name(**it));
            if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/\0", 0, 3) != std::wstring::npos)
                throw std::runtime_error("snapshot contains invalid entry name");
            if (path.back() != L'\\') path.push_back(L'\\');
            path += name;
            if (path.size() > 32767) throw std::runtime_error("snapshot path exceeds Windows limit");
        }
        return path;
    }
    // Scope-check directory ancestors once. Files are filtered using their
    // parent's bit before expensive full path construction.
    void SetScope(const std::wstring& normalized_scope) {
        scope_ = normalized_scope;
        if (scope_.empty()) return;
        included_directories_.assign(snapshot_.entries.size(), false);
        for (const auto& directory : directories_)
            if (DecodeFrnDrive(directory.key) == std::towupper(scope_[0]))
                included_directories_[directory.value] = Within(Path(snapshot_.entries[directory.value]), scope_);
    }
    bool Matches(const MftEntryRecord& entry, std::size_t index) const {
        if (scope_.empty()) return true;
        if (DecodeFrnDrive(entry.frn) != std::towupper(scope_[0])) return false;
        if (entry.is_directory) return included_directories_[index];
        const auto parent = directories_.find(entry.parent_frn);
        if (parent == directories_.end()) throw std::runtime_error("snapshot has orphaned entry");
        // An exact file scope may match even if its parent is outside scope.
        if (included_directories_[parent->value]) return true;
        const auto separator = scope_.find_last_of(L'\\');
        const auto basename = scope_.substr(separator == std::wstring::npos ? 0 : separator + 1);
        auto name = Utf8ToWide(snapshot_.Name(entry));
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
        return name == basename && Within(Path(entry), scope_);
    }
private:
    const MftSnapshot& snapshot_;
    FlatMap<Frn, std::size_t> directories_;
    std::wstring scope_;
    std::vector<bool> included_directories_;
};
}

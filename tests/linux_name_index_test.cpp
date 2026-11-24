#include "linux_name_index.h"
#include "utf8.h"
#include "filter.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int main() {
    Require(fsengine::WideToUtf8(fsengine::Utf8ToWide("中文😀")) == "中文😀", "UTF-8 roundtrip failed");
    Require(fsengine::Utf8ToWide(std::string("a\xff") + "b") == L"a\ufffdb", "invalid byte erased valid text");
    Require(fsengine::Utf8ToWide(std::string("\xed\xa0\x80")).find(static_cast<wchar_t>(0xd800)) == std::wstring::npos,
        "UTF-8 surrogate accepted");
    Require(fsengine::Utf8ToWide(std::string("\xf4\x90\x80\x80")).find(static_cast<wchar_t>(0x110000)) == std::wstring::npos,
        "out-of-range codepoint accepted");
    fsengine::LinuxNameIndex index;
    index.Upsert("/a", "中文文档.txt");
    index.Upsert("/b", "Report.TXT");
    index.Upsert("/hardlink", "Report.TXT");
    Require(index.Candidates("中文").count("/a"), "Chinese query failed");
    Require(index.Candidates("zhongwen").count("/a"), "pinyin query failed");
    Require(index.Candidates("zwwd").count("/a"), "initials query failed");
    Require(index.Candidates("REPORT").size() == 2, "case folding or hardlinks failed");
    Require(index.Candidates("r").size() == 2, "short query failed");
    index.Upsert("/b", "renamed.log");
    Require(index.Candidates("report").size() == 1, "upsert retained old terms");
    index.Erase("/hardlink");
    Require(index.Candidates("report").empty(), "delete retained postings");
    index.Upsert("/false-positive", "abc---bcd");
    Require(index.Candidates("abcd").empty(), "candidate verification failed");
    Require(index.Candidates("").empty(), "empty query failed");
    index.Upsert("/fuzzy-report", "Report.TXT");
    Require(!index.FuzzyCandidates("repotr", 2).empty(), "fuzzy typo not found");
    fsengine::LinuxNameIndex differential;
    std::vector<std::string> names;
    std::vector<std::string> paths;
    unsigned random = 12345;
    auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
    for (int i = 0; i < 100; ++i) {
        std::string name;
        for (int j = 0; j < 12; ++j) name += static_cast<char>('a' + (next() >> 16) % 4);
        names.push_back(name);
        paths.push_back((i % 2 ? "/scope_extra/" : "/scope/") + std::to_string(i));
        differential.Upsert(paths.back(), name);
    }
    for (int i = 0; i < 100; ++i) {
        std::string query;
        for (unsigned j = 0, size = 1 + (next() >> 16) % 10; j < size; ++j)
            query += static_cast<char>('a' + (next() >> 16) % 4);
        for (int distance = 0; distance <= 2; ++distance) {
            std::set<std::string> expected, actual;
            for (std::size_t n = 0; n < names.size(); ++n)
                if (fsengine::FuzzyDistance(names[n], query, distance) <= distance) expected.insert(paths[n]);
            for (const auto& [path, score] : differential.FuzzyCandidates(query, distance)) actual.insert(path);
            Require(actual == expected, "fuzzy candidate recall differs from exhaustive verification");
            const std::set<std::string> excluded{paths[0], paths[1]};
            for (const auto& scope : {std::string{}, std::string("/scope"), std::string("/scope_extra"), paths[2]}) {
                auto within = [&](const auto& path) {
                    return scope.empty() || path == scope || path.starts_with(scope + "/");
                };
                std::vector<std::pair<int, std::string>> ordered;
                std::set<std::string> exact;
                for (std::size_t n = 0; n < names.size(); ++n) {
                    if (!within(paths[n])) continue;
                    if (names[n].find(query) != std::string::npos) exact.insert(paths[n]);
                    const int score = fsengine::FuzzyDistance(names[n], query, distance);
                    if (score <= distance && !excluded.count(paths[n])) ordered.emplace_back(score, paths[n]);
                }
                std::sort(ordered.begin(), ordered.end());
                for (std::size_t limit : {0, 1, 7, 200}) {
                    auto exact_expected = exact;
                    while (exact_expected.size() > limit) exact_expected.erase(std::prev(exact_expected.end()));
                    Require(differential.Candidates(query, scope, limit) == exact_expected, "limited scoped exact order/membership differs");
                    std::vector<std::pair<std::string, int>> fuzzy_expected;
                    for (std::size_t n = 0; n < std::min(limit, ordered.size()); ++n)
                        fuzzy_expected.emplace_back(ordered[n].second, ordered[n].first);
                    Require(differential.FuzzyCandidates(query, distance, scope, limit, excluded) == fuzzy_expected,
                        "limited fuzzy score/order/scope/exclusion differs from exhaustive verification");
                }
            }
        }
    }
    std::cout << "linux_name_index_test passed\n";
}

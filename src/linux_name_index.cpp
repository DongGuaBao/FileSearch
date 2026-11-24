#include "linux_name_index.h"
#include "pinyin.h"
#include "utf8.h"
#include "filter.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <bit>
#include <limits>

namespace fsengine {
namespace {
std::string Normalize(const std::string& value) {
    return WideToLowerUtf8(Utf8ToWide(value));
}
bool Within(const std::string& path, const std::string& scope) {
    return scope.empty() || scope == "/" || path == scope ||
        (path.size() > scope.size() && path.compare(0, scope.size(), scope) == 0 && path[scope.size()] == '/');
}
std::set<std::string> Grams(const std::vector<std::string>& terms) {
    std::set<std::string> result;
    for (const auto& term : terms)
        for (std::size_t width = 1; width <= 3; ++width)
            for (std::size_t i = 0; i + width <= term.size(); ++i)
                result.insert(term.substr(i, width));
    return result;
}
}
void LinuxNameIndex::Posting::insert(std::uint64_t id) {
    auto& block = blocks_[id >> 6];
    const auto bit = std::uint64_t{1} << (id & 63);
    if (!(block & bit)) { block |= bit; ++size_; }
}
void LinuxNameIndex::Posting::erase(std::uint64_t id) {
    auto block = blocks_.find(id >> 6);
    if (block == blocks_.end()) return;
    const auto bit = std::uint64_t{1} << (id & 63);
    if (block->second & bit) { block->second &= ~bit; --size_; }
    if (!block->second) blocks_.erase(block);
}
bool LinuxNameIndex::Posting::count(std::uint64_t id) const {
    auto block = blocks_.find(id >> 6);
    return block != blocks_.end() && (block->second & (std::uint64_t{1} << (id & 63)));
}
std::vector<std::uint64_t> LinuxNameIndex::Posting::Values() const {
    std::vector<std::uint64_t> result; result.reserve(size_);
    Visit([&](auto id) { result.push_back(id); });
    return result;
}
void LinuxNameIndex::Posting::Visit(const std::function<void(std::uint64_t)>& visitor) const {
    for (const auto& [block, bits] : blocks_) {
        auto remaining = bits;
        while (remaining) {
            visitor((block << 6) + std::countr_zero(remaining));
            remaining &= remaining - 1;
        }
    }
}
void LinuxNameIndex::Erase(const std::string& path) {
    auto found = ids_.find(path);
    if (found == ids_.end()) return;
    const auto id = found->second;
    for (const auto& gram : Grams(names_.at(id).terms)) {
        auto posting = postings_.find(gram);
        if (posting == postings_.end()) continue;
        posting->second.erase(id);
        if (posting->second.empty()) postings_.erase(posting);
    }
    names_.erase(id);
    ids_.erase(found);
}
void LinuxNameIndex::Upsert(const std::string& path, const std::string& name) {
    Erase(path);
    auto wide = Utf8ToWide(name);
    std::vector<std::string> terms{Normalize(name), Normalize(ToPinyinFull(wide)), Normalize(ToPinyinInitials(wide))};
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    if (!next_id_) throw std::runtime_error("name index ID space exhausted");
    const auto id = next_id_++;
    for (const auto& gram : Grams(terms)) postings_[gram].insert(id);
    names_[id] = {path, std::move(terms)};
    ids_[path] = id;
}
std::set<std::string> LinuxNameIndex::Candidates(const std::string& query) const {
    return Candidates(query, {}, std::numeric_limits<std::size_t>::max());
}
std::set<std::string> LinuxNameIndex::Candidates(const std::string& query, const std::string& scope, std::size_t limit) const {
    std::set<std::string> result;
    if (!limit) return result;
    VisitExactIds(Normalize(query), [&](auto id) {
        const auto& path = names_.at(id).path;
        if (!Within(path, scope) || (result.size() == limit && path >= *result.rbegin())) return;
        result.insert(path);
        if (result.size() > limit) result.erase(std::prev(result.end()));
    });
    return result;
}
void LinuxNameIndex::VisitExactIds(const std::string& normalized, const std::function<void(std::uint64_t)>& visitor) const {
    if (normalized.empty()) return;
    const auto width = std::min<std::size_t>(3, normalized.size());
    std::vector<const Posting*> lists;
    for (std::size_t i = 0; i + width <= normalized.size(); ++i) {
        auto posting = postings_.find(normalized.substr(i, width));
        if (posting == postings_.end()) return;
        lists.push_back(&posting->second);
    }
    std::sort(lists.begin(), lists.end(), [](auto a, auto b) { return a->size() < b->size(); });
    lists.front()->Visit([&](auto id) {
        if (std::any_of(lists.begin()+1, lists.end(), [&](const auto* list) { return !list->count(id); })) return;
        const auto& terms = names_.at(id).terms;
        if (std::any_of(terms.begin(), terms.end(), [&](const auto& term) { return term.find(normalized) != std::string::npos; }))
            visitor(id);
    });
}
std::vector<std::pair<std::string, int>> LinuxNameIndex::FuzzyCandidates(const std::string& query, int max_distance) const {
    return FuzzyCandidates(query, max_distance, {}, std::numeric_limits<std::size_t>::max(), {});
}
std::vector<std::pair<std::string, int>> LinuxNameIndex::FuzzyCandidates(const std::string& query, int max_distance,
    const std::string& scope, std::size_t limit, const std::set<std::string>& excluded) const {
    std::set<std::pair<int, std::string>> selected;
    if (limit) VisitFuzzyMatches(query, max_distance, scope, excluded, [&](const auto& path, int distance) {
        if (selected.size() == limit && (distance > selected.rbegin()->first ||
            (distance == selected.rbegin()->first && path >= selected.rbegin()->second))) return;
        selected.emplace(distance, path);
        if (selected.size() > limit) selected.erase(std::prev(selected.end()));
    });
    std::vector<std::pair<std::string, int>> result;
    result.reserve(selected.size());
    for (const auto& [score, path] : selected) result.emplace_back(path, score);
    return result;
}
void LinuxNameIndex::VisitFuzzyMatches(const std::string& query, int max_distance, const std::string& scope,
    const std::set<std::string>& excluded, const std::function<void(const std::string&, int)>& visitor) const {
    const auto normalized = Normalize(query);
    if (normalized.empty() || max_distance < 0 || max_distance > 2) return;
    auto verify = [&](auto id) {
        const auto& name = names_.at(id);
        if (!Within(name.path, scope) || excluded.count(name.path)) return;
        int distance = max_distance + 1;
        for (const auto& term : name.terms)
            distance = std::min(distance, FuzzyDistance(term, normalized, max_distance));
        if (distance <= max_distance) visitor(name.path, distance);
    };
    std::vector<std::uint64_t> candidates;
    const auto parts = static_cast<std::size_t>(max_distance + 1);
    // At most k edits leave at least one of k+1 disjoint pieces untouched.
    // Look up those exact pieces before running edit-distance verification.
    if (normalized.size() > static_cast<std::size_t>(max_distance)) {
        for (std::size_t part = 0; part < parts; ++part) {
            const auto begin = normalized.size() * part / parts;
            const auto end = normalized.size() * (part + 1) / parts;
            VisitExactIds(normalized.substr(begin, end - begin), [&](auto id) { candidates.push_back(id); });
        }
    } else {
        for (const auto& [id, name] : names_) verify(id);
        return;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const auto id : candidates) verify(id);
}
}

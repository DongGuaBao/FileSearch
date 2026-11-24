#pragma once
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <functional>

namespace fsengine {
// Paths identify directory entries, so hard links remain distinct search results.
class LinuxNameIndex {
public:
    void Upsert(const std::string& path, const std::string& name);
    void Erase(const std::string& path);
    std::set<std::string> Candidates(const std::string& query) const;
    std::set<std::string> Candidates(const std::string& query, const std::string& scope, std::size_t limit) const;
    std::vector<std::pair<std::string, int>> FuzzyCandidates(const std::string& query, int max_distance) const;
    std::vector<std::pair<std::string, int>> FuzzyCandidates(const std::string& query, int max_distance,
        const std::string& scope, std::size_t limit, const std::set<std::string>& excluded) const;
private:
    void VisitExactIds(const std::string& normalized, const std::function<void(std::uint64_t)>& visitor) const;
    void VisitFuzzyMatches(const std::string& query, int max_distance, const std::string& scope,
        const std::set<std::string>& excluded, const std::function<void(const std::string&, int)>& visitor) const;
    struct IndexedName { std::string path; std::vector<std::string> terms; };
    // Sparse 64-ID blocks avoid allocating one tree node per posting.
    class Posting {
    public:
        void insert(std::uint64_t id);
        void erase(std::uint64_t id);
        bool count(std::uint64_t id) const;
        bool empty() const { return size_ == 0; }
        std::size_t size() const { return size_; }
        std::vector<std::uint64_t> Values() const;
        void Visit(const std::function<void(std::uint64_t)>& visitor) const;
    private:
        std::unordered_map<std::uint64_t, std::uint64_t> blocks_;
        std::size_t size_ = 0;
    };
    std::unordered_map<std::string, std::uint64_t> ids_;
    std::unordered_map<std::uint64_t, IndexedName> names_;
    std::unordered_map<std::string, Posting> postings_;
    std::uint64_t next_id_ = 1;
};
}

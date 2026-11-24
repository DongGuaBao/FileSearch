#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace fsengine {

template <typename K, typename V, typename Hash = std::hash<K>>
class FlatMap {
public:
    struct Entry {
        K key{};
        V value{};
        std::uint8_t state = 0;  // 0 empty, 1 occupied, 2 tombstone
    };

    template <typename EntryT, typename MapT>
    class IteratorBase {
    public:
        IteratorBase() = default;
        IteratorBase(MapT* map, std::size_t index) : map_(map), index_(index) {
            SkipEmpty();
        }

        EntryT& operator*() const { return map_->entries_[index_]; }
        EntryT* operator->() const { return &map_->entries_[index_]; }

        IteratorBase& operator++() {
            ++index_;
            SkipEmpty();
            return *this;
        }

        bool operator==(const IteratorBase& other) const {
            return map_ == other.map_ && index_ == other.index_;
        }

        bool operator!=(const IteratorBase& other) const {
            return !(*this == other);
        }

    private:
        void SkipEmpty() {
            if (map_ == nullptr) {
                return;
            }
            while (index_ < map_->entries_.size() && map_->entries_[index_].state != 1) {
                ++index_;
            }
        }

        MapT* map_ = nullptr;
        std::size_t index_ = 0;
    };

    using iterator = IteratorBase<Entry, FlatMap>;
    using const_iterator = IteratorBase<const Entry, const FlatMap>;

    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return entries_.size(); }
    std::size_t bytes_allocated() const { return entries_.capacity() * sizeof(Entry); }

    void clear() {
        entries_.clear();
        size_ = 0;
        tombstones_ = 0;
    }

    void release_memory() {
        std::vector<Entry>().swap(entries_);
        size_ = 0;
        tombstones_ = 0;
    }

    void reserve(std::size_t count) {
        const std::size_t wanted = CapacityFor(count);
        if (wanted > entries_.size()) {
            Rehash(wanted);
        }
    }

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, entries_.size()); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, entries_.size()); }

    iterator find(const K& key) {
        if (entries_.empty()) {
            return end();
        }
        const std::size_t index = FindIndex(key);
        if (index == npos) {
            return end();
        }
        return iterator(this, index);
    }

    const_iterator find(const K& key) const {
        if (entries_.empty()) {
            return end();
        }
        const std::size_t index = FindIndex(key);
        if (index == npos) {
            return end();
        }
        return const_iterator(this, index);
    }

    V& operator[](const K& key) {
        EnsureCapacityForInsert();
        std::size_t slot = FindSlotForInsert(key);
        Entry& entry = entries_[slot];
        if (entry.state != 1) {
            if (entry.state == 2) {
                --tombstones_;
            }
            entry.key = key;
            entry.value = V{};
            entry.state = 1;
            ++size_;
        }
        return entry.value;
    }

    void erase(const K& key) {
        const std::size_t index = FindIndex(key);
        if (index != npos) {
            EraseIndex(index);
        }
    }

    void erase(iterator it) {
        if (it == end()) {
            return;
        }
        const Entry* entry = &*it;
        const std::size_t index = static_cast<std::size_t>(entry - entries_.data());
        EraseIndex(index);
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    static constexpr std::size_t kMaxLoadNumerator = 85;
    static constexpr std::size_t kMaxLoadDenominator = 100;

    static std::size_t CapacityFor(std::size_t count) {
        std::size_t capacity = 16;
        while (capacity * kMaxLoadNumerator / kMaxLoadDenominator < count) {
            capacity *= 2;
        }
        return capacity;
    }

    void EnsureCapacityForInsert() {
        if (entries_.empty()) {
            Rehash(16);
            return;
        }
        if ((size_ + tombstones_ + 1) * kMaxLoadDenominator >= entries_.size() * kMaxLoadNumerator) {
            Rehash(CapacityFor(size_ + 1));
        }
    }

    std::size_t ProbeStart(const K& key) const {
        return hasher_(key) & (entries_.size() - 1);
    }

    std::size_t FindIndex(const K& key) const {
        std::size_t index = ProbeStart(key);
        for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
            const Entry& entry = entries_[index];
            if (entry.state == 0) {
                return npos;
            }
            if (entry.state == 1 && entry.key == key) {
                return index;
            }
            index = (index + 1) & (entries_.size() - 1);
        }
        return npos;
    }

    std::size_t FindSlotForInsert(const K& key) {
        std::size_t index = ProbeStart(key);
        std::size_t first_tombstone = npos;
        for (;;) {
            Entry& entry = entries_[index];
            if (entry.state == 1 && entry.key == key) {
                return index;
            }
            if (entry.state == 2 && first_tombstone == npos) {
                first_tombstone = index;
            }
            if (entry.state == 0) {
                return first_tombstone == npos ? index : first_tombstone;
            }
            index = (index + 1) & (entries_.size() - 1);
        }
    }

    void EraseIndex(std::size_t index) {
        entries_[index].value = V{};
        entries_[index].state = 2;
        --size_;
        ++tombstones_;
    }

    void Rehash(std::size_t new_capacity) {
        std::vector<Entry> old = std::move(entries_);
        entries_.clear();
        entries_.resize(new_capacity);
        size_ = 0;
        tombstones_ = 0;
        for (Entry& entry : old) {
            if (entry.state == 1) {
                EnsureCapacityForInsert();
                std::size_t slot = FindSlotForInsert(entry.key);
                entries_[slot].key = entry.key;
                entries_[slot].value = std::move(entry.value);
                entries_[slot].state = 1;
                ++size_;
            }
        }
    }

    std::vector<Entry> entries_;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
    Hash hasher_;
};

}  // namespace fsengine

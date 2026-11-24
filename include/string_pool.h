#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "utf8.h"

namespace fsengine {

struct StringRef {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

class StringPool {
public:
    void Clear() {
        data_.clear();
    }

    void ReleaseMemory() {
        std::vector<char>().swap(data_);
    }

    void ReserveAdditional(std::size_t chars) {
        data_.reserve(data_.size() + chars);
    }

    std::size_t CharCount() const {
        return data_.size();
    }

    const char* Data() const {
        return data_.empty() ? nullptr : data_.data();
    }

    void AssignRaw(const char* data, std::size_t size) {
        data_.assign(data, data + size);
    }

    const std::vector<char>& Raw() const {
        return data_;
    }

    StringRef Add(std::string_view value) {
        const std::size_t offset = data_.size();
        data_.insert(data_.end(), value.begin(), value.end());
        return StringRef{
            static_cast<std::uint32_t>(offset),
            static_cast<std::uint32_t>(value.size()),
        };
    }

    StringRef AddLower(std::wstring_view value) {
        return Add(WideToLowerUtf8(value));
    }

    std::string_view View(StringRef ref) const {
        if (ref.length == 0) {
            return {};
        }
        if (static_cast<std::size_t>(ref.offset) + ref.length > data_.size()) {
            return {};
        }
        return std::string_view(data_.data() + ref.offset, ref.length);
    }

    std::string ToString(StringRef ref) const {
        const std::string_view view = View(ref);
        return std::string(view.data(), view.size());
    }

    std::wstring ToWideString(StringRef ref) const {
        return Utf8ToWide(View(ref));
    }

private:
    std::vector<char> data_;
};

}  // namespace fsengine

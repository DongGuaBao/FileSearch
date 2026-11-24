#include "pinyin.h"

#include "pinyin_data.inc"

#include <algorithm>
#include <string>

namespace fsengine {

namespace {

bool IsCjk(wchar_t ch) {
    return (ch >= 0x4E00 && ch <= 0x9FFF) ||
           (ch >= 0x3400 && ch <= 0x4DBF) ||
           (ch >= 0xF900 && ch <= 0xFAFF);
}

const char* LookupPinyin(wchar_t ch) {
    // Binary search in the sorted pinyin range table
    const auto& ranges = pinyin_detail::kPinyinRanges;
    std::size_t lo = 0;
    std::size_t hi = pinyin_detail::kPinyinRangeCount;

    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const auto& r = ranges[mid];
        if (ch < r.begin) {
            hi = mid;
        } else if (ch > r.end) {
            lo = mid + 1;
        } else {
            return r.pinyin;  // ch is within this range
        }
    }
    return nullptr;  // not found
}

}  // namespace

std::string ToPinyinFull(std::wstring_view value) {
    std::string output;
    for (wchar_t ch : value) {
        if (ch >= 0 && ch <= 127) {
            output.push_back(static_cast<char>(ch));
            continue;
        }
        if (const char* pinyin = LookupPinyin(ch)) {
            output.append(pinyin);
        } else if (IsCjk(ch)) {
            output.push_back('?');
        }
        // Non-CJK, non-ASCII characters (e.g., symbols) are silently dropped
    }
    return output;
}

std::string ToPinyinInitials(std::wstring_view value) {
    std::string output;
    for (wchar_t ch : value) {
        if (ch >= 0 && ch <= 127) {
            output.push_back(static_cast<char>(ch));
            continue;
        }
        if (const char* pinyin = LookupPinyin(ch)) {
            output.push_back(pinyin[0]);
        } else if (IsCjk(ch)) {
            output.push_back('?');
        }
    }
    return output;
}

}  // namespace fsengine

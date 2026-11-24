#pragma once

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

#include <cwctype>
#include <string>
#include <string_view>

namespace fsengine {

inline std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
#if defined(_WIN32)
    const int size = WideCharToMultiByte(CP_UTF8,
                                         0,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size,
                        nullptr,
                        nullptr);
    return output;
#else
    std::string output;
    for (wchar_t character : value) {
        auto code = static_cast<unsigned int>(character);
        if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) code = 0xfffd;
        if (code <= 0x7f) output.push_back(static_cast<char>(code));
        else if (code <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (code >> 6)));
            output.push_back(static_cast<char>(0x80 | (code & 63)));
        } else if (code <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (code >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (code & 63)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (code >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (code & 63)));
        }
    }
    return output;
#endif
}

inline std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
#if defined(_WIN32)
    const int size = MultiByteToWideChar(CP_UTF8,
                                         MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size);
    return output;
#else
    std::wstring output;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (first < 0x80) { output.push_back(first); ++i; continue; }
        unsigned count = first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        unsigned code = count ? first & ((1u << (7 - count)) - 1) : 0;
        bool valid = count && i + count <= value.size();
        for (unsigned j = 1; valid && j < count; ++j) {
            const auto byte = static_cast<unsigned char>(value[i+j]);
            if ((byte & 0xc0) != 0x80) valid = false;
            else code = (code << 6) | (byte & 63);
        }
        if (valid && ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
            (count == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))) valid = false;
        output.push_back(static_cast<wchar_t>(valid ? code : 0xfffd));
        i += valid ? count : 1;
    }
    return output;
#endif
}

inline std::string WideToLowerUtf8(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (wchar_t ch : value) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return WideToUtf8(lowered);
}

}  // namespace fsengine

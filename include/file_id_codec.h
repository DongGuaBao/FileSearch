#pragma once

#include "types.h"

#include <cwctype>
#include <cstdint>

namespace fsengine {

constexpr std::uint64_t kFrnSequenceMask = 0xFFFF000000000000ull;
constexpr std::uint64_t kFrnRecordMask = 0x0000FFFFFFFFFFFFull;
constexpr std::uint64_t kEncodedFrnRecordMask = 0x000001FFFFFFFFFFull;
constexpr std::uint64_t kSearchSameNameNextMask = 0x0000020000000000ull;
constexpr std::uint64_t kEncodedFrnDriveShift = 42;
constexpr std::uint64_t kEncodedFrnDriveMask = 0x0000FC0000000000ull;

inline bool DriveToCode(wchar_t drive, std::uint8_t* code) {
    drive = static_cast<wchar_t>(std::towupper(drive));
    if (drive < L'A' || drive > L'Z') {
        return false;
    }
    if (code != nullptr) {
        *code = static_cast<std::uint8_t>(drive - L'A');
    }
    return true;
}

inline wchar_t CodeToDrive(std::uint8_t code) {
    return code < 26 ? static_cast<wchar_t>(L'A' + code) : L'\0';
}

inline bool EncodeFrn(wchar_t drive, Frn raw_frn, Frn* encoded) {
    if (raw_frn == kRootParentFrn) {
        if (encoded != nullptr) {
            *encoded = kRootParentFrn;
        }
        return true;
    }
    std::uint8_t code = 0;
    if (!DriveToCode(drive, &code)) {
        return false;
    }
    const std::uint64_t sequence = raw_frn & kFrnSequenceMask;
    const std::uint64_t record = raw_frn & kFrnRecordMask;
    if ((record & ~kEncodedFrnRecordMask) != 0) {
        return false;
    }
    if (encoded != nullptr) {
        *encoded = sequence |
                   (static_cast<std::uint64_t>(code) << kEncodedFrnDriveShift) |
                   record;
    }
    return true;
}

inline wchar_t DecodeFrnDrive(Frn encoded_frn) {
    encoded_frn &= ~kSearchSameNameNextMask;
    const auto code = static_cast<std::uint8_t>((encoded_frn & kEncodedFrnDriveMask) >> kEncodedFrnDriveShift);
    return CodeToDrive(code);
}

inline Frn DecodeRawFrn(Frn encoded_frn) {
    if (encoded_frn == kRootParentFrn) {
        return kRootParentFrn;
    }
    return (encoded_frn & kFrnSequenceMask) | (encoded_frn & kEncodedFrnRecordMask);
}

inline Frn StripSearchFrnFlags(Frn encoded_frn) {
    return encoded_frn & ~kSearchSameNameNextMask;
}

inline bool HasSameNameNext(Frn encoded_frn) {
    return (encoded_frn & kSearchSameNameNextMask) != 0;
}

inline Frn SetSameNameNext(Frn encoded_frn, bool value) {
    encoded_frn = StripSearchFrnFlags(encoded_frn);
    return value ? (encoded_frn | kSearchSameNameNextMask) : encoded_frn;
}

}  // namespace fsengine

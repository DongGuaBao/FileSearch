#include "posting_codec.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace fsengine {

namespace {

constexpr std::size_t kOptPfdBlockSize = 128;
constexpr std::uint8_t kPostingBlockRaw = 0;
constexpr std::uint8_t kPostingBlockOptPfd = 1;

void WriteVarUint32(std::uint32_t value, std::vector<std::uint8_t>* out) {
    while (value >= 0x80) {
        out->push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<std::uint8_t>(value));
}

bool ReadVarUint32(const std::uint8_t*& ptr,
                   const std::uint8_t* end,
                   std::uint32_t* value) {
    std::uint32_t result = 0;
    std::uint32_t shift = 0;
    while (ptr < end && shift <= 28) {
        const std::uint8_t byte = *ptr++;
        result |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

std::size_t VarUint32Size(std::uint32_t value) {
    std::size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

void WriteUint32(std::uint32_t value, std::vector<std::uint8_t>* out) {
    out->push_back(static_cast<std::uint8_t>(value & 0xff));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

bool ReadUint32(const std::uint8_t*& ptr,
                const std::uint8_t* end,
                std::uint32_t* value) {
    if (static_cast<std::size_t>(end - ptr) < sizeof(std::uint32_t)) {
        return false;
    }
    *value = static_cast<std::uint32_t>(ptr[0]) |
             (static_cast<std::uint32_t>(ptr[1]) << 8) |
             (static_cast<std::uint32_t>(ptr[2]) << 16) |
             (static_cast<std::uint32_t>(ptr[3]) << 24);
    ptr += sizeof(std::uint32_t);
    return true;
}

void PackLowBits(const std::uint32_t* values,
                 std::size_t count,
                 std::uint8_t bit_width,
                 std::vector<std::uint8_t>* out) {
    if (bit_width == 0) {
        return;
    }
    if (bit_width == 32) {
        for (std::size_t i = 0; i < count; ++i) {
            WriteUint32(values[i], out);
        }
        return;
    }

    std::uint64_t buffer = 0;
    std::uint32_t bits = 0;
    const std::uint64_t mask = (std::uint64_t{1} << bit_width) - 1;
    for (std::size_t i = 0; i < count; ++i) {
        buffer |= (static_cast<std::uint64_t>(values[i]) & mask) << bits;
        bits += bit_width;
        while (bits >= 8) {
            out->push_back(static_cast<std::uint8_t>(buffer & 0xff));
            buffer >>= 8;
            bits -= 8;
        }
    }
    if (bits > 0) {
        out->push_back(static_cast<std::uint8_t>(buffer & 0xff));
    }
}

bool UnpackLowBits(const std::uint8_t*& ptr,
                   const std::uint8_t* end,
                   std::size_t count,
                   std::uint8_t bit_width,
                   std::vector<std::uint32_t>* values) {
    values->assign(count, 0);
    if (bit_width == 0) {
        return true;
    }
    if (bit_width == 32) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!ReadUint32(ptr, end, &(*values)[i])) {
                return false;
            }
        }
        return true;
    }

    std::uint64_t buffer = 0;
    std::uint32_t bits = 0;
    const std::uint64_t mask = (std::uint64_t{1} << bit_width) - 1;
    for (std::size_t i = 0; i < count; ++i) {
        while (bits < bit_width) {
            if (ptr >= end) {
                return false;
            }
            buffer |= static_cast<std::uint64_t>(*ptr++) << bits;
            bits += 8;
        }
        (*values)[i] = static_cast<std::uint32_t>(buffer & mask);
        buffer >>= bit_width;
        bits -= bit_width;
    }
    return true;
}

void EncodePostingBlock(const std::uint32_t* deltas,
                        std::size_t count,
                        std::vector<std::uint8_t>* out) {
    const std::size_t raw_size = 2 + count * sizeof(std::uint32_t);
    std::uint8_t best_width = 0;
    std::size_t best_size = (std::numeric_limits<std::size_t>::max)();
    for (std::uint8_t width = 0; width < 32; ++width) {
        const std::uint32_t threshold = std::uint32_t{1} << width;
        const std::size_t low_bytes = (static_cast<std::size_t>(width) * count + 7) / 8;
        std::size_t exception_bytes = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (deltas[i] >= threshold) {
                exception_bytes += 1 + VarUint32Size(deltas[i] >> width);
            }
        }
        const std::size_t total = 4 + VarUint32Size(static_cast<std::uint32_t>(exception_bytes)) +
                                  low_bytes + exception_bytes;
        if (total < best_size) {
            best_size = total;
            best_width = width;
        }
    }

    if (raw_size <= best_size) {
        out->push_back(kPostingBlockRaw);
        out->push_back(static_cast<std::uint8_t>(count - 1));
        for (std::size_t i = 0; i < count; ++i) {
            WriteUint32(deltas[i], out);
        }
        return;
    }

    const std::size_t exception_size_pos = out->size() + 3;
    out->push_back(kPostingBlockOptPfd);
    out->push_back(static_cast<std::uint8_t>(count - 1));
    out->push_back(best_width);
    out->push_back(0);

    const std::size_t lows_start = out->size();
    PackLowBits(deltas, count, best_width, out);
    const std::size_t exceptions_start = out->size();
    const std::uint32_t threshold = std::uint32_t{1} << best_width;
    for (std::size_t i = 0; i < count; ++i) {
        if (deltas[i] >= threshold) {
            out->push_back(static_cast<std::uint8_t>(i));
            WriteVarUint32(deltas[i] >> best_width, out);
        }
    }
    const std::size_t exception_bytes = out->size() - exceptions_start;
    if (exception_bytes < 0x80) {
        (*out)[exception_size_pos] = static_cast<std::uint8_t>(exception_bytes);
        return;
    }

    std::vector<std::uint8_t> rebuilt;
    rebuilt.reserve(out->size() + VarUint32Size(static_cast<std::uint32_t>(exception_bytes)));
    rebuilt.insert(rebuilt.end(), out->begin(), out->begin() + static_cast<std::ptrdiff_t>(exception_size_pos));
    WriteVarUint32(static_cast<std::uint32_t>(exception_bytes), &rebuilt);
    rebuilt.insert(rebuilt.end(), out->begin() + static_cast<std::ptrdiff_t>(lows_start), out->end());
    out->swap(rebuilt);
}

}  // namespace

std::vector<std::uint8_t> EncodePostingList(const std::vector<std::uint32_t>& postings) {
    std::vector<std::uint8_t> out;
    out.reserve(std::max<std::size_t>(16, postings.size() * 2));
    std::uint32_t previous = 0;
    for (std::size_t offset = 0; offset < postings.size(); offset += kOptPfdBlockSize) {
        const std::size_t count = std::min<std::size_t>(kOptPfdBlockSize, postings.size() - offset);
        std::uint32_t deltas[kOptPfdBlockSize];
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t value = postings[offset + i];
            deltas[i] = value - previous;
            previous = value;
        }
        EncodePostingBlock(deltas, count, &out);
    }
    return out;
}

bool ForEachEncodedPostingValue(const std::uint8_t* data,
                                std::uint32_t byte_size,
                                std::uint32_t count,
                                const std::function<bool(std::uint32_t)>& callback) {
    if (data == nullptr && byte_size != 0) {
        return false;
    }

    const std::uint8_t* ptr = data;
    const std::uint8_t* end = data + byte_size;
    std::size_t emitted = 0;
    std::uint32_t previous = 0;
    while (ptr < end && emitted < count) {
        const std::uint8_t kind = *ptr++;
        if (ptr >= end) {
            return false;
        }
        const std::size_t block_count = static_cast<std::size_t>(*ptr++) + 1;
        if (kind == kPostingBlockRaw) {
            for (std::size_t i = 0; i < block_count; ++i) {
                std::uint32_t delta = 0;
                if (!ReadUint32(ptr, end, &delta)) {
                    return false;
                }
                previous += delta;
                ++emitted;
                if (!callback(previous)) {
                    return true;
                }
            }
            continue;
        }

        if (kind != kPostingBlockOptPfd || ptr >= end) {
            return false;
        }
        const std::uint8_t bit_width = *ptr++;
        std::uint32_t exception_bytes = 0;
        if (!ReadVarUint32(ptr, end, &exception_bytes) ||
            exception_bytes > static_cast<std::uint32_t>(end - ptr)) {
            return false;
        }
        std::vector<std::uint32_t> deltas;
        if (!UnpackLowBits(ptr, end, block_count, bit_width, &deltas) ||
            exception_bytes > static_cast<std::uint32_t>(end - ptr)) {
            return false;
        }
        const std::uint8_t* exception_end = ptr + exception_bytes;
        while (ptr < exception_end) {
            const std::uint8_t position = *ptr++;
            std::uint32_t high_bits = 0;
            if (position >= deltas.size() ||
                !ReadVarUint32(ptr, exception_end, &high_bits)) {
                return false;
            }
            deltas[position] |= high_bits << bit_width;
        }
        if (ptr != exception_end) {
            return false;
        }
        for (const std::uint32_t delta : deltas) {
            previous += delta;
            ++emitted;
            if (!callback(previous)) {
                return true;
            }
        }
    }
    return emitted == count;
}

}  // namespace fsengine

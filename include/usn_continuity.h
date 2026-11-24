#pragma once
#include <cstdint>

namespace fsengine {
// Pure checkpoint validation, independent of kernel IO. A checkpoint equal to
// NextUsn is a valid empty tail; one before FirstUsn has lost retained history.
inline bool UsnCheckpointContinuous(std::uint64_t checkpoint_journal,
                                    std::int64_t checkpoint,
                                    std::uint64_t current_journal,
                                    std::int64_t first,
                                    std::int64_t next) {
    return checkpoint_journal != 0 && checkpoint_journal == current_journal &&
           first >= 0 && next >= first && checkpoint >= first && checkpoint <= next;
}
}

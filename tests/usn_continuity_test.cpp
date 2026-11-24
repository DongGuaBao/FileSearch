#include "usn_continuity.h"
#include <stdexcept>

int main() {
    using fsengine::UsnCheckpointContinuous;
    auto require = [](bool value) { if (!value) throw std::runtime_error("USN continuity regression"); };
    require(UsnCheckpointContinuous(7, 100, 7, 100, 200));
    require(UsnCheckpointContinuous(7, 150, 7, 100, 200));
    require(UsnCheckpointContinuous(7, 200, 7, 100, 200));
    require(UsnCheckpointContinuous(7, 100, 7, 100, 100));
    require(!UsnCheckpointContinuous(7, 99, 7, 100, 200));
    require(!UsnCheckpointContinuous(7, 201, 7, 100, 200));
    require(!UsnCheckpointContinuous(7, 150, 8, 100, 200));
    require(!UsnCheckpointContinuous(0, 150, 7, 100, 200));
    require(!UsnCheckpointContinuous(7, 150, 7, 200, 100));
    require(!UsnCheckpointContinuous(7, -1, 7, -2, 100));
}

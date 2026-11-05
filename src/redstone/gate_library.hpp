#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "redstone/solver.hpp"
#include "redstone/world.hpp"

namespace rs {

// One 2-input gate built by the solver, ready to be placed: inputs are
// repeaters facing east on the west face at in1 (top row) and in2 (bottom
// row), the output is dust on the east face. Both faces are empty but for
// the ports, so modules can sit next to each other.
struct GateModule {
  std::uint8_t function = 0;
  World layout{{1, 1, 1}};
  Vec3 in1{}, in2{}, out{};
};

struct GateLibraryOptions {
  // Boxes tried in order; the first that holds the gate is used.
  std::vector<Vec3> sizes{{4, 2, 3}, {5, 2, 3}, {5, 2, 4},
                          {4, 3, 3}, {5, 3, 3}, {5, 3, 4}};
  double seconds_per_size = 60.0; // per size and output row
  int horizon = 30;
  SolverOptions solver; // threads and verification placements
  std::atomic<bool> *cancel = nullptr;
};

// Builds a module for `function` (bit (a << 1) | b of the 2-input truth
// table). Nothing if no size works in time.
std::optional<GateModule> build_gate(std::uint8_t function,
                                     const GateLibraryOptions &options,
                                     const ProgressCallback &progress = {});

} // namespace rs

#pragma once

#include <atomic>
#include <optional>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs {

// A full adder from two half adders made by the solver:
//   module: a half adder in a small box (inputs on its west face, XOR and
//     AND outputs on its east face), faces empty but for the ports so two
//     copies can touch (or `gap` free columns apart);
//   stage 1: both copies pinned, HA1's XOR output feeding HA2; the solver
//     only routes the third input to HA2 and is asked for the sum;
//   stage 2: stage 1 pinned; the solver routes HA1's AND output past HA2
//     into HA2's AND output, which becomes the carry.
// Each solve may only use the cells its wire needs, which keeps it small.
struct ComposeOptions {
  Vec3 module_size{5, 2, 4}; // y 2: one layer on a floor
  int xor_row = 2;           // z of the module's XOR output on its east face
  int and_row = 0;           // z of its AND output; below xor_row
  bool clean_faces = true;
  int gap = 0;
  int horizon = 80; // two gates in a chain settle slowly
  double module_seconds = 300.0;
  double stage_seconds = 300.0;
  // Threads and verification placements for every solve.
  SolverOptions solver;
  std::atomic<bool> *cancel = nullptr;
};

struct ComposeResult {
  Spec spec;    // the composed box and ports, the full adder rows, the
                // circuit as hints
  World layout; // verified
};

// Nothing if a step finds no circuit (the progress log says which).
std::optional<ComposeResult>
compose_full_adder(const ComposeOptions &options,
                   const ProgressCallback &progress = {});

} // namespace rs

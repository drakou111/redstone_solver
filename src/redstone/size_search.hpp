#pragma once

#include <atomic>
#include <optional>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs {

struct SizeSearchOptions {
  double time_limit_seconds = 600.0;
  // Searches at the same time; 0: a quarter of the hardware threads (each
  // search runs two).
  int parallel = 0;
  // A size's first try; sizes still open get twice as long each round.
  double first_try_seconds = 60.0;
  int max_height = 0; // 0: up to the height of the circuit to beat
  // Threads, verification placements. Time limit, stop-at-first and cancel
  // are set per search.
  SolverOptions solver;
  std::atomic<bool> *cancel = nullptr;
  PreviewCallback preview; // each smaller circuit found
  // Cells of a smaller circuit found elsewhere (a shrink running alongside):
  // boxes at least this big are skipped.
  const std::atomic<int> *bound = nullptr;
};

struct SizeSearchResult {
  Spec spec;    // its box, ports where the circuit has them, layout as hints
  World layout; // verified
};

// Fresh searches for the request of `spec` (its rows, port kinds, palette
// and timing) in every box smaller than `beat`, largest first: inputs may
// sit anywhere on the west side and outputs anywhere on the east side, and
// the solver places everything else. Unlike a shrink, nothing is kept from
// an earlier circuit, so it cannot get stuck in one. The smallest verified
// circuit is returned; nothing if no smaller box worked in time.
std::optional<SizeSearchResult>
search_sizes(const Spec &spec, Vec3 beat, const SizeSearchOptions &options,
             const ProgressCallback &progress = {});

// The request of `spec` in a box of `size`, ports free along their sides.
// Nothing when the box cannot hold the ports.
std::optional<Spec> spec_for_size(const Spec &spec, Vec3 size);

} // namespace rs

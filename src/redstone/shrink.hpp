#pragma once

#include <atomic>
#include <optional>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs {

struct ShrinkOptions {
  double time_limit_seconds = 600.0; // the whole shrink
  double cut_seconds = 120.0;        // one repair attempt
  int parallel = 3;                  // cuts tried at the same time
  // Per repair: threads, verification placements, sketch step time. Time
  // limit, stop-at-first and cancel are set by shrink().
  SolverOptions solver;
  std::atomic<bool> *cancel = nullptr;
};

struct ShrinkResult {
  Spec spec;    // the last verified box, its ports and the layout as hints
  World layout; // the last verified layout
  int steps = 0;
};

// The spec with slice `index` of `axis` (0 x, 1 y, 2 z) removed and
// everything beyond it moved back by one: ports, kept, empty and sketch
// cells. Nothing if a fixed port sits in the slice or a free port's region
// would be emptied.
std::optional<Spec> cut_slice(const Spec &spec, int axis, int index);

// The spec with its ports fixed where `layout` has them and every other
// block of `layout` as a sketch: the starting point of shrink().
Spec spec_with_layout(const Spec &spec, const World &layout);

// Removes one slice at a time from a verified layout: every cut is tried
// (several at once) with the layout as a sketch, the first one the solver
// verifies is kept, and the search starts again from it, until no cut
// works, the time runs out or `cancel` is set.
ShrinkResult shrink(const Spec &spec, const World &layout,
                    const ShrinkOptions &options,
                    const ProgressCallback &progress = {});

} // namespace rs

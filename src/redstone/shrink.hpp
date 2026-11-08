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
  // Cuts tried at the same time; 0: half the hardware threads (each repair
  // is a small, mostly single-threaded solve).
  int parallel = 0;
  // Per repair: threads, verification placements, sketch step time. Time
  // limit, stop-at-first and cancel are set by shrink().
  SolverOptions solver;
  std::atomic<bool> *cancel = nullptr;
  // Each smaller (or, tidying, leaner) circuit as it is found.
  PreviewCallback preview;
};

struct ShrinkResult {
  Spec spec;    // the last verified box, its ports and the layout as hints
  World layout; // the last verified layout
  int steps = 0;
};

// Blocks of a layout, ports left out.
int layout_blocks(const World &layout);

// Fewer blocks in the same box: windows of the layout are freed in turn
// (everything else kept as it is) and solved for the fewest blocks, several
// at once; the best saving of a round is kept and the windows are tried
// again, until a round saves nothing or the time (options.time_limit_seconds)
// runs out. `steps` counts the rounds that saved blocks.
ShrinkResult tidy(const Spec &spec, const World &layout,
                  const ShrinkOptions &options,
                  const ProgressCallback &progress = {});

// How many cuts `options` tries at the same time.
int shrink_workers(const ShrinkOptions &options);

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
// verifies is kept, and the search starts again from it. At a dead end it
// goes back a step and tries the other cuts there (a few times at most).
// Ends when no cut works, the time runs out or `cancel` is set, with the
// smallest layout found; time left when no cut works goes to tidy().
ShrinkResult shrink(const Spec &spec, const World &layout,
                    const ShrinkOptions &options,
                    const ProgressCallback &progress = {});

} // namespace rs

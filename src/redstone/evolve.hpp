#pragma once

#include <atomic>
#include <functional>
#include <optional>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs {

// Searching a 2-high box block by block, with no gates, placement or
// routing: each thread anneals a grid of the component layer (y = 1; the
// floor follows: a block under whatever needs one), scoring every grid by
// simulating each input row from the built state, as the verifier does. So
// circuits that rely on loops are found as easily as others (the spec's
// allow_feedback still decides whether they are accepted).
struct EvolveOptions {
  double time_limit_seconds = 60.0;
  int threads = 0; // 0: all hardware threads
  unsigned seed = 1;
  // Defaults from the runs that found the 7x2x7 input counter.
  double start_temperature = 150.0;
  double end_temperature = 5.0;
  long steps_per_run = 1000000; // then a run starts over
  double from_best = 0.7;       // share of runs that start from the best grid
  bool use_sketch = true;       // runs start from the spec's hint lines
  // Checks a grid that gets every row right before it is accepted.
  VerificationOptions verification;
  std::atomic<bool> *cancel = nullptr;
  // The best grid so far, now and then.
  PreviewCallback preview;
  // Where some runs start instead: a grid from elsewhere (the SAT search's
  // rejected candidates, say), or nothing when there is none.
  std::function<std::optional<World>()> fresh_start;
};

struct EvolveResult {
  std::optional<World> layout; // a circuit the verifier accepted
  std::optional<World> best;   // otherwise the closest grid found
  long best_score = 0;         // rows-right score of `best`
  long perfect_score = 0;      // the score of a grid right in every row
  int checks = 0;              // (row, output) pairs to get right
  int best_right = 0;          // how many of them `best` gets right
  long evaluations = 0;
  long rejected = 0; // grids right in every row the checks refused
};

// Throws std::invalid_argument for a box that is not 2 high, or a spec
// whose rows are not all on/off inputs.
EvolveResult evolve(const Spec &spec, const EvolveOptions &options,
                    const ProgressCallback &progress = {});

// Polishing a near miss: every change of one cell of the spec's sketch,
// then of two (with `triples`, also of three cells close together), tried
// in parallel; the first circuit the checks accept ends it. When none is
// accepted but some scored better, the best becomes the start and it goes
// again, `rounds` times at most.
struct PolishOptions {
  int rounds = 4;
  bool triples = false;
  double time_limit_seconds = 600.0;
  int threads = 0;
  VerificationOptions verification;
  std::atomic<bool> *cancel = nullptr;
};

std::optional<World> polish(const Spec &spec, const PolishOptions &options,
                            const ProgressCallback &progress = {});

} // namespace rs

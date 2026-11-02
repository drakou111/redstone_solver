#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "redstone/encoder.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

namespace rs {

enum class SolverEngine {
  Sat,   // CaDiCaL portfolio on the clause model, block count by bounds
  CpSat, // OR-Tools CP-SAT
};

struct SolverOptions {
  SolverEngine engine = SolverEngine::Sat;
  double time_limit_seconds = 60.0;
  int threads = 0; // 0: all hardware threads
  int max_refinements = 100;
  bool log_search = false; // CP-SAT's own progress log
  // Parallel SAT solvers with different seeds. More than one was slower in
  // benchmarks (they compete for memory bandwidth), hence the default.
  int max_sat_workers = 1;
  int sat_seed = 0; // varies the SAT search (benchmarks average over seeds)
  bool stop_at_first = false; // accept the first verified circuit
  // SAT engine: a second search alongside the main one only places solid,
  // dust, torches and repeaters. Its model is several times smaller, so it
  // often finds circuits much sooner; the best circuit of either bounds both.
  bool quick_palette_search = true;
  bool show_rejected = false; // include rejected layouts in progress messages
  VerificationOptions verification;
  // Set to true from another thread to stop early.
  std::atomic<bool> *cancel = nullptr;
};

enum class SolveStatus {
  Optimal,         // fewest blocks among circuits the model can express
  Feasible,        // verified, but the time ran out before proving optimality
  Infeasible,      // no circuit fits the volume and palette
  OutOfTime,       // no verified circuit within the time limit
  RefinementLimit, // verification kept rejecting candidates
  Cancelled,       // stopped through SolverOptions::cancel
};

std::string_view to_string(SolveStatus status);

struct SolveReport {
  SolveStatus status = SolveStatus::OutOfTime;
  std::optional<EncodedSolution> solution;
  int refinements = 0;
  double seconds = 0.0;
  std::int64_t sat_conflicts = 0; // search effort of the SAT engine
};

using ProgressCallback = std::function<void(std::string_view)>;

// Solves the model, verifies the best candidate in the simulator, and refines
// the model with each counterexample until a candidate passes.
SolveReport solve(const Spec &spec, const SolverOptions &options = {},
                  const ProgressCallback &progress = {});

} // namespace rs

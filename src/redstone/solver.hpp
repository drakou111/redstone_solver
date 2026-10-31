#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "redstone/encoder.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

namespace rs {

struct SolverOptions {
  double time_limit_seconds = 60.0;
  int threads = 0; // 0: all hardware threads
  int max_refinements = 100;
  bool log_search = false;    // CP-SAT's own progress log
  bool stop_at_first = false; // accept the first verified circuit
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
};

using ProgressCallback = std::function<void(std::string_view)>;

// Solves the model, verifies the best candidate in the simulator, and refines
// the model with each counterexample until a candidate passes.
SolveReport solve(const Spec &spec, const SolverOptions &options = {},
                  const ProgressCallback &progress = {});

} // namespace rs

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "ortools/sat/cp_model.h"
#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs {

// An input vector whose settled state is part of the model.
struct SteadyCase {
  InputVector inputs;
  std::optional<std::vector<SignalRange>>
      expected; // nullopt: outputs unconstrained
};

// Settled under `from`, the inputs switch to `to` (indices into the steady
// cases).
struct TransitionCase {
  int from = 0;
  int to = 0;
  bool operator==(const TransitionCase &) const = default;
};

struct ModelPlan {
  std::vector<SteadyCase> steady;
  std::vector<TransitionCase> transitions;
};

struct EncoderOptions {
  bool enforce_outputs = true;
  bool minimize_blocks = true;
  std::optional<World> fixed_layout;    // forces every non-port cell
  std::vector<World> forbidden_layouts; // layouts that failed verification
  std::optional<int> max_blocks;        // upper bound on placed blocks
  // Off: only the clauses are kept (for a SAT solver), not the CP-SAT model.
  bool build_cp_model = true;
};

struct TransitionTrace {
  TransitionCase transition;
  std::vector<std::vector<int>> outputs; // [tick 0..horizon][output channel]
};

struct EncodedSolution {
  World layout;
  int block_count = 0;
  std::vector<InputVector> steady_inputs;       // [steady case]
  std::vector<std::vector<int>> steady_outputs; // [steady case][output channel]
  std::vector<TransitionTrace> traces;
};

// CP-SAT model of a circuit in the spec's volume that settles correctly for
// every steady case and meets the timing for every transition.
//
// Per game tick, dust and block power are solved exactly. Torches, repeaters,
// comparators and observers are state machines with scheduled-tick variables
// that read the state at the end of the previous tick. Order effects within a
// tick are not modelled; the simulator verifies candidates for that.
class CircuitEncoder {
public:
  CircuitEncoder(const Spec &spec, const ModelPlan &plan,
                 EncoderOptions options = {});
  ~CircuitEncoder();
  CircuitEncoder(const CircuitEncoder &) = delete;
  CircuitEncoder &operator=(const CircuitEncoder &) = delete;

  const operations_research::sat::CpModelBuilder &model() const;
  EncodedSolution
  decode(const operations_research::sat::CpSolverResponse &response) const;
  // values[i] is variable i (DIMACS variable i + 1).
  EncodedSolution decode(const std::vector<bool> &values) const;

  // These add clauses after construction (clauses() grows), so a SAT solver
  // can take in a refined model without starting over.
  void forbid_layout(const World &layout);
  void limit_blocks(int max_blocks);
  void add_steady(const SteadyCase &steady);
  // `steady` lists every steady case added so far, in order.
  void add_transition(TransitionCase transition,
                      const std::vector<SteadyCase> &steady);
  int horizon() const;
  // The model as CNF (DIMACS literals: variable index + 1, negative = not).
  const std::vector<std::vector<int>> &clauses() const;
  int variable_count() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rs

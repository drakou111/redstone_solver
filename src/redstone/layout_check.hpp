#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"
#include "redstone/world.hpp"

namespace rs {

// Outputs once the circuit has come to rest under one input vector.
struct SettledCheck {
  InputVector inputs;
  std::vector<int> outputs; // empty: never came to rest
  std::vector<SignalRange> expected;
  bool ok = false;
};

// Outputs tick by tick after switching from one settled vector to another.
struct SwitchCheck {
  InputVector from;
  InputVector to;
  std::vector<std::vector<int>> timeline; // [tick][output channel]
  std::optional<std::string> problem;     // first broken rule
};

// Everything wrong with a complete layout, not just the first failure: what
// a designer (person or AI) needs to fix it.
struct LayoutCheck {
  std::vector<std::string> invalid; // placement rules broken
  std::optional<std::string> unstable;
  std::vector<SettledCheck> settled;
  std::vector<SwitchCheck> switches;
  // A failure only some positions or rotations show (update order).
  std::optional<VerificationFailure> elsewhere;

  bool passes() const;
};

// The spec's ports and kept blocks plus its hint lines, every other cell
// empty. Free ports are not supported.
World layout_from_hints(const Spec &spec);

// Whether the solver's model can express a layout: its placement rules and,
// unless the spec allows feedback, no signal loop through a component. The
// simulator accepts loops; the solver (and so a sketch repair) cannot.
// Builds the model once; each layout is then one quick solve. Not
// thread-safe: one per thread.
class ExpressChecker {
public:
  explicit ExpressChecker(const Spec &spec);
  ~ExpressChecker();
  ExpressChecker(const ExpressChecker &) = delete;
  ExpressChecker &operator=(const ExpressChecker &) = delete;

  bool can_express(const World &layout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool solver_can_express(const World &layout, const Spec &spec);

LayoutCheck check_layout(const World &layout, const Spec &spec,
                         const VerificationOptions &options = {});

// Plain text report: rows, then each input change as runs of output values
// ("OUT1 0@0 15@4" means 0 from tick 0, 15 from tick 4).
std::string format_check(const LayoutCheck &check, const Spec &spec);

} // namespace rs

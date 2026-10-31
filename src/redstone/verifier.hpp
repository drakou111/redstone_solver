#pragma once

#include <optional>
#include <string>
#include <vector>

#include "redstone/spec.hpp"
#include "redstone/update_order.hpp"
#include "redstone/world.hpp"

namespace rs {

struct VerificationOptions {
  std::vector<Placement> placements; // empty: standard_placements()
  int max_settle_ticks = 400;
  int max_extra_ticks = 100; // allowed beyond the horizon to come to rest
};

struct VerificationFailure {
  Placement placement;
  std::optional<InputVector> from; // nullopt: settling under `to` failed
  InputVector to;
  std::string reason;

  bool at_identity_placement() const;
};

// All 8 orientations at a few different world positions.
std::vector<Placement> standard_placements();

// Simulates the layout at every placement: settles under each vector, then
// switches from every vector to every other one and checks outputs and timing.
// Returns the first failure, ordered by placement then vectors.
std::optional<VerificationFailure>
verify_layout(const World &layout, const Spec &spec,
              const std::vector<InputVector> &vectors,
              const VerificationOptions &options = {});

std::string to_string(const InputVector &vector);

} // namespace rs

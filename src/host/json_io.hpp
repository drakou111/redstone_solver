#pragma once

#include <nlohmann/json.hpp>

#include "redstone/simulator.hpp"
#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"
#include "redstone/world.hpp"

namespace rs::host {

using Json = nlohmann::json;

// Positions are [x, y, z]; directions and block types use their spec-file
// names. Parsing throws std::invalid_argument with a readable message.

Json to_json(const Spec &spec);
Spec spec_from_json(const Json &json);

Json to_json(const Block &block);
Block block_from_json(const Json &json);

// Only non-air blocks are listed.
Json to_json(const World &world);
World world_from_json(const Json &json);

Json to_json(const SolveReport &report);
Json to_json(const VerificationFailure &failure);

// Live state of every non-air cell, plus inputs, outputs and time.
Json simulation_json(const Simulator &sim, int input_count, int output_count);

} // namespace rs::host

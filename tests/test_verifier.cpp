#include <catch2/catch_test_macros.hpp>

#include "redstone/verifier.hpp"

using namespace rs;

namespace {

constexpr const char *kNotSpec = R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";

World not_gate(const Spec &spec) {
  World world = spec.base_world();
  world.set({1, 1, 0}, Block::solid());
  world.set({2, 1, 0}, Block::torch(Dir::West));
  world.set({3, 0, 0}, Block::glass());
  return world;
}

} // namespace

TEST_CASE("a correct NOT gate passes verification at every placement") {
  Spec spec = parse_spec(kNotSpec);
  CHECK_FALSE(verify_layout(not_gate(spec), spec, spec.verification_vectors()));
}

TEST_CASE("a wrong circuit fails with the offending input change") {
  Spec spec = parse_spec(kNotSpec);
  World buffer = spec.base_world();
  buffer.set({1, 1, 0}, Block::repeater(Dir::East, 1));
  buffer.set({2, 1, 0}, Block::dust());
  buffer.set({1, 0, 0}, Block::glass());
  buffer.set({2, 0, 0}, Block::glass());
  buffer.set({3, 0, 0}, Block::glass());
  std::optional<VerificationFailure> failure =
      verify_layout(buffer, spec, spec.verification_vectors());
  REQUIRE(failure);
  CHECK(failure->at_identity_placement());
  CHECK_FALSE(failure->tiling_interference);
}

TEST_CASE("a deadline that is too early fails verification") {
  Spec spec = parse_spec(kNotSpec);
  spec.timing.latest = 1;
  std::optional<VerificationFailure> failure =
      verify_layout(not_gate(spec), spec, spec.verification_vectors());
  REQUIRE(failure);
  CHECK(failure->from.has_value());
}

TEST_CASE("tiled copies whose output dust touches interfere") {
  Spec spec = parse_spec(kNotSpec);
  spec.tiled[2] = true;
  std::optional<VerificationFailure> failure =
      verify_layout(not_gate(spec), spec, spec.verification_vectors());
  REQUIRE(failure);
  CHECK(failure->tiling_interference);
}

TEST_CASE("tiled copies with room between them do not interfere") {
  Spec spec = parse_spec(kNotSpec);
  spec.size.z = 2;
  spec.tiled[2] = true;
  CHECK_FALSE(verify_layout(not_gate(spec), spec, spec.verification_vectors()));
}

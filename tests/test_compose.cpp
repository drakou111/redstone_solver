#include "redstone/compose.hpp"
#include "redstone/verifier.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

TEST_CASE("compose builds a verified full adder from two half adders") {
  ComposeOptions options;
  options.module_seconds = 120.0;
  options.stage_seconds = 120.0;
  std::optional<ComposeResult> result = compose_full_adder(options);
  REQUIRE(result);
  CHECK(result->layout.size() == result->spec.size);
  CHECK(result->spec.inputs.size() == 3);
  CHECK(result->spec.outputs.size() == 2);
  CHECK_FALSE(result->spec.hints.empty());
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

TEST_CASE("compose rejects module settings that cannot fit") {
  ComposeOptions options;
  options.and_row = 2;
  options.xor_row = 1;
  CHECK_THROWS_AS(compose_full_adder(options), std::invalid_argument);
}

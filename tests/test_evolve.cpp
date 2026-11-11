#include <atomic>
#include <string>

#include "redstone/evolve.hpp"
#include "redstone/solver.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

// Last, so Catch2's CHECK replaces the one abseil (via OR-Tools) defines.
#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

// An OR of two inputs in a small 2-high box.
const char *kOr = R"(size 4 2 3
input IN1 repeater 0 1 0 east
input IN2 repeater 0 1 2 east
output OUT1 dust 3 1 1
row IN1(ss0),IN2(ss0) -> OUT1(ss0)
row IN1(ss15),IN2(ss0) -> OUT1(ss1..15)
row IN1(ss0),IN2(ss15) -> OUT1(ss1..15)
row IN1(ss15),IN2(ss15) -> OUT1(ss1..15)
)";

// The user's 3-input counter (one of four outputs on for how many inputs
// are on): it works in game thanks to a loop through two comparators.
const char *kCounter = R"(size 8 2 7
horizon 20
input IN1 dust 0 1 1
input IN2 dust 0 1 3
input IN3 dust 0 1 5
output OUT1 repeater 7 1 0 east
output OUT2 repeater 7 1 2 east
output OUT3 repeater 7 1 4 east
output OUT4 repeater 7 1 6 east
row IN1(ss0),IN2(ss0),IN3(ss0) -> OUT1(ss0),OUT2(ss0),OUT3(ss0),OUT4(ss1..15)
row IN1(ss0),IN2(ss0),IN3(ss15) -> OUT1(ss0),OUT2(ss0),OUT3(ss1..15),OUT4(ss0)
row IN1(ss0),IN2(ss15),IN3(ss0) -> OUT1(ss0),OUT2(ss0),OUT3(ss1..15),OUT4(ss0)
row IN1(ss0),IN2(ss15),IN3(ss15) -> OUT1(ss0),OUT2(ss1..15),OUT3(ss0),OUT4(ss0)
row IN1(ss15),IN2(ss0),IN3(ss0) -> OUT1(ss0),OUT2(ss0),OUT3(ss1..15),OUT4(ss0)
row IN1(ss15),IN2(ss0),IN3(ss15) -> OUT1(ss0),OUT2(ss1..15),OUT3(ss0),OUT4(ss0)
row IN1(ss15),IN2(ss15),IN3(ss0) -> OUT1(ss0),OUT2(ss1..15),OUT3(ss0),OUT4(ss0)
row IN1(ss15),IN2(ss15),IN3(ss15) -> OUT1(ss1..15),OUT2(ss0),OUT3(ss0),OUT4(ss0)
hint 0 0 0 solid
hint 1 0 0 solid
hint 2 0 0 solid
hint 3 0 0 solid
hint 4 0 0 solid
hint 5 0 0 solid
hint 6 0 0 solid
hint 7 0 0 solid
hint 0 0 1 solid
hint 1 0 1 solid
hint 2 0 1 solid
hint 3 0 1 solid
hint 4 0 1 solid
hint 5 0 1 solid
hint 6 0 1 solid
hint 7 0 1 solid
hint 0 0 2 solid
hint 1 0 2 solid
hint 2 0 2 solid
hint 3 0 2 solid
hint 4 0 2 solid
hint 5 0 2 solid
hint 6 0 2 solid
hint 7 0 2 solid
hint 0 0 3 solid
hint 1 0 3 solid
hint 2 0 3 solid
hint 3 0 3 solid
hint 4 0 3 solid
hint 5 0 3 solid
hint 6 0 3 solid
hint 7 0 3 solid
hint 0 0 4 solid
hint 1 0 4 solid
hint 2 0 4 solid
hint 3 0 4 solid
hint 4 0 4 solid
hint 5 0 4 solid
hint 6 0 4 solid
hint 7 0 4 solid
hint 0 0 5 solid
hint 1 0 5 solid
hint 2 0 5 solid
hint 3 0 5 solid
hint 4 0 5 solid
hint 5 0 5 solid
hint 6 0 5 solid
hint 7 0 5 solid
hint 0 0 6 solid
hint 1 0 6 solid
hint 2 0 6 solid
hint 3 0 6 solid
hint 4 0 6 solid
hint 5 0 6 solid
hint 6 0 6 solid
hint 7 0 6 solid
hint 2 1 0 solid
hint 3 1 0 dust
hint 5 1 0 dust
hint 6 1 0 repeater east 1
hint 1 1 1 comparator east subtract
hint 2 1 1 comparator north subtract
hint 3 1 1 solid
hint 4 1 1 torch west
hint 5 1 1 dust
hint 0 1 2 dust
hint 1 1 2 dust
hint 2 1 2 solid
hint 3 1 2 comparator south compare
hint 5 1 2 target
hint 6 1 2 torch west
hint 1 1 3 comparator east subtract
hint 2 1 3 comparator north subtract
hint 3 1 3 solid
hint 4 1 3 dust
hint 5 1 3 dust
hint 0 1 4 dust
hint 1 1 4 dust
hint 2 1 4 solid
hint 4 1 4 dust
hint 5 1 4 repeater east 1
hint 6 1 4 comparator east subtract
hint 1 1 5 comparator east subtract
hint 2 1 5 comparator north subtract
hint 4 1 5 dust
hint 6 1 5 dust
hint 2 1 6 barrel 5
hint 4 1 6 dust
hint 5 1 6 repeater east 1
hint 6 1 6 solid
)";

EvolveOptions quick(double seconds) {
  EvolveOptions o;
  o.time_limit_seconds = seconds;
  o.threads = 4;
  o.verification.placements = {Placement{}};
  return o;
}

} // namespace

TEST_CASE("evolve finds an OR from an empty box") {
  const Spec spec = parse_spec(kOr);
  EvolveResult result = evolve(spec, quick(60));
  REQUIRE(result.layout);
  CHECK_FALSE(verify_layout(*result.layout, spec, spec.verification_vectors(),
                            quick(0).verification));
}

TEST_CASE("evolve keeps a working sketch, loops only when allowed") {
  Spec spec = parse_spec(kCounter);
  spec.allow_feedback = true;
  EvolveResult kept = evolve(spec, quick(30));
  REQUIRE(kept.layout);
  CHECK(kept.evaluations < 100000); // found next to the sketch, not far away

  // Without loops the same sketch is refused.
  spec.allow_feedback = false;
  EvolveResult refused = evolve(spec, quick(5));
  CHECK_FALSE(refused.layout);
  CHECK(refused.rejected > 0);
  // Its closest try is right everywhere; only the loop rule refused it.
  CHECK(refused.checks == 64); // 8 rows x 4 outputs, from built and in the walk
  CHECK(refused.best_right == 64);
}

TEST_CASE("evolve stops when cancelled and wants a 2-high box") {
  const Spec spec = parse_spec(kOr);
  std::atomic<bool> cancel = true;
  EvolveOptions o = quick(60);
  o.cancel = &cancel;
  EvolveResult result = evolve(spec, o);
  CHECK(result.evaluations < 100000);

  Spec tall = spec;
  tall.size.y = 3;
  CHECK_THROWS_AS(evolve(tall, quick(1)), std::invalid_argument);
}

TEST_CASE("Search the box runs Evolve beside the SAT search, 2-high boxes") {
  const Spec spec = parse_spec(kOr);
  for (const bool alongside : {true, false}) {
    SolverOptions o;
    o.time_limit_seconds = 60;
    o.threads = 4;
    o.stop_at_first = true;
    o.evolve_alongside = alongside;
    o.verification.placements = {Placement{}};
    std::string log;
    SolveReport report = solve(
        spec, o, [&](std::string_view m) { log += std::string(m) + "\n"; });
    INFO(log);
    REQUIRE(report.solution);
    CHECK_FALSE(verify_layout(report.solution->layout, spec,
                              spec.verification_vectors(), o.verification));
    CHECK((log.find("[evolve]") != std::string::npos) == alongside);
  }
}

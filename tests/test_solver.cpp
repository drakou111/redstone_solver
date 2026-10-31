#include <catch2/catch_test_macros.hpp>

#include "redstone/simulator.hpp"
#include "redstone/solver.hpp"

using namespace rs;

namespace {

constexpr double kTimeLimitSeconds = 60.0;

SolverOptions test_options() {
  SolverOptions options;
  options.time_limit_seconds = kTimeLimitSeconds;
  return options;
}

} // namespace

TEST_CASE("solver finds the smallest NOT gate") {
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
palette solid glass dust torch repeater comparator
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  SolveReport report = solve(spec, test_options());
  REQUIRE(report.status == SolveStatus::Optimal);
  REQUIRE(report.solution);
  // Block, torch, and something under the output dust.
  CHECK(report.solution->block_count == 3);
  CHECK(report.solution->layout.at({2, 1, 0}).type == BlockType::Torch);
}

TEST_CASE("solver meets an exact delay with outputs held until then") {
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
timing 4 4
hold_previous on
palette solid glass dust torch repeater
row IN1(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)");
  SolveReport report = solve(spec, test_options());
  REQUIRE(report.solution);
  CHECK((report.status == SolveStatus::Optimal ||
         report.status == SolveStatus::Feasible));

  // Independent check of the timeline in the simulator.
  Simulator sim(report.solution->layout);
  REQUIRE(sim.run_until_quiet(100));
  sim.set_input(0, 15);
  std::vector<int> timeline{sim.output(0)};
  for (int t = 1; t <= 6; ++t) {
    sim.tick();
    timeline.push_back(sim.output(0));
  }
  CHECK(timeline[0] == 0);
  CHECK(timeline[3] == 0);
  CHECK(timeline[4] > 0);
  CHECK(timeline[6] > 0);
}

TEST_CASE("solver respects tiling: touching dust outputs are impossible, "
          "torches are fine") {
  const char *base = R"(
size 4 3 1
input IN1 strong 0 1 0 east
tile z
horizon 8
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";
  Spec dust = parse_spec(std::string(base) + "output OUT1 dust 3 1 0\n");
  CHECK(solve(dust, test_options()).status == SolveStatus::Infeasible);

  Spec component =
      parse_spec(std::string(base) + "output OUT1 component 3 1 0 east\n");
  SolveReport report = solve(component, test_options());
  REQUIRE(report.status == SolveStatus::Optimal);
  CHECK(report.solution->block_count == 2);
}

TEST_CASE("stop at first returns a verified circuit") {
  Spec spec = parse_spec(R"(
size 4 3 3
input IN1 strong 0 1 0 east
input IN2 strong 0 1 2 east
output OUT1 dust 3 1 1
horizon 10
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
)");
  SolverOptions options = test_options();
  options.stop_at_first = true;
  SolveReport report = solve(spec, options);
  INFO("status: " << to_string(report.status));
  REQUIRE(report.solution);
  CHECK((report.status == SolveStatus::Feasible ||
         report.status == SolveStatus::Optimal));
  CHECK_FALSE(verify_layout(report.solution->layout, spec,
                            spec.verification_vectors()));
}

TEST_CASE("solver reports infeasible when the volume cannot work") {
  // Output dust on the bottom layer has nothing to stand on.
  Spec spec = parse_spec(R"(
size 3 1 1
input IN1 strong 0 0 0 east
output OUT1 dust 2 0 0
row IN1(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)");
  SolveReport report = solve(spec, test_options());
  CHECK(report.status == SolveStatus::Infeasible);
  CHECK_FALSE(report.solution);
}

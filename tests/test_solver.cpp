#include "redstone/simulator.hpp"
#include "redstone/solver.hpp"
#include "redstone/verifier.hpp"
#include <string>
#include <vector>

// Last, so Catch2's CHECK replaces the one abseil (via OR-Tools) defines.
#include <catch2/catch_test_macros.hpp>

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
  // Block, torch, and supports under the comparator input and output dust.
  CHECK(report.solution->block_count == 4);
  CHECK(report.solution->layout.at({2, 1, 0}).type == BlockType::Torch);
  // The support layer holds solid blocks only, even with glass allowed.
  for (int x = 0; x < spec.size.x; ++x) {
    BlockType floor = report.solution->layout.at({x, 0, 0}).type;
    CHECK((floor == BlockType::Air || floor == BlockType::Solid));
  }
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

  Spec repeater =
      parse_spec(std::string(base) + "output OUT1 repeater 3 1 0 east\n");
  SolveReport report = solve(repeater, test_options());
  REQUIRE(report.status == SolveStatus::Optimal);
  // Block and torch, plus supports under the input and the output repeater.
  CHECK(report.solution->block_count == 4);
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

TEST_CASE("a cancelled solve stops promptly") {
  Spec spec = parse_spec(R"(
size 6 4 6
input IN1 strong 0 1 0 east
input IN2 strong 0 1 5 east
output OUT1 dust 5 1 3
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
)");
  std::atomic<bool> cancel = true;
  SolverOptions options = test_options();
  options.cancel = &cancel;
  SolveReport report = solve(spec, options);
  CHECK(report.status == SolveStatus::Cancelled);
  CHECK(report.seconds < 10.0);
}

TEST_CASE("solver keeps pre-placed blocks and leaves empty cells empty") {
  const std::string base = R"(
size 4 2 2
input IN1 comparator 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";
  // Keep the wool the input powers and forbid the cell where the torch
  // usually goes, so the solver has to route around it.
  Spec spec = parse_spec(base + "keep 1 1 0 solid\nempty 2 1 0\n");
  SolveReport report = solve(spec, test_options());
  REQUIRE(report.solution);
  const World &layout = report.solution->layout;
  CHECK(layout.at({1, 1, 0}) == Block::solid());
  CHECK(layout.at({2, 1, 0}).type == BlockType::Air);
  CHECK_FALSE(verify_layout(layout, spec, spec.verification_vectors()));

  // A kept block survives even when the circuit works without it.
  Spec spare = parse_spec(base + "keep 1 1 1 dust\n");
  SolveReport spare_report = solve(spare, test_options());
  REQUIRE(spare_report.solution);
  CHECK(spare_report.solution->layout.at({1, 1, 1}) == Block::dust());

  // The output dust can only stand on the cell below it.
  Spec impossible = parse_spec(base + "empty 3 0 0\n");
  CHECK(solve(impossible, test_options()).status == SolveStatus::Infeasible);
}

TEST_CASE("refinement makes special blocks plain when they are not needed") {
  // A wire on target blocks: plain solid supports work just as well.
  Spec spec = parse_spec(R"(
size 4 2 1
support_layer off
input IN1 repeater 0 1 0 east
output OUT1 dust 3 1 0
horizon 10
row IN1(ss15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)");
  World layout = spec.base_world();
  for (int x = 0; x < 4; ++x)
    layout.set({x, 0, 0}, Block::target());
  layout.set({1, 1, 0}, Block::dust());
  layout.set({2, 1, 0}, Block::dust());
  const std::vector<InputVector> vectors = spec.verification_vectors();
  REQUIRE_FALSE(verify_layout(layout, spec, vectors));

  LayoutRefinement refined = refine_layout(layout, spec, vectors, {}, nullptr,
                                           std::chrono::steady_clock::now() +
                                               std::chrono::seconds(30));

  CHECK(refined.simplified_blocks == 4);
  for (int x = 0; x < 4; ++x)
    CHECK(layout.at({x, 0, 0}).type == BlockType::Solid);
  CHECK_FALSE(verify_layout(layout, spec, vectors));
}

TEST_CASE("refinement turns a chest that must stay a container into a barrel") {
  // A comparator reads the chest's signal: a solid block would give 0, a
  // barrel with the same contents works.
  Spec spec = parse_spec(R"(
size 4 2 2
support_layer off
input IN1 repeater 0 1 1 east
output OUT1 dust 3 1 0
horizon 10
row IN1(ss0..15) -> OUT1(ss1..15)
)");
  World layout = spec.base_world();
  layout.set({0, 0, 1}, Block::solid());
  layout.set({2, 0, 0}, Block::solid());
  layout.set({3, 0, 0}, Block::solid());
  layout.set({1, 1, 0}, Block::chest(5));
  layout.set({2, 1, 0}, Block::comparator(Dir::East, ComparatorMode::Compare));
  const std::vector<InputVector> vectors = spec.verification_vectors();
  REQUIRE_FALSE(verify_layout(layout, spec, vectors));

  LayoutRefinement refined = refine_layout(layout, spec, vectors, {}, nullptr,
                                           std::chrono::steady_clock::now() +
                                               std::chrono::seconds(30));

  CHECK(refined.simplified_blocks >= 1);
  CHECK(layout.at({1, 1, 0}).type == BlockType::Barrel);
  CHECK(layout.at({1, 1, 0}).stored_signal() == 5);
  CHECK_FALSE(verify_layout(layout, spec, vectors));
}

TEST_CASE("signal loops forbidden lazily still give the smallest NOT gate") {
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
palette solid glass dust torch repeater comparator
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  SolverOptions options = test_options();
  options.lazy_loops = true;
  SolveReport report = solve(spec, options);
  REQUIRE(report.status == SolveStatus::Optimal);
  REQUIRE(report.solution);
  CHECK(report.solution->block_count == 4);
}

TEST_CASE("signal loops forbidden lazily: a half adder verifies") {
  Spec spec = parse_spec(R"(
size 6 2 3
input IN1 repeater 0 1 0 east
input IN2 repeater 0 1 2 east
output OUT1 dust 5 1 2
output OUT2 dust 5 1 0
timing none
settle 4
horizon 80
palette solid dust torch repeater comparator target barrel
row IN1(ss0),IN2(ss0) -> OUT1(ss0),OUT2(ss0)
row IN1(ss15),IN2(ss0) -> OUT1(ss1..15),OUT2(ss0)
row IN1(ss0),IN2(ss15) -> OUT1(ss1..15),OUT2(ss0)
row IN1(ss15),IN2(ss15) -> OUT1(ss0),OUT2(ss1..15)
)");
  SolverOptions options = test_options();
  options.lazy_loops = true;
  options.stop_at_first = true;
  options.verification.placements = {Placement{}};
  SolveReport report = solve(spec, options);
  REQUIRE(report.solution);
  CHECK_FALSE(verify_layout(report.solution->layout, spec,
                            spec.verification_vectors(), options.verification));
}

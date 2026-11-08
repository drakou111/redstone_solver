#include <format>
#include <string>
#include <utility>

#include "redstone/shrink.hpp"
#include "redstone/simulator.hpp"
#include "redstone/verifier.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

constexpr double kTimeLimitSeconds = 60.0;

// A NOT gate with room to spare along x.
constexpr const char *kWideNot = R"(
size 7 2 1
input IN1 comparator 0 1 0 east
output OUT1 dust 6 1 0
horizon 12
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";

} // namespace

TEST_CASE("cutting a slice moves everything beyond it back by one") {
  Spec spec = parse_spec(std::string(kWideNot) +
                         "keep 1 1 0 solid\nempty 3 1 0\nhint 5 1 0 dust\n");
  std::optional<Spec> cut = cut_slice(spec, 0, 2);
  REQUIRE(cut);
  CHECK(cut->size == Vec3{6, 2, 1});
  CHECK(cut->inputs[0].position == Vec3{0, 1, 0});
  CHECK(cut->outputs[0].position == Vec3{5, 1, 0});
  REQUIRE(cut->fixed.size() == 1);
  CHECK(cut->fixed[0].first == Vec3{1, 1, 0});
  REQUIRE(cut->forbidden.size() == 1);
  CHECK(cut->forbidden[0] == Vec3{2, 1, 0});
  REQUIRE(cut->hints.size() == 1);
  CHECK(cut->hints[0].first == Vec3{4, 1, 0});

  // A kept block in the slice simply goes with it.
  std::optional<Spec> through_kept = cut_slice(spec, 0, 1);
  REQUIRE(through_kept);
  CHECK(through_kept->fixed.empty());
}

TEST_CASE("a slice through a fixed port, or a free port's whole region, is not "
          "cut") {
  Spec spec = parse_spec(kWideNot);
  CHECK_FALSE(cut_slice(spec, 0, 0));
  CHECK_FALSE(cut_slice(spec, 0, 6));
  CHECK_FALSE(cut_slice(spec, 1, 1));

  Spec free = parse_spec(R"(
size 7 2 1
input IN1 comparator 0 1 0 east
output OUT1 dust free
region OUT1 6 1 0
horizon 12
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  CHECK_FALSE(cut_slice(free, 0, 6));
  CHECK(cut_slice(free, 0, 3));
}

TEST_CASE("shrink takes slices out of a verified layout until none can go") {
  Spec spec = parse_spec(kWideNot);
  SolverOptions solve_options;
  solve_options.time_limit_seconds = kTimeLimitSeconds;
  solve_options.stop_at_first = true;
  SolveReport report = solve(spec, solve_options);
  REQUIRE(report.solution);

  ShrinkOptions options;
  options.time_limit_seconds = kTimeLimitSeconds;
  options.cut_seconds = 20.0;
  ShrinkResult result = shrink(spec, report.solution->layout, options);
  CHECK(result.steps > 0);
  CHECK(result.spec.size.x < spec.size.x);
  CHECK(result.layout.size() == result.spec.size);
  CHECK_FALSE(verify_layout(result.layout, result.spec,
                            result.spec.verification_vectors()));
}

TEST_CASE("tidy straightens a wire that takes a detour") {
  // A buffer whose wire goes around a U instead of straight across.
  std::string text = R"(
size 5 2 3
input IN1 repeater 0 1 0 east
output OUT1 dust 4 1 0
horizon 20
row IN1(ss15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
hint 0 0 0 solid
hint 4 0 0 solid
)";
  for (auto [x, z] :
       {std::pair{1, 0}, {1, 1}, {1, 2}, {2, 2}, {3, 2}, {3, 1}, {3, 0}})
    text += std::format("hint {} 1 {} dust\nhint {} 0 {} solid\n", x, z, x, z);
  Spec spec = parse_spec(text);
  World layout = spec.base_world();
  for (const auto &[p, block] : spec.hints)
    layout.set(p, block);
  REQUIRE_FALSE(verify_layout(layout, spec, spec.verification_vectors()));
  const int before = layout_blocks(layout);

  ShrinkOptions options;
  options.time_limit_seconds = kTimeLimitSeconds;
  ShrinkResult result = tidy(spec, layout, options);
  CHECK(result.steps > 0);
  CHECK(layout_blocks(result.layout) < before);
  CHECK(result.layout.size() == layout.size());
  CHECK_FALSE(verify_layout(result.layout, result.spec,
                            result.spec.verification_vectors()));
}

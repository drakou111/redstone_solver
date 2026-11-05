#include <string>

#include "redstone/layout_check.hpp"
#include "redstone/spec.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

constexpr const char *kNotGate = R"(
size 4 2 1
input IN1 comparator 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
hint 0 0 0 solid
hint 3 0 0 solid
hint 1 1 0 solid
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";

} // namespace

TEST_CASE("a working layout drawn with hint lines passes the check") {
  Spec spec = parse_spec(std::string(kNotGate) + "hint 2 1 0 torch west\n");
  LayoutCheck check = check_layout(layout_from_hints(spec), spec);
  CHECK(check.passes());
  CHECK(check.settled.size() == spec.verification_vectors().size());
  std::string text = format_check(check, spec);
  CHECK(text.find("PASS") != std::string::npos);
  CHECK(text.find("FAIL") == std::string::npos);
}

TEST_CASE("the check reports every wrong row, not just the first") {
  // Without the torch the output never lights: every row expecting a signal
  // fails, the others pass.
  Spec spec = parse_spec(kNotGate);
  LayoutCheck check = check_layout(layout_from_hints(spec), spec);
  CHECK_FALSE(check.passes());
  int failing = 0;
  for (const SettledCheck &s : check.settled)
    failing += s.ok ? 0 : 1;
  CHECK(failing == 1);
  CHECK(format_check(check, spec).find("NOT PASSING") != std::string::npos);
}

TEST_CASE("the check names blocks placed against the rules") {
  // A torch hanging on air.
  Spec spec = parse_spec(std::string(kNotGate) + "hint 2 1 0 torch east\n");
  LayoutCheck check = check_layout(layout_from_hints(spec), spec);
  CHECK_FALSE(check.invalid.empty());
  CHECK(format_check(check, spec).starts_with("INVALID LAYOUT"));
}

TEST_CASE("a hint on a port cell is refused") {
  CHECK_THROWS_AS(parse_spec(std::string(kNotGate) + "hint 3 1 0 solid\n"),
                  std::invalid_argument);
}

TEST_CASE("a signal loop through a comparator is reported unless feedback "
          "is on") {
  // The comparator's output dust runs round to its own side input.
  const std::string base = R"(
size 4 2 3
input IN1 repeater 0 1 1 east
output OUT1 dust 3 1 1
horizon 10
hint 0 0 0 solid
hint 1 0 0 solid
hint 2 0 0 solid
hint 0 0 1 solid
hint 1 0 1 solid
hint 2 0 1 solid
hint 3 0 1 solid
hint 1 1 1 comparator east subtract
hint 2 1 1 dust
hint 2 1 0 dust
row IN1(ss15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)";
  Spec open = parse_spec(base);
  CHECK(solver_can_express(layout_from_hints(open), open));

  Spec loop = parse_spec(base + "hint 1 1 0 dust\n");
  World layout = layout_from_hints(loop);
  CHECK_FALSE(solver_can_express(layout, loop));
  LayoutCheck check = check_layout(layout, loop);
  CHECK_FALSE(check.passes());
  CHECK(format_check(check, loop).find("loop") != std::string::npos);

  Spec allowed = parse_spec(base + "hint 1 1 0 dust\nfeedback on\n");
  CHECK(solver_can_express(layout_from_hints(allowed), allowed));
}

TEST_CASE("a block beside a repeater's side is not a loop") {
  // The repeater powers S; a torch on S lights B above it, B powers dust on
  // X, and X sits beside the repeater. Only diodes facing into its side
  // could lock the repeater, so nothing here feeds back.
  Spec spec = parse_spec(R"(
size 4 3 2
input IN1 repeater 0 1 1 east
output OUT1 dust 3 1 1
horizon 10
hint 0 0 1 solid
hint 1 0 1 solid
hint 3 0 1 solid
hint 1 1 1 repeater east 1
hint 2 1 1 solid
hint 2 1 0 torch south
hint 2 2 0 solid
hint 1 1 0 solid
hint 1 2 0 dust
row IN1(ss15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)");
  World layout = layout_from_hints(spec);
  CHECK(layout.validate().empty());
  CHECK(solver_can_express(layout, spec));
  CHECK(check_layout(layout, spec).passes());
}

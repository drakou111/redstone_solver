#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "redstone/spec.hpp"

using namespace rs;

namespace {

constexpr const char *kAndSpec = R"(
# 2-input AND
size 4 3 3
input IN1 strong 0 1 0 east
input IN2 strong 0 1 2 east
output OUT1 dust 3 1 1
timing 2rt 6
hold_previous on
palette solid dust torch
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
)";

} // namespace

TEST_CASE("truth rows parse ranges and default unlisted ports to anything") {
  TruthRow row = parse_truth_row("IN2(ss3), IN1(ss1..15) -> OUT1(ss0)", 3, 2);
  CHECK(row.inputs[0] == SignalRange{1, 15});
  CHECK(row.inputs[1] == SignalRange{3, 3});
  CHECK(row.inputs[2] == SignalRange{0, 15});
  CHECK(row.outputs[0] == SignalRange{0, 0});
  CHECK(row.outputs[1] == SignalRange{0, 15});
}

TEST_CASE("truth rows reject malformed text") {
  CHECK_THROWS_AS(parse_truth_row("IN1(ss1) OUT1(ss0)", 1, 1),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_truth_row("IN3(ss1) -> OUT1(ss0)", 1, 1),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_truth_row("IN1(ss16) -> OUT1(ss0)", 1, 1),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_truth_row("IN1(ss5..2) -> OUT1(ss0)", 1, 1),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_truth_row("IN1(ss1),IN1(ss2) -> OUT1(ss0)", 1, 1),
                  std::invalid_argument);
}

TEST_CASE("ticks accept game and redstone tick units") {
  CHECK(parse_ticks("12") == 12);
  CHECK(parse_ticks("12gt") == 12);
  CHECK(parse_ticks("3rt") == 6);
  CHECK_THROWS_AS(parse_ticks("x"), std::invalid_argument);
}

TEST_CASE("spec files parse into ports, timing, palette and rows") {
  Spec spec = parse_spec(kAndSpec);
  CHECK(spec.size == Vec3{4, 3, 3});
  REQUIRE(spec.inputs.size() == 2);
  CHECK(spec.inputs[1].position == Vec3{0, 1, 2});
  CHECK(spec.inputs[1].facing == Dir::East);
  CHECK(spec.outputs[0].kind == OutputKind::Dust);
  CHECK(spec.timing.earliest == 4);
  CHECK(spec.timing.latest == 6);
  CHECK(spec.timing.hold_previous);
  CHECK(spec.timing.horizon() == 10);
  CHECK(spec.palette ==
        std::vector{BlockType::Solid, BlockType::Dust, BlockType::Torch});
  CHECK(spec.rows.size() == 3);
}

TEST_CASE("expected outputs intersect every matching row") {
  Spec spec = parse_spec(kAndSpec);
  CHECK(spec.expected_outputs({5, 9}) == std::vector{SignalRange{1, 15}});
  CHECK(spec.expected_outputs({0, 0}) == std::vector{SignalRange{0, 0}});
}

TEST_CASE("verification vectors cover min, middle and max of each range") {
  Spec spec = parse_spec(kAndSpec);
  std::vector<InputVector> vectors = spec.verification_vectors();
  auto has = [&](InputVector v) {
    return std::find(vectors.begin(), vectors.end(), v) != vectors.end();
  };
  CHECK(has({1, 1}));
  CHECK(has({8, 15}));
  CHECK(has({0, 7}));
  CHECK(has({15, 0}));
}

TEST_CASE("representative vectors take each row's minimum and maximum, "
          "without duplicates") {
  Spec spec = parse_spec(kAndSpec);
  CHECK(spec.representative_vectors() ==
        std::vector<InputVector>{{1, 1}, {15, 15}, {0, 0}, {0, 15}, {15, 0}});
}

TEST_CASE("spec validation reports contradictions and bad ports") {
  CHECK_THROWS_AS(parse_spec("size 2 1 1\ninput IN1 strong 0 0 0 east\noutput "
                             "OUT1 dust 0 0 0\nrow -> OUT1(ss0)\n"),
                  std::invalid_argument);
  CHECK_THROWS_AS(
      parse_spec(
          "size 2 1 1\ninput IN1 strong 0 0 0 east\noutput OUT1 dust 1 0 0\n"
          "row IN1(ss1..15) -> OUT1(ss0)\nrow IN1(ss5) -> OUT1(ss3)\n"),
      std::invalid_argument);
  CHECK_THROWS_AS(parse_spec("size 2 1 1\ninput IN1 strong 0 0 0\noutput OUT1 "
                             "dust 1 0 0\nrow -> OUT1(ss0)\n"),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_spec("size 2 1 1\nbogus 1\n"), std::invalid_argument);
}

TEST_CASE("tile and feedback settings parse") {
  Spec spec = parse_spec("size 2 2 2\ninput IN1 strong 0 1 0 east\n"
                         "output OUT1 dust 1 1 0\ntile x z\nfeedback on\n"
                         "row -> OUT1(ss0..15)\n");
  CHECK(spec.tiled == std::array{true, false, true});
  CHECK(spec.is_tiled());
  CHECK(spec.allow_feedback);
  CHECK_THROWS_AS(parse_spec("size 2 2 2\ntile w\n"), std::invalid_argument);
}

TEST_CASE("format_spec round-trips through parse_spec") {
  Spec spec = parse_spec(kAndSpec);
  spec.tiled = {false, false, true};
  spec.allow_feedback = true;
  Spec again = parse_spec(format_spec(spec));
  CHECK(again.size == spec.size);
  CHECK(again.inputs.size() == spec.inputs.size());
  CHECK(again.inputs[1].position == spec.inputs[1].position);
  CHECK(again.outputs[0].kind == spec.outputs[0].kind);
  CHECK(again.timing.earliest == spec.timing.earliest);
  CHECK(again.timing.latest == spec.timing.latest);
  CHECK(again.timing.hold_previous == spec.timing.hold_previous);
  CHECK(again.palette == spec.palette);
  CHECK(again.tiled == spec.tiled);
  CHECK(again.allow_feedback);
  REQUIRE(again.rows.size() == spec.rows.size());
  for (std::size_t r = 0; r < spec.rows.size(); ++r) {
    CHECK(again.rows[r].inputs == spec.rows[r].inputs);
    CHECK(again.rows[r].outputs == spec.rows[r].outputs);
  }
}

TEST_CASE("signal ranges parse from editor text") {
  CHECK(parse_signal_range("") == SignalRange{0, 15});
  CHECK(parse_signal_range(" * ") == SignalRange{0, 15});
  CHECK(parse_signal_range("7") == SignalRange{7, 7});
  CHECK(parse_signal_range("1..15") == SignalRange{1, 15});
  CHECK(parse_signal_range("ss2..3") == SignalRange{2, 3});
  CHECK_THROWS_AS(parse_signal_range("abc"), std::invalid_argument);
  CHECK(short_range_text({0, 15}) == "*");
  CHECK(short_range_text({4, 4}) == "4");
  CHECK(short_range_text({1, 15}) == "1..15");
}

TEST_CASE("port kinds parse, including the old names") {
  Spec spec = parse_spec("size 4 2 2\n"
                         "input IN1 strong 0 1 0 east\n"
                         "input IN2 dust 0 1 1\n"
                         "output OUT1 component 3 1 0 east\n"
                         "output OUT2 comparator 3 1 1 east\n"
                         "row -> OUT1(ss0..15)\n");
  CHECK(spec.inputs[0].kind == InputKind::Comparator);
  CHECK(spec.inputs[1].kind == InputKind::Dust);
  CHECK(spec.outputs[0].kind == OutputKind::Repeater);
  CHECK(spec.outputs[1].kind == OutputKind::Comparator);
  CHECK_THROWS_AS(parse_spec("size 2 2 1\ninput IN1 repeater 0 1 0\n"
                             "output OUT1 dust 1 1 0\nrow -> OUT1(ss0)\n"),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_spec("size 2 2 1\ninput IN1 lever 0 1 0\n"),
                  std::invalid_argument);
}

TEST_CASE("kept and empty cells parse, validate and round-trip") {
  Spec spec = parse_spec("size 4 2 1\n"
                         "input IN1 comparator 0 1 0 east\n"
                         "output OUT1 dust 3 1 0\n"
                         "keep 1 1 0 repeater east 2\n"
                         "keep 1 0 0 barrel 7\n"
                         "empty 2 1 0\n"
                         "row -> OUT1(ss0..15)\n");
  REQUIRE(spec.fixed.size() == 2);
  CHECK(spec.fixed[0].second == Block::repeater(Dir::East, 2));
  CHECK(spec.fixed[1].second == Block::barrel(7));
  CHECK(spec.forbidden == std::vector<Vec3>{{2, 1, 0}});
  CHECK(spec.base_world().at({1, 1, 0}) == Block::repeater(Dir::East, 2));

  Spec again = parse_spec(format_spec(spec));
  CHECK(again.fixed == spec.fixed);
  CHECK(again.forbidden == spec.forbidden);

  CHECK_THROWS_AS(parse_spec("size 2 2 1\ninput IN1 comparator 0 1 0 east\n"
                             "output OUT1 dust 1 1 0\nempty 0 1 0\n"
                             "row -> OUT1(ss0)\n"),
                  std::invalid_argument);
  CHECK_THROWS_AS(parse_spec("size 2 2 1\ninput IN1 comparator 0 1 0 east\n"
                             "output OUT1 dust 1 1 0\nkeep 0 0 0 repeater\n"
                             "row -> OUT1(ss0)\n"),
                  std::invalid_argument);
}

TEST_CASE("hints parse, validate and round-trip") {
  Spec spec = parse_spec("size 4 2 1\n"
                         "input IN1 comparator 0 1 0 east\n"
                         "output OUT1 dust 3 1 0\n"
                         "hint 1 1 0 solid\n"
                         "hint 2 1 0 torch west\n"
                         "row -> OUT1(ss0..15)\n");
  REQUIRE(spec.hints.size() == 2);
  CHECK(spec.hints[1].second == Block::torch(Dir::West));
  // A hint is only a preference: the cell stays free.
  CHECK(spec.base_world().at({1, 1, 0}).type == BlockType::Air);
  CHECK(parse_spec(format_spec(spec)).hints == spec.hints);

  CHECK_THROWS_AS(parse_spec("size 2 2 1\ninput IN1 comparator 0 1 0 east\n"
                             "output OUT1 dust 1 1 0\nhint 5 1 0 solid\n"
                             "row -> OUT1(ss0)\n"),
                  std::invalid_argument);
}

TEST_CASE("free ports take their cells from region lines") {
  Spec spec = parse_spec(R"(
size 4 2 3
input IN1 repeater free
output OUT1 dust 3 1 1
region IN1 0 1 0..2
region IN1 0 1 1
palette solid dust torch
row IN1(ss15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  REQUIRE(spec.inputs[0].is_free());
  CHECK(spec.inputs[0].region ==
        std::vector<Vec3>{{0, 1, 0}, {0, 1, 1}, {0, 1, 2}});
  CHECK_FALSE(spec.outputs[0].is_free());
  // Free ports are not part of the base world.
  CHECK(spec.base_world().at({0, 1, 0}).type == BlockType::Air);

  Spec again = parse_spec(format_spec(spec));
  CHECK(again.inputs[0].region == spec.inputs[0].region);
  CHECK(again.inputs[0].kind == InputKind::Repeater);
}

TEST_CASE("free port regions are checked") {
  auto spec_with = [](const std::string &lines) {
    return "size 4 2 3\ninput IN1 dust free\noutput OUT1 dust 3 1 1\n" + lines +
           "\nrow IN1(ss0) -> OUT1(ss0)\n";
  };
  CHECK_THROWS(parse_spec(spec_with("")));                    // no region
  CHECK_THROWS(parse_spec(spec_with("region IN1 4 1 0")));    // outside
  CHECK_THROWS(parse_spec(spec_with("region IN1 3 1 1")));    // on OUT1
  CHECK_THROWS(parse_spec(spec_with("region IN1 2..1 1 0"))); // backwards
  CHECK_THROWS(parse_spec(spec_with("region IN1 0 1 0\nregion OUT1 0 1 1")));
  CHECK_NOTHROW(parse_spec(spec_with("region IN1 0..1 1 0..2")));
}

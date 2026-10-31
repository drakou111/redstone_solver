#include <catch2/catch_test_macros.hpp>

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

TEST_CASE(
    "representative vectors take each row's minimum, without duplicates") {
  Spec spec = parse_spec(kAndSpec);
  CHECK(spec.representative_vectors() ==
        std::vector<InputVector>{{1, 1}, {0, 0}});
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

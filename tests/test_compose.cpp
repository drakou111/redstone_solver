#include <format>
#include <functional>
#include <string>

#include "redstone/compose.hpp"
#include "redstone/verifier.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

// Rows of an on/off function of `inputs` inputs and `outputs` outputs.
std::string rows_of(int inputs, int outputs,
                    const std::function<int(int, int)> &value) {
  std::string text;
  for (int t = 0; t < (1 << inputs); ++t) {
    text += "row ";
    for (int i = 0; i < inputs; ++i)
      text += std::format("{}IN{}(ss{})", i ? "," : "", i + 1,
                          ((t >> i) & 1) ? 15 : 0);
    text += " ->";
    for (int o = 0; o < outputs; ++o)
      text += std::format("{}OUT{}({})", o ? "," : " ", o + 1,
                          value(t, o) ? "ss1..15" : "ss0");
    text += "\n";
  }
  return text;
}

Spec spec_of(int inputs, int outputs, const std::function<int(int, int)> &f) {
  std::string text = "size 4 2 4\n";
  for (int i = 0; i < inputs; ++i)
    text += std::format("input IN{} repeater 0 1 {} east\n", i + 1, i);
  for (int o = 0; o < outputs; ++o)
    text += std::format("output OUT{} dust 3 1 {}\n", o + 1, o);
  return parse_spec(text + rows_of(inputs, outputs, f));
}

void check_composes(const Spec &spec) {
  NetlistComposeOptions options;
  options.stage_seconds = 120.0;
  std::vector<std::string> log;
  std::optional<ComposeResult> result = compose_spec(
      spec, options, [&](std::string_view m) { log.emplace_back(m); });
  std::string text;
  for (const auto &line : log)
    text += line + "\n";
  INFO(text);
  REQUIRE(result);
  CHECK(result->layout.size() == result->spec.size);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

} // namespace

TEST_CASE("compose wires a single gate") {
  check_composes(spec_of(2, 1, [](int t, int) { return (t & 1) ^ (t >> 1); }));
}

TEST_CASE("compose wires a multiplexer through a pass-through track") {
  check_composes(
      spec_of(3, 1, [](int t, int) { return (t & 4) ? (t >> 1) & 1 : t & 1; }));
}

TEST_CASE("compose wires a full adder from its five gates") {
  check_composes(spec_of(3, 2, [](int t, int o) {
    int count = (t & 1) + ((t >> 1) & 1) + ((t >> 2) & 1);
    return o == 0 ? count % 2 : count >= 2 ? 1 : 0;
  }));
}

TEST_CASE("a height limit keeps only gate shapes that fit") {
  GateLibraryOptions gates;
  GateLibraryOptions flat = gates_within_height(gates, 2);
  REQUIRE_FALSE(flat.sizes.empty());
  for (Vec3 size : flat.sizes)
    CHECK(size.y <= 2);
  CHECK(gates_within_height(gates, 0).sizes.size() == gates.sizes.size());
  CHECK_THROWS_AS(gates_within_height(gates, 1), std::invalid_argument);
}

TEST_CASE("flush placement builds a compact flat full adder") {
  Spec spec = spec_of(3, 2, [](int t, int o) {
    int count = (t & 1) + ((t >> 1) & 1) + ((t >> 2) & 1);
    return o == 0 ? count % 2 : count >= 2 ? 1 : 0;
  });
  NetlistComposeOptions options;
  options.max_height = 2;
  std::vector<std::string> log;
  std::optional<ComposeResult> result = compose_spec(
      spec, options, [&](std::string_view m) { log.emplace_back(m); });
  std::string text;
  for (const auto &line : log)
    text += line + "\n";
  INFO(text);
  REQUIRE(result);
  CHECK(text.find("laying out in columns") == std::string::npos);
  Vec3 size = result->layout.size();
  CHECK(size.y <= 2);
  // The column layout needs 27x3x7; flush placement gave 11x2x8.
  CHECK(size.x * size.z <= 100);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

TEST_CASE("compose and shrink keeps the smaller variant, verified") {
  Spec spec = spec_of(3, 2, [](int t, int o) {
    int count = (t & 1) + ((t >> 1) & 1) + ((t >> 2) & 1);
    return o == 0 ? count % 2 : count >= 2 ? 1 : 0;
  });
  NetlistComposeOptions options;
  options.max_height = 2;
  std::optional<ComposeResult> plain = compose_spec(spec, options);
  REQUIRE(plain);

  ShrinkOptions shrink;
  shrink.time_limit_seconds = 40;
  shrink.cut_seconds = 15;
  std::vector<std::string> log;
  std::optional<ComposeResult> result = compose_and_shrink(
      spec, options, shrink, [&](std::string_view m) { log.emplace_back(m); });
  std::string text;
  for (const auto &line : log)
    text += line + "\n";
  INFO(text);
  REQUIRE(result);
  CHECK(text.find("ORs as joined wires") != std::string::npos);
  CHECK(text.find("ORs as modules") != std::string::npos);
  Vec3 a = result->layout.size();
  Vec3 b = plain->layout.size();
  CHECK(a.x * a.y * a.z <= b.x * b.y * b.z);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

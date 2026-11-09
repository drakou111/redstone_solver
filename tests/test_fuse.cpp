#include <format>
#include <string>

#include "redstone/compose_parts.hpp"
#include "redstone/verifier.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;
using namespace rs::compose_parts;

namespace {

constexpr std::uint8_t kXor = 0b0110;
constexpr std::uint8_t kAnd = 0b1000;
constexpr std::uint8_t kOr = 0b1110;
constexpr std::uint8_t kOr3 = 0xFE;

// sum = (a XOR b) XOR c, carry = (a AND b) OR ((a XOR b) AND c).
Netlist full_adder() {
  return {3,
          {{0, 1, kXor}, {3, 2, kXor}, {0, 1, kAnd}, {3, 2, kAnd}, {5, 6, kOr}},
          {4, 7}};
}

} // namespace

TEST_CASE("two chained gates fuse into one 3-input piece") {
  Netlist xor3{3, {{0, 1, kXor}, {3, 2, kXor}}, {4}};
  std::vector<std::vector<Group>> groupings = fused_groupings(
      xor3, [](std::uint8_t f) { return f == kXor3Function; }, 4);
  REQUIRE(groupings.size() == 1);
  REQUIRE(groupings[0].size() == 1);
  const Group &piece = groupings[0][0];
  CHECK(piece.operands == std::vector<int>{0, 1, 2});
  CHECK(piece.functions == std::vector<std::uint8_t>{kXor3Function});
  CHECK(piece.nodes == std::vector<int>{4});
  CHECK(group_name(piece) == "XOR3");
  CHECK(fused_groupings(xor3, [](std::uint8_t) { return false; }, 4).empty());
}

TEST_CASE("a full adder fuses into a 3-input XOR and a majority") {
  std::vector<std::vector<Group>> groupings = fused_groupings(
      full_adder(),
      [](std::uint8_t f) {
        return f == kXor3Function || f == kMajorityFunction;
      },
      4);
  REQUIRE_FALSE(groupings.empty());
  const std::vector<Group> &best = groupings[0];
  REQUIRE(best.size() == 2);
  for (const Group &group : best)
    CHECK(group.operands == std::vector<int>{0, 1, 2});
  CHECK(best[0].functions[0] != best[1].functions[0]);
}

TEST_CASE("a piece whose gates are needed anyway is not fused") {
  // The sum as one XOR3 still leaves a XOR b for the carry: nothing saved.
  CHECK(fused_groupings(
            full_adder(), [](std::uint8_t f) { return f == kXor3Function; }, 4)
            .empty());
}

TEST_CASE("flush placement builds a fused piece from a catalog module") {
  GateLibraryOptions gates;
  gates.time_limit_seconds = 30.0;
  GateArrangement arrangement{{4, 2, 5}, {0, 2, 4}, {2}};
  std::optional<GateModule> module = build_gate_in({kOr3}, arrangement, gates);
  REQUIRE(module);
  GateCatalog catalog;
  NetlistComposeOptions options;
  options.max_height = 2;
  catalog.add(catalog_key(options.palette, options.solver.verification),
              *module);
  options.catalog = &catalog;

  Netlist or3{3, {{0, 1, kOr}, {3, 2, kOr}}, {4}};
  std::vector<std::vector<Group>> groupings =
      fused_groupings(or3, [](std::uint8_t f) { return f == kOr3; }, 4);
  REQUIRE(groupings.size() == 1);
  BoolTable table;
  table.inputs = 3;
  table.values.resize(1);
  for (int t = 0; t < 8; ++t)
    table.values[0].push_back(t != 0);
  std::vector<std::string> log;
  std::optional<ComposeResult> result =
      compose_flush_groups(or3, table, groupings[0], options,
                           [&](std::string_view m) { log.emplace_back(m); });
  std::string text;
  for (const auto &line : log)
    text += line + "\n";
  INFO(text);
  REQUIRE(result);
  CHECK(text.find("OR3") == std::string::npos); // named as a 3-input piece
  CHECK(text.find("3-input 254") != std::string::npos);
  CHECK(result->layout.size().y <= 2);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

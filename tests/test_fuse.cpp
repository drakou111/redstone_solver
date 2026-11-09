#include <format>
#include <functional>
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

// A catalog of single-output pieces: those `has` says yes to.
std::function<bool(const std::vector<std::uint8_t> &)>
single(std::function<bool(std::uint8_t)> has) {
  return [has](const std::vector<std::uint8_t> &functions) {
    return functions.size() == 1 && has(functions[0]);
  };
}

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
      xor3, single([](std::uint8_t f) { return f == kXor3Function; }), 4);
  REQUIRE(groupings.size() == 1);
  REQUIRE(groupings[0].size() == 1);
  const Group &piece = groupings[0][0];
  CHECK(piece.operands == std::vector<int>{0, 1, 2});
  CHECK(piece.functions == std::vector<std::uint8_t>{kXor3Function});
  CHECK(piece.nodes == std::vector<int>{4});
  CHECK(group_name(piece) == "XOR3");
  CHECK(fused_groupings(xor3, single([](std::uint8_t) { return false; }), 4)
            .empty());
}

TEST_CASE("a full adder fuses into a 3-input XOR and a majority") {
  std::vector<std::vector<Group>> groupings =
      fused_groupings(full_adder(), single([](std::uint8_t f) {
                        return f == kXor3Function || f == kMajorityFunction;
                      }),
                      4);
  REQUIRE_FALSE(groupings.empty());
  const std::vector<Group> &best = groupings[0];
  REQUIRE(best.size() == 2);
  for (const Group &group : best)
    CHECK(group.operands == std::vector<int>{0, 1, 2});
  CHECK(best[0].functions[0] != best[1].functions[0]);
}

TEST_CASE("a fused piece is also placed after the gates it shares inputs "
          "with") {
  // Only the carry fuses: a XOR b, then sum, then the majority.
  std::vector<std::vector<Group>> groupings = fused_groupings(
      full_adder(),
      single([](std::uint8_t f) { return f == kMajorityFunction; }), 4);
  REQUIRE(groupings.size() == 2);
  // In signal order the majority (one step from the inputs) comes second.
  REQUIRE(groupings[0].size() == 3);
  CHECK(groupings[0][1].functions[0] == kMajorityFunction);
  // Placed late it comes last, after the sum.
  REQUIRE(groupings[1].size() == 3);
  CHECK(groupings[1][2].functions[0] == kMajorityFunction);
  CHECK(groupings[1][1].nodes == std::vector<int>{4});
}

TEST_CASE("a piece whose gates are needed anyway is not fused") {
  // The sum as one XOR3 still leaves a XOR b for the carry: nothing saved.
  CHECK(fused_groupings(
            full_adder(),
            single([](std::uint8_t f) { return f == kXor3Function; }), 4)
            .empty());
}

TEST_CASE("flush placement builds a fused piece from a catalog module") {
  GateLibraryOptions gates;
  gates.time_limit_seconds = 30.0;
  GateArrangement arrangement{
      {4, 2, 5}, {{0, 1, 0}, {0, 1, 2}, {0, 1, 4}}, {{3, 1, 2}}};
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
      fused_groupings(or3, single([](std::uint8_t f) { return f == kOr3; }), 4);
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

TEST_CASE("flush placement wires a module input on its north face") {
  GateLibraryOptions gates;
  gates.time_limit_seconds = 30.0;
  // OR3 with its third input on the north face.
  GateArrangement arrangement{
      {4, 2, 5}, {{0, 1, 2}, {0, 1, 4}, {2, 1, 0}}, {{3, 1, 2}}};
  std::optional<GateModule> module = build_gate_in({kOr3}, arrangement, gates);
  REQUIRE(module);
  GateCatalog catalog;
  NetlistComposeOptions options;
  options.max_height = 3;
  catalog.add(catalog_key(options.palette, options.solver.verification),
              *module);
  options.catalog = &catalog;

  Netlist or3{3, {{0, 1, kOr}, {3, 2, kOr}}, {4}};
  std::vector<std::vector<Group>> groupings =
      fused_groupings(or3, single([](std::uint8_t f) { return f == kOr3; }), 4);
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
  CHECK(text.find("3-input 254") != std::string::npos);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

TEST_CASE("flush placement reads a module output on its south face") {
  GateLibraryOptions gates;
  gates.time_limit_seconds = 30.0;
  // (a OR b OR c) AND d, the OR3 a catalog piece whose output leaves south.
  GateArrangement arrangement{
      {4, 2, 5}, {{0, 1, 0}, {0, 1, 1}, {0, 1, 2}}, {{2, 1, 4}}};
  std::optional<GateModule> module = build_gate_in({kOr3}, arrangement, gates);
  REQUIRE(module);
  GateCatalog catalog;
  NetlistComposeOptions options;
  options.max_height = 3;
  catalog.add(catalog_key(options.palette, options.solver.verification),
              *module);
  options.catalog = &catalog;

  Netlist netlist{4, {{0, 1, kOr}, {4, 2, kOr}, {5, 3, kAnd}}, {6}};
  std::vector<std::vector<Group>> groupings = fused_groupings(
      netlist, single([](std::uint8_t f) { return f == kOr3; }), 4);
  REQUIRE_FALSE(groupings.empty());
  BoolTable table;
  table.inputs = 4;
  table.values.resize(1);
  for (int t = 0; t < 16; ++t)
    table.values[0].push_back((t & 7) != 0 && (t & 8) != 0);
  std::vector<std::string> log;
  std::optional<ComposeResult> result =
      compose_flush_groups(netlist, table, groupings[0], options,
                           [&](std::string_view m) { log.emplace_back(m); });
  std::string text;
  for (const auto &line : log)
    text += line + "\n";
  INFO(text);
  REQUIRE(result);
  CHECK(text.find("3-input 254") != std::string::npos);
  CHECK_FALSE(verify_layout(result->layout, result->spec,
                            result->spec.verification_vectors()));
}

TEST_CASE("a gate that reads a piece's signals joins it as an output") {
  // The solver's full adder: x = b ^ c, y = b & c, sum = a ^ x,
  // carry = y ^ (a & x). The carry is a piece over (a, x, y); the sum reads
  // a and x only, so it joins: two modules in all.
  Netlist adder{
      3,
      {{1, 2, kXor}, {1, 2, kAnd}, {0, 3, kAnd}, {0, 3, kXor}, {4, 5, kXor}},
      {6, 7}};
  const std::uint8_t kCarry = 0x6A; // (a & x) ^ y
  const std::uint8_t kSum = 0x3C;   // a ^ x
  std::vector<std::vector<std::uint8_t>> asked;
  std::vector<std::vector<Group>> groupings = fused_groupings(
      adder,
      [&](const std::vector<std::uint8_t> &functions) {
        asked.push_back(functions);
        return functions == std::vector<std::uint8_t>{kCarry} ||
               functions == std::vector<std::uint8_t>{kCarry, kSum};
      },
      4);
  REQUIRE_FALSE(groupings.empty());
  const std::vector<Group> &best = groupings[0];
  REQUIRE(best.size() == 2);
  CHECK(best[0].operands == std::vector<int>{1, 2});
  CHECK(best[0].nodes == std::vector<int>{3, 4});
  CHECK(best[1].operands == std::vector<int>{0, 3, 4});
  CHECK(best[1].nodes == std::vector<int>{7, 6});
  CHECK(best[1].functions == std::vector<std::uint8_t>{kCarry, kSum});
  // Without a two-output module the sum stays a gate of its own.
  std::vector<std::vector<Group>> alone = fused_groupings(
      adder, single([&](std::uint8_t f) { return f == kCarry; }), 4);
  REQUIRE_FALSE(alone.empty());
  CHECK(alone[0].size() == 3);
}

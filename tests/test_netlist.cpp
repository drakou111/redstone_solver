#include <algorithm>
#include <string>

#include "redstone/netlist.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

BoolTable table_of(int inputs, const std::vector<std::vector<int>> &outputs) {
  BoolTable table;
  table.inputs = inputs;
  for (const auto &column : outputs) {
    std::vector<std::optional<bool>> values;
    for (int v : column)
      values.push_back(v < 0 ? std::nullopt : std::optional<bool>(v == 1));
    table.values.push_back(values);
  }
  return table;
}

void check_computes(const Netlist &netlist, const BoolTable &table) {
  for (int t = 0; t < (1 << table.inputs); ++t) {
    std::vector<bool> v = netlist.evaluate(t);
    for (std::size_t o = 0; o < table.values.size(); ++o)
      if (table.values[o][t])
        CHECK(v[netlist.outputs[o]] == *table.values[o][t]);
  }
}

} // namespace

TEST_CASE("synthesis finds the smallest gate networks") {
  BoolTable xor2 = table_of(2, {{0, 1, 1, 0}});
  std::optional<Netlist> x = synthesize(xor2);
  REQUIRE(x);
  CHECK(x->gates.size() == 1);
  CHECK(function_name(x->gates[0].function) == "XOR");

  // Sum and carry of three inputs: five gates (two half adders and an OR).
  BoolTable adder =
      table_of(3, {{0, 1, 1, 0, 1, 0, 0, 1}, {0, 0, 0, 1, 0, 1, 1, 1}});
  std::optional<Netlist> fa = synthesize(adder);
  REQUIRE(fa);
  CHECK(fa->gates.size() == 5);
  check_computes(*fa, adder);
  INFO(fa->describe());
}

TEST_CASE("synthesis uses don't-cares and reads inputs directly") {
  // OUT1 = IN1 wherever it matters: no gate needed.
  BoolTable copy = table_of(2, {{0, 1, -1, -1}});
  std::optional<Netlist> c = synthesize(copy);
  REQUIRE(c);
  CHECK(c->gates.empty());
  CHECK(c->outputs[0] == 0);
}

TEST_CASE("the boolean table of a spec rejects exact strengths") {
  Spec ok = parse_spec(R"(
size 4 2 3
input IN1 repeater 0 1 0 east
input IN2 repeater 0 1 2 east
output OUT1 dust 3 1 1
row IN1(ss1..15),IN2(ss0) -> OUT1(ss1..15)
row IN1(ss0),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0),IN2(ss0) -> OUT1(ss0)
)");
  BoolTable t = boolean_table(ok);
  CHECK(t.values[0][0] == false);
  CHECK(t.values[0][1] == true);
  CHECK(t.values[0][2] == true);
  CHECK_FALSE(t.values[0][3]); // not covered by any row: don't care

  Spec exact = parse_spec(R"(
size 4 2 3
input IN1 repeater 0 1 0 east
output OUT1 dust 3 1 1
row IN1(ss15) -> OUT1(ss7)
row IN1(ss0) -> OUT1(ss0)
)");
  CHECK_THROWS_AS(boolean_table(exact), std::invalid_argument);
}

TEST_CASE("synthesis handles a multiplexer and a 4-input comparison") {
  // OUT = IN3 ? IN2 : IN1
  std::vector<int> mux;
  for (int t = 0; t < 8; ++t)
    mux.push_back((t & 4) ? (t >> 1) & 1 : t & 1);
  BoolTable m = table_of(3, {mux});
  std::optional<Netlist> mn = synthesize(m);
  REQUIRE(mn);
  CHECK(mn->gates.size() == 3);
  check_computes(*mn, m);

  // 2-bit equality: (IN1, IN2) == (IN3, IN4)
  std::vector<int> eq;
  for (int t = 0; t < 16; ++t)
    eq.push_back(((t & 3) == (t >> 2)) ? 1 : 0);
  BoolTable e = table_of(4, {eq});
  std::optional<Netlist> en = synthesize(e);
  REQUIRE(en);
  CHECK(en->gates.size() == 3);
  check_computes(*en, e);
}

TEST_CASE("synthesis prefers AND, OR and XOR at the same gate count") {
  BoolTable adder =
      table_of(3, {{0, 1, 1, 0, 1, 0, 0, 1}, {0, 0, 0, 1, 0, 1, 1, 1}});
  std::optional<Netlist> fa = synthesize(adder);
  REQUIRE(fa);
  CHECK(fa->gates.size() == 5);
  for (const Gate &g : fa->gates) {
    std::string name = function_name(g.function);
    CHECK((name == "AND" || name == "OR" || name == "XOR"));
  }
}

TEST_CASE("synthesis keeps the network as shallow as the gate count allows") {
  BoolTable adder =
      table_of(3, {{0, 1, 1, 0, 1, 0, 0, 1}, {0, 0, 0, 1, 0, 1, 1, 1}});
  std::optional<Netlist> fa = synthesize(adder);
  REQUIRE(fa);
  std::vector<int> levels = fa->levels();
  CHECK(*std::max_element(levels.begin(), levels.end()) == 3);
  check_computes(*fa, adder);
}

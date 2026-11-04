#include <random>
#include <vector>

#include "redstone/circuit_gen.hpp"
#include "redstone/spec.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

constexpr const char *kAdderPorts = R"(
size 7 4 5
input IN1 repeater 0 1 0 east
input IN2 repeater 0 1 2 east
input IN3 repeater 0 1 4 east
output OUT1 dust 6 1 1
output OUT2 dust 6 1 3
row IN1(ss15),IN2(ss0),IN3(ss0) -> OUT1(ss1..15),OUT2(ss0)
)";

constexpr const char *kNotGate = R"(
size 4 2 1
input IN1 repeater 0 1 0 east
output OUT1 dust 3 1 0
row IN1(ss15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";

} // namespace

TEST_CASE("every cell class maps back to its block") {
  gen::CellClasses classes;
  for (int i = 0; i < classes.port(); ++i)
    CHECK(classes.of(classes.block(i)) == i);
  CHECK(classes.of(Block::input(0, InputKind::Repeater, Dir::East)) ==
        classes.port());
  CHECK(classes.names().size() == static_cast<std::size_t>(classes.port() + 1));
}

TEST_CASE("on/off vectors count in binary, input 1 first") {
  std::vector<InputVector> vectors = gen::on_off_vectors(2);
  CHECK(vectors ==
        std::vector<InputVector>{
            {0, 0}, {gen::kOn, 0}, {0, gen::kOn}, {gen::kOn, gen::kOn}});
  CHECK(gen::key_of({0, 3, 0, 15}) == 0b1010);
}

TEST_CASE("the truth table of a torch inverter") {
  Spec spec = parse_spec(kNotGate);
  World w = spec.base_world();
  w.set({0, 0, 0}, Block::solid());
  w.set({3, 0, 0}, Block::solid());
  w.set({1, 1, 0}, Block::solid());
  w.set({2, 1, 0}, Block::torch(Dir::West));
  auto table = gen::truth_table(w, gen::on_off_vectors(1), 1);
  REQUIRE(table);
  CHECK((*table)[0] > 0);  // input off: the torch lights the output
  CHECK((*table)[1] == 0); // input on: off

  gen::Target target = gen::target_of(spec, gen::on_off_vectors(1));
  CHECK(target.matches(gen::key_of(*table)));
  CHECK(gen::score_of(*table, target, gen::count_blocks(w)).right == 2);
}

TEST_CASE("grown circuits keep the placement rules and often react") {
  Spec spec = parse_spec(kAdderPorts);
  std::vector<InputVector> vectors = gen::on_off_vectors(3);
  std::mt19937_64 rng(7);
  int reacting = 0;
  constexpr int kCircuits = 200;
  for (int i = 0; i < kCircuits; ++i) {
    World w = gen::grown_layout(spec, rng);
    INFO("circuit " << i);
    REQUIRE(w.validate().empty());
    auto table = gen::truth_table(w, vectors, 2);
    if (table && gen::key_of(*table) != 0 &&
        gen::key_of(*table) != (std::uint64_t{1} << 16) - 1)
      ++reacting;
  }
  // Most grown circuits compute something; random cells almost never do.
  CHECK(reacting > kCircuits / 4);
}

TEST_CASE("mutations never move the ports") {
  Spec spec = parse_spec(kAdderPorts);
  std::mt19937_64 rng(3);
  World w = gen::grown_layout(spec, rng);
  World base = spec.base_world();
  for (int i = 0; i < 500; ++i)
    gen::mutate(w, spec, rng);
  for (int i = 0; i < base.volume(); ++i) {
    Vec3 p = base.position_of(i);
    BlockType t = base.at(p).type;
    if (t == BlockType::Input || t == BlockType::Output)
      CHECK(w.at(p) == base.at(p));
  }
}

TEST_CASE("motif circuits keep the placement rules and the input cells") {
  Spec spec = parse_spec(kAdderPorts);
  std::mt19937_64 rng(11);
  for (int i = 0; i < 200; ++i) {
    World w = gen::motif_layout(spec, rng);
    INFO("circuit " << i);
    REQUIRE(w.validate().empty());
    // The cell in front of each input carries that input's own wire only:
    // never a block placed for something else.
    for (const InputPort &port : spec.inputs) {
      BlockType t = w.at(port.position + offset(port.facing)).type;
      CHECK((t == BlockType::Air || t == BlockType::Dust));
    }
  }
}

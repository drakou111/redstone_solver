#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "redstone/simulator.hpp"

using namespace rs;

namespace {

constexpr int kSettleTicks = 1000;

World with_glass_floor(Vec3 size) {
  World world(size);
  for (int x = 0; x < size.x; ++x)
    for (int z = 0; z < size.z; ++z)
      world.set({x, 0, z}, Block::glass());
  return world;
}

Simulator settled(const World &world, Placement placement = {}) {
  Simulator sim(world, placement);
  REQUIRE(sim.run_until_quiet(kSettleTicks));
  return sim;
}

void advance(Simulator &sim, int ticks) {
  for (int i = 0; i < ticks; ++i)
    sim.tick();
}

// Input 0 -> solid block -> wall torch -> output dust 0.
World not_gate() {
  World world = with_glass_floor({4, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  world.set({1, 1, 0}, Block::solid());
  world.set({2, 1, 0}, Block::torch(Dir::West));
  world.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  return world;
}

// Output 0 sampled after each of `ticks` game ticks.
std::vector<int> output_timeline(Simulator &sim, int ticks) {
  std::vector<int> timeline;
  for (int i = 0; i < ticks; ++i) {
    sim.tick();
    timeline.push_back(sim.output(0));
  }
  return timeline;
}

} // namespace

TEST_CASE("dust loses one signal strength per block") {
  constexpr int kLength = 18;
  World world = with_glass_floor({kLength + 1, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  for (int x = 1; x <= kLength; ++x)
    world.set({x, 1, 0}, Block::dust());
  Simulator sim = settled(world);

  sim.set_input(0, kMaxSignal);
  for (int x = 1; x <= kLength; ++x)
    CHECK(sim.state_at({x, 1, 0}).power == std::max(0, kMaxSignal + 1 - x));

  sim.set_input(0, 10);
  CHECK(sim.state_at({1, 1, 0}).power == 10);
  CHECK(sim.state_at({10, 1, 0}).power == 1);
  CHECK(sim.state_at({11, 1, 0}).power == 0);

  sim.set_input(0, 0);
  for (int x = 1; x <= kLength; ++x)
    CHECK(sim.state_at({x, 1, 0}).power == 0);
  CHECK_FALSE(sim.has_pending_ticks());
}

TEST_CASE("redstone block powers adjacent dust") {
  World world = with_glass_floor({3, 2, 1});
  world.set({0, 1, 0}, Block::redstone_block());
  world.set({1, 1, 0}, Block::dust());
  world.set({2, 1, 0}, Block::dust());
  Simulator sim = settled(world);
  CHECK(sim.state_at({1, 1, 0}).power == 15);
  CHECK(sim.state_at({2, 1, 0}).power == 14);
}

TEST_CASE("torch inverts its attached block with a 2 game tick delay") {
  Simulator sim = settled(not_gate());
  CHECK(sim.output(0) == 15);

  sim.set_input(0, 15);
  CHECK(output_timeline(sim, 3) == std::vector{15, 0, 0});
  CHECK_FALSE(sim.has_pending_ticks());

  sim.set_input(0, 1);
  sim.set_input(0, 0);
  CHECK(output_timeline(sim, 3) == std::vector{0, 15, 15});
}

TEST_CASE("repeater delays by 2 game ticks per step and extends short pulses") {
  World world = with_glass_floor({3, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  world.set({1, 1, 0}, Block::repeater(Dir::East, 3));
  world.set({2, 1, 0}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);

  sim.set_input(0, 15);
  CHECK(output_timeline(sim, 7) == std::vector{0, 0, 0, 0, 0, 15, 15});
  sim.set_input(0, 0);
  CHECK(output_timeline(sim, 7) == std::vector{15, 15, 15, 15, 15, 0, 0});

  // One game tick input pulse: the repeater still outputs for its full delay.
  sim.set_input(0, 15);
  sim.tick();
  sim.set_input(0, 0);
  CHECK(output_timeline(sim, 12) ==
        std::vector{0, 0, 0, 0, 15, 15, 15, 15, 15, 15, 0, 0});
}

TEST_CASE("a repeater powered from the side by a diode locks") {
  World world = with_glass_floor({4, 2, 4});
  world.set({1, 1, 1}, Block::input(0, InputKind::Comparator, Dir::East));
  world.set({2, 1, 1}, Block::repeater(Dir::East, 1));
  world.set({3, 1, 1}, Block::output(0, OutputKind::Dust));
  world.set({2, 1, 3}, Block::input(1, InputKind::Comparator, Dir::North));
  world.set({2, 1, 2}, Block::repeater(Dir::North, 1));
  Simulator sim = settled(world);

  sim.set_input(1, 15);
  advance(sim, 2);
  CHECK(sim.state_at({2, 1, 1}).locked);

  sim.set_input(0, 15);
  advance(sim, 20);
  CHECK(sim.output(0) == 0);

  sim.set_input(1, 0);
  advance(sim, 2);
  CHECK_FALSE(sim.state_at({2, 1, 1}).locked);
  CHECK(sim.output(0) == 0);
  advance(sim, 2);
  CHECK(sim.output(0) == 15);
}

TEST_CASE("comparator reads containers, also through a solid block") {
  World world = with_glass_floor({4, 2, 2});
  world.set({0, 1, 0}, Block::barrel(7));
  world.set({1, 1, 0}, Block::comparator(Dir::East));
  world.set({2, 1, 0}, Block::output(0, OutputKind::Dust));
  world.set({0, 1, 1}, Block::barrel(9));
  world.set({1, 1, 1}, Block::solid());
  world.set({2, 1, 1}, Block::comparator(Dir::East));
  world.set({3, 1, 1}, Block::output(1, OutputKind::Dust));
  Simulator sim = settled(world);
  CHECK(sim.output(0) == 7);
  CHECK(sim.output(1) == 9);
}

TEST_CASE("container input drives a comparator after 2 game ticks") {
  World world = with_glass_floor({3, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Container));
  world.set({1, 1, 0}, Block::comparator(Dir::East));
  world.set({2, 1, 0}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);

  sim.set_input(0, 6);
  CHECK(output_timeline(sim, 3) == std::vector{0, 6, 6});
}

TEST_CASE("comparator compare and subtract modes") {
  auto build = [](ComparatorMode mode) {
    World world = with_glass_floor({3, 2, 2});
    world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
    world.set({1, 1, 0}, Block::comparator(Dir::East, mode));
    world.set({1, 1, 1}, Block::input(1, InputKind::Comparator, Dir::North));
    world.set({2, 1, 0}, Block::output(0, OutputKind::Dust));
    return world;
  };
  auto evaluate = [](Simulator &sim, int rear, int side) {
    sim.set_input(0, rear);
    sim.set_input(1, side);
    REQUIRE(sim.run_until_quiet(kSettleTicks));
    return sim.output(0);
  };

  Simulator subtract = settled(build(ComparatorMode::Subtract));
  CHECK(evaluate(subtract, 12, 5) == 7);
  CHECK(evaluate(subtract, 12, 12) == 0);
  CHECK(evaluate(subtract, 3, 0) == 3);

  Simulator compare = settled(build(ComparatorMode::Compare));
  CHECK(evaluate(compare, 12, 5) == 12);
  CHECK(evaluate(compare, 12, 12) == 12);
  CHECK(evaluate(compare, 12, 13) == 0);
}

TEST_CASE(
    "observer emits a 2 game tick pulse after a watched note block changes") {
  World world = with_glass_floor({2, 2, 3});
  world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  world.set({1, 1, 0}, Block::note_block());
  world.set({1, 1, 1}, Block::observer(Dir::North));
  world.set({1, 1, 2}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);
  CHECK(sim.output(0) == 0);

  sim.set_input(0, 15);
  CHECK(sim.state_at({1, 1, 0}).on);
  CHECK(output_timeline(sim, 6) == std::vector{0, 15, 15, 0, 0, 0});

  sim.set_input(0, 0);
  CHECK(output_timeline(sim, 6) == std::vector{0, 15, 15, 0, 0, 0});
}

TEST_CASE("observer sees a hopper being disabled") {
  World world = with_glass_floor({2, 2, 3});
  world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  world.set({1, 1, 0}, Block::hopper(2));
  world.set({1, 1, 1}, Block::observer(Dir::North));
  world.set({1, 1, 2}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);

  sim.set_input(0, 15);
  CHECK(output_timeline(sim, 4) == std::vector{0, 15, 15, 0});
}

TEST_CASE("dust climbs transparent blocks but does not descend them") {
  // Lower dust at x=1, glass at x=2 with upper dust on top of it.
  auto build = [](Block step, bool power_from_top) {
    World world = with_glass_floor({4, 3, 1});
    world.set({1, 1, 0}, Block::output(0, OutputKind::Dust));
    world.set({2, 1, 0}, step);
    world.set({2, 2, 0}, Block::output(1, OutputKind::Dust));
    if (power_from_top) {
      world.set({3, 1, 0}, Block::glass());
      world.set({3, 2, 0}, Block::input(0, InputKind::Comparator, Dir::West));
    } else
      world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
    return world;
  };

  Simulator up_glass = settled(build(Block::glass(), false));
  up_glass.set_input(0, 15);
  CHECK(up_glass.output(1) == 14);

  Simulator down_glass = settled(build(Block::glass(), true));
  down_glass.set_input(0, 15);
  CHECK(down_glass.output(1) == 15);
  CHECK(down_glass.output(0) == 0);

  Simulator down_solid = settled(build(Block::solid(), true));
  down_solid.set_input(0, 15);
  CHECK(down_solid.output(0) == 14);
}

TEST_CASE("blocks powered by dust power components but not other dust") {
  // Source -> (dust | repeater) -> solid. Dust beyond the solid and a repeater
  // reading it.
  auto build = [](Block feeder) {
    World world = with_glass_floor({4, 2, 3});
    world.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
    world.set({1, 1, 0}, feeder);
    world.set({2, 1, 0}, Block::solid());
    world.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
    world.set({2, 1, 1}, Block::repeater(Dir::South, 1));
    world.set({2, 1, 2}, Block::output(1, OutputKind::Repeater, Dir::South));
    return world;
  };

  Simulator weak = settled(build(Block::dust()));
  weak.set_input(0, 15);
  REQUIRE(weak.run_until_quiet(kSettleTicks));
  CHECK(weak.output(0) == 0);
  CHECK(weak.output(1) == 15);

  Simulator strong = settled(build(Block::repeater(Dir::East, 1)));
  strong.set_input(0, 15);
  REQUIRE(strong.run_until_quiet(kSettleTicks));
  CHECK(strong.output(0) == 15);
  CHECK(strong.output(1) == 15);
}

TEST_CASE("dust running past a block does not power it") {
  World world = with_glass_floor({5, 2, 3});
  world.set({1, 1, 0}, Block::input(0, InputKind::Comparator, Dir::South));
  world.set({1, 1, 1}, Block::dust());
  world.set({1, 1, 2}, Block::dust());
  world.set({2, 1, 1}, Block::solid());
  world.set({3, 1, 1}, Block::torch(Dir::West));
  world.set({4, 1, 1}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);

  sim.set_input(0, 15);
  REQUIRE(sim.run_until_quiet(kSettleTicks));
  CHECK(sim.state_at({1, 1, 1}).power == 15);
  CHECK(sim.output(0) == 15);
}

TEST_CASE("a self-inverting torch burns out after 8 toggles and stays out") {
  // Torch on the side of block B powers the block above it, which powers dust
  // sitting on B.
  World world = with_glass_floor({3, 3, 1});
  world.set({1, 1, 0}, Block::solid());
  world.set({2, 1, 0}, Block::torch(Dir::West));
  world.set({2, 2, 0}, Block::solid());
  world.set({1, 2, 0}, Block::dust());
  Simulator sim(world);

  std::vector<std::int64_t> turned_off;
  bool lit = sim.state_at({2, 1, 0}).on;
  for (int t = 0; t < 400; ++t) {
    sim.tick();
    bool now = sim.state_at({2, 1, 0}).on;
    if (lit && !now)
      turned_off.push_back(sim.time());
    lit = now;
  }
  CHECK(turned_off == std::vector<std::int64_t>{2, 6, 10, 14, 18, 22, 26, 30});
  CHECK_FALSE(lit);
  CHECK_FALSE(sim.has_pending_ticks());
}

TEST_CASE("NOT gate behaves the same at every placement and orientation") {
  Simulator reference = settled(not_gate());
  reference.set_input(0, 15);
  std::vector<int> expected = output_timeline(reference, 5);

  for (Symmetry symmetry : Symmetry::all()) {
    for (Vec3 origin : {Vec3{0, 0, 0}, Vec3{5, 1, 3}, Vec3{-7, 64, 12}}) {
      Simulator sim = settled(not_gate(), Placement{origin, symmetry});
      sim.set_input(0, 15);
      CHECK(output_timeline(sim, 5) == expected);
    }
  }
}

TEST_CASE("simulator rejects invalid worlds and unknown channels") {
  World floating({1, 2, 1});
  floating.set({0, 1, 0}, Block::dust());
  CHECK_THROWS_AS(Simulator(floating), std::invalid_argument);

  Simulator sim = settled(not_gate());
  CHECK_THROWS_AS(sim.set_input(3, 1), std::invalid_argument);
  CHECK_THROWS_AS(sim.set_input(0, 16), std::invalid_argument);
  CHECK_THROWS_AS(sim.output(2), std::invalid_argument);
}

TEST_CASE("repeater input outputs 15 for any non-zero value") {
  World world = with_glass_floor({3, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Repeater, Dir::East));
  world.set({1, 1, 0}, Block::dust());
  world.set({2, 1, 0}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);
  sim.set_input(0, 3);
  CHECK(sim.output(0) == 14);
  sim.set_input(0, 0);
  CHECK(sim.output(0) == 0);
}

TEST_CASE("dust input is held at the input value and decays along dust") {
  World world = with_glass_floor({4, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Dust));
  world.set({1, 1, 0}, Block::dust());
  world.set({2, 1, 0}, Block::dust());
  world.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  Simulator sim = settled(world);
  sim.set_input(0, 9);
  CHECK(sim.state_at({0, 1, 0}).power == 9);
  CHECK(sim.output(0) == 6);
  sim.set_input(0, 0);
  CHECK(sim.output(0) == 0);
}

TEST_CASE("comparator output reads a container through a block") {
  World world = with_glass_floor({3, 2, 1});
  world.set({0, 1, 0}, Block::input(0, InputKind::Container));
  world.set({1, 1, 0}, Block::solid());
  world.set({2, 1, 0}, Block::output(0, OutputKind::Comparator, Dir::East));
  Simulator sim = settled(world);
  sim.set_input(0, 11);
  CHECK(sim.output(0) == 11);
}

TEST_CASE(
    "repeater and comparator ports need support and a horizontal facing") {
  World floating({1, 2, 1});
  floating.set({0, 1, 0}, Block::output(0, OutputKind::Repeater, Dir::East));
  CHECK_FALSE(floating.validate().empty());
  CHECK_THROWS_AS(Block::input(0, InputKind::Repeater, Dir::Up),
                  std::invalid_argument);
}

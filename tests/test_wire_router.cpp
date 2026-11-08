#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "redstone/simulator.hpp"
#include "redstone/wire_router.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

using Cells = WireRouter::Cells;
constexpr int kSettleTicks = 200;

std::array<int, 3> key(Vec3 p) { return {p.x, p.y, p.z}; }

// An input port at (0, 1, from_z) and, at (x, 1, to_z), a repeater facing
// east into a dust output: the wire goes from the port to the cell west of
// the repeater.
struct Board {
  Vec3 size;
  Cells cells;
  Vec3 port;
  WireGoal goal;

  Board(Vec3 size, int from_z, int x, int to_z, int channel = 0)
      : size(size), port{0, 1, from_z}, goal{{x - 1, 1, to_z}, Dir::East} {
    this->add_endpoints(from_z, x, to_z, channel);
  }

  void add_endpoints(int from_z, int x, int to_z, int channel) {
    this->cells[key({0, 1, from_z})] =
        Block::input(channel, InputKind::Repeater, Dir::East);
    this->cells[key({0, 0, from_z})] = Block::solid();
    this->cells[key({x, 1, to_z})] = Block::repeater(Dir::East);
    this->cells[key({x, 0, to_z})] = Block::solid();
    this->cells[key({x + 1, 1, to_z})] =
        Block::output(channel, OutputKind::Dust);
    this->cells[key({x + 1, 0, to_z})] = Block::solid();
  }

  std::optional<std::vector<std::pair<Vec3, Block>>>
  route(Vec3 port_cell, const WireGoal &target) const {
    WireRouterOptions options;
    options.low = {1, 1, 0};
    options.high = {this->size.x - 1, this->size.y - 1, this->size.z - 1};
    WireRouter router(this->cells, [](Vec3) { return false; }, options);
    return router.route({{port_cell, kMaxSignal, Dir::East}}, {}, target);
  }

  void add(const std::vector<std::pair<Vec3, Block>> &blocks) {
    for (const auto &[p, block] : blocks)
      this->cells[key(p)] = block;
  }

  World world() const {
    World w(this->size);
    for (const auto &[cell, block] : this->cells)
      w.set({cell[0], cell[1], cell[2]}, block);
    return w;
  }
};

// The output follows the input, on and off.
void check_follows(const World &world, int channel = 0) {
  Simulator sim(world);
  for (int signal : {kMaxSignal, 0, kMaxSignal}) {
    sim.set_input(channel, signal);
    REQUIRE(sim.run_until_quiet(kSettleTicks));
    CHECK((sim.output(channel) > 0) == (signal > 0));
  }
}

} // namespace

TEST_CASE("the wire router draws a straight wire") {
  Board board({8, 2, 3}, 1, 6, 1);
  auto wire = board.route(board.port, board.goal);
  REQUIRE(wire);
  CHECK(wire->size() == 2 * 5); // dust and block from x 1 to x 5
  board.add(*wire);
  check_follows(board.world());
}

TEST_CASE("the wire router refreshes a long wire with a repeater") {
  Board board({24, 2, 3}, 1, 22, 1);
  auto wire = board.route(board.port, board.goal);
  REQUIRE(wire);
  int repeaters = 0;
  for (const auto &[p, block] : *wire)
    repeaters += block.type == BlockType::Repeater ? 1 : 0;
  CHECK(repeaters >= 1);
  board.add(*wire);
  check_follows(board.world());
}

TEST_CASE("the wire router goes around a wall, keeping clear of it") {
  Board board({12, 2, 9}, 1, 10, 7);
  for (int z = 0; z <= 5; ++z)
    board.cells[key({5, 1, z})] = Block::solid();
  auto wire = board.route(board.port, board.goal);
  REQUIRE(wire);
  for (const auto &[p, block] : *wire)
    for (Dir h : kHorizontalDirs) {
      Vec3 n = p + offset(h);
      bool wall = n.x == 5 && n.y == 1 && n.z <= 5;
      CHECK_FALSE(wall);
    }
  board.add(*wire);
  check_follows(board.world());
}

TEST_CASE("the wire router crosses another wire two levels up") {
  // A second signal runs north to south along x = 5 on the floor.
  Board board({12, 4, 9}, 1, 10, 7);
  board.cells[key({5, 1, 0})] =
      Block::input(1, InputKind::Repeater, Dir::South);
  board.cells[key({5, 0, 0})] = Block::solid();
  for (int z = 1; z <= 7; ++z) {
    board.cells[key({5, 1, z})] = Block::dust();
    board.cells[key({5, 0, z})] = Block::solid();
  }
  board.cells[key({5, 1, 8})] = Block::output(1, OutputKind::Dust);
  board.cells[key({5, 0, 8})] = Block::solid();

  auto wire = board.route(board.port, board.goal);
  REQUIRE(wire);
  board.add(*wire);
  World world = board.world();
  Simulator sim(world);
  for (int a : {0, 1})
    for (int b : {0, 1}) {
      sim.set_input(0, a * kMaxSignal);
      sim.set_input(1, b * kMaxSignal);
      REQUIRE(sim.run_until_quiet(kSettleTicks));
      CHECK((sim.output(0) > 0) == (a == 1));
      CHECK((sim.output(1) > 0) == (b == 1));
    }
}

TEST_CASE("the wire router finds nothing when a wire cannot cross") {
  Board board({12, 2, 9}, 1, 10, 7);
  for (int z = 0; z <= 8; ++z) {
    board.cells[key({5, 1, z})] = Block::dust();
    board.cells[key({5, 0, z})] = Block::solid();
  }
  CHECK_FALSE(board.route(board.port, board.goal));
}

TEST_CASE("the wire router branches off a wire of the same signal") {
  Board board({10, 2, 9}, 1, 8, 1);
  auto first = board.route(board.port, board.goal);
  REQUIRE(first);
  board.add(*first);
  // A second reader of the same input, further south.
  board.cells[key({8, 1, 7})] = Block::repeater(Dir::East);
  board.cells[key({8, 0, 7})] = Block::solid();
  board.cells[key({9, 1, 7})] = Block::output(1, OutputKind::Dust);
  board.cells[key({9, 0, 7})] = Block::solid();

  std::vector<WireStart> starts{{board.port, kMaxSignal, Dir::East}};
  std::set<std::array<int, 3>> own;
  int strength = kMaxSignal;
  for (const auto &[p, block] : *first)
    if (block.type == BlockType::Dust) {
      starts.push_back({p, --strength, std::nullopt});
      own.insert(key(p));
    }
  WireRouterOptions options;
  options.low = {1, 1, 0};
  options.high = {9, 1, 8};
  WireRouter router(board.cells, [](Vec3) { return false; }, options);
  auto second = router.route(starts, own, {{7, 1, 7}, Dir::East});
  REQUIRE(second);
  board.add(*second);
  World world = board.world();
  Simulator sim(world);
  for (int signal : {kMaxSignal, 0}) {
    sim.set_input(0, signal);
    REQUIRE(sim.run_until_quiet(kSettleTicks));
    CHECK((sim.output(0) > 0) == (signal > 0));
    CHECK((sim.output(1) > 0) == (signal > 0));
  }
}

TEST_CASE("the wire router slips a repeater past a wire beside it") {
  // Another input's wire leaves right beside this port and turns south.
  Board board({10, 2, 9}, 1, 8, 1);
  board.cells[key({0, 1, 2})] = Block::input(1, InputKind::Repeater, Dir::East);
  board.cells[key({0, 0, 2})] = Block::solid();
  for (int z = 2; z <= 7; ++z) {
    board.cells[key({1, 1, z})] = Block::dust();
    board.cells[key({1, 0, z})] = Block::solid();
  }
  board.cells[key({1, 1, 8})] = Block::output(1, OutputKind::Dust);
  board.cells[key({1, 0, 8})] = Block::solid();

  auto wire = board.route(board.port, board.goal);
  REQUIRE(wire);
  std::optional<Block> first;
  for (const auto &[p, block] : *wire)
    if (p == Vec3{1, 1, 1})
      first = block;
  REQUIRE(first);
  CHECK(first->type == BlockType::Repeater);
  board.add(*wire);
  World world = board.world();
  Simulator sim(world);
  for (int a : {0, 1})
    for (int b : {0, 1}) {
      sim.set_input(0, a * kMaxSignal);
      sim.set_input(1, b * kMaxSignal);
      REQUIRE(sim.run_until_quiet(kSettleTicks));
      CHECK((sim.output(0) > 0) == (a == 1));
      CHECK((sim.output(1) > 0) == (b == 1));
    }
}

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "redstone/report.hpp"

using namespace rs;

namespace {

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("setblock export uses Java block states and places supports first") {
  World world({5, 2, 2});
  for (int x = 0; x < 5; ++x)
    world.set({x, 0, 0}, Block::solid());
  world.set({0, 1, 0}, Block::input(0, InputKind::Strong, Dir::East));
  world.set({1, 1, 0}, Block::repeater(Dir::East, 3));
  world.set({2, 1, 0}, Block::comparator(Dir::East, ComparatorMode::Subtract));
  world.set({3, 1, 0}, Block::dust());
  world.set({4, 1, 0}, Block::torch(Dir::Down));
  world.set({1, 1, 1}, Block::solid());
  world.set({2, 1, 1}, Block::torch(Dir::West));
  world.set({4, 0, 1}, Block::observer(Dir::Up));

  std::string text = setblock_commands(world);
  CHECK(contains(text, "setblock ~1 ~1 ~0 repeater[facing=west,delay=3]"));
  CHECK(contains(text,
                 "setblock ~2 ~1 ~0 comparator[facing=west,mode=subtract]"));
  CHECK(contains(text, "setblock ~3 ~1 ~0 redstone_wire[north=none,south=none,"
                       "east=side,west=side]"));
  CHECK(contains(text, "setblock ~4 ~1 ~0 redstone_torch\n"));
  CHECK(contains(text, "setblock ~2 ~1 ~1 redstone_wall_torch[facing=east]"));
  CHECK(contains(text, "setblock ~4 ~0 ~1 observer[facing=up]"));
  CHECK(contains(text, "# IN1 at ~0 ~1 ~0"));
  CHECK(text.find("cyan_wool") < text.find("redstone_wire"));
}

TEST_CASE("block codes are short and directional") {
  CHECK(block_code(Block::repeater(Dir::East, 2)) == "re2");
  CHECK(block_code(Block::comparator(Dir::North, ComparatorMode::Subtract)) ==
        "cns");
  CHECK(block_code(Block::solid()) == "#");
}

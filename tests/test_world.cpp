#include <catch2/catch_test_macros.hpp>

#include "redstone/world.hpp"

using namespace rs;

TEST_CASE("positions round-trip through flat indices") {
  World world({3, 4, 5});
  for (int i = 0; i < world.volume(); ++i)
    CHECK(world.index_of(world.position_of(i)) == i);
}

TEST_CASE("outside the box is air and cannot be written") {
  World world({2, 2, 2});
  CHECK(world.at({-1, 0, 0}).type == BlockType::Air);
  CHECK_THROWS_AS(world.set({2, 0, 0}, Block::solid()), std::out_of_range);
}

TEST_CASE("block factories reject invalid settings") {
  CHECK_THROWS_AS(Block::repeater(Dir::Up, 1), std::invalid_argument);
  CHECK_THROWS_AS(Block::repeater(Dir::North, 5), std::invalid_argument);
  CHECK_THROWS_AS(Block::comparator(Dir::Down), std::invalid_argument);
  CHECK_THROWS_AS(Block::torch(Dir::Up), std::invalid_argument);
  CHECK_THROWS_AS(Block::barrel(16), std::invalid_argument);
}

TEST_CASE("validation catches unsupported and conflicting blocks") {
  SECTION("floating dust") {
    World world({1, 2, 1});
    world.set({0, 1, 0}, Block::dust());
    CHECK(world.validate().size() == 1);
  }
  SECTION("dust on glass, hopper and solid is fine") {
    World world({3, 2, 1});
    world.set({0, 0, 0}, Block::glass());
    world.set({1, 0, 0}, Block::hopper());
    world.set({2, 0, 0}, Block::solid());
    for (int x = 0; x < 3; ++x)
      world.set({x, 1, 0}, Block::dust());
    CHECK(world.validate().empty());
  }
  SECTION("dust cannot sit on a chest") {
    World world({1, 2, 1});
    world.set({0, 0, 0}, Block::chest(0));
    world.set({0, 1, 0}, Block::dust());
    CHECK_FALSE(world.validate().empty());
  }
  SECTION("wall torch needs a side it can hang on") {
    World world({2, 1, 1});
    world.set({0, 0, 0}, Block::hopper());
    world.set({1, 0, 0}, Block::torch(Dir::West));
    CHECK_FALSE(world.validate().empty());
    world.set({0, 0, 0}, Block::glass());
    CHECK(world.validate().empty());
  }
  SECTION("adjacent chests are rejected") {
    World world({2, 1, 1});
    world.set({0, 0, 0}, Block::chest(1));
    world.set({1, 0, 0}, Block::chest(1));
    CHECK_FALSE(world.validate().empty());
  }
  SECTION("hoppers may not touch containers above or below") {
    World world({1, 2, 1});
    world.set({0, 0, 0}, Block::hopper(3));
    world.set({0, 1, 0}, Block::barrel(5));
    CHECK_FALSE(world.validate().empty());
  }
  SECTION("channels must be unique") {
    World world({2, 1, 1});
    world.set({0, 0, 0}, Block::input(0, InputKind::Strong));
    world.set({1, 0, 0}, Block::input(0, InputKind::Container));
    CHECK_FALSE(world.validate().empty());
  }
}

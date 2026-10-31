#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "redstone/update_order.hpp"

using namespace rs;

namespace {

struct HashCase {
  Vec3 origin;
  std::array<Vec3, UpdateOrder::kDustUpdateSetSize> relative_order;
};

// Produced by iterating a real java.util.HashSet filled like
// DefaultRedstoneController does.
const std::vector<HashCase> kJavaHashSetOrders{
    {{0, 0, 0},
     {{{0, 0, 0},
       {0, 0, -1},
       {-1, 0, 0},
       {0, 0, 1},
       {1, 0, 0},
       {0, -1, 0},
       {0, 1, 0}}}},
    {{5, 1, 3},
     {{{0, 1, 0},
       {0, 0, -1},
       {-1, 0, 0},
       {0, 0, 0},
       {0, -1, 0},
       {0, 0, 1},
       {1, 0, 0}}}},
    {{-7, 64, 12},
     {{{0, 1, 0},
       {0, 0, -1},
       {-1, 0, 0},
       {0, 0, 0},
       {0, -1, 0},
       {0, 0, 1},
       {1, 0, 0}}}},
    {{100, -3, -250},
     {{{0, 1, 0},
       {0, 0, -1},
       {-1, 0, 0},
       {0, 0, 0},
       {0, -1, 0},
       {0, 0, 1},
       {1, 0, 0}}}},
    {{123456, 70, -98765},
     {{{0, 0, 0},
       {0, 1, 0},
       {0, 0, -1},
       {-1, 0, 0},
       {0, -1, 0},
       {0, 0, 1},
       {1, 0, 0}}}},
};

} // namespace

TEST_CASE("dust update set iterates like Java's HashSet<BlockPos>") {
  for (const HashCase &c : kJavaHashSetOrders) {
    UpdateOrder order(Placement{c.origin, Symmetry{}});
    std::array<Vec3, UpdateOrder::kDustUpdateSetSize> actual =
        order.dust_update_positions({0, 0, 0});
    for (int i = 0; i < UpdateOrder::kDustUpdateSetSize; ++i)
      CHECK(actual[i] == c.relative_order[i]);
  }
}

TEST_CASE("identity placement keeps Java's direction orders") {
  UpdateOrder order;
  CHECK(order.neighbor_order() == std::array{Dir::West, Dir::East, Dir::Down,
                                             Dir::Up, Dir::North, Dir::South});
  CHECK(order.shape_order() == std::array{Dir::West, Dir::East, Dir::North,
                                          Dir::South, Dir::Down, Dir::Up});
}

TEST_CASE("rotated placement maps the world order back into local directions") {
  // A quarter turn sends local north to world east, so world west is local
  // south.
  UpdateOrder order(Placement{{}, Symmetry(1, false)});
  CHECK(order.neighbor_order()[0] == Dir::South);
  CHECK(order.neighbor_order()[1] == Dir::North);
}

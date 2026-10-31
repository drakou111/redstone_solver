#include <catch2/catch_test_macros.hpp>

#include "redstone/geometry.hpp"

using namespace rs;

TEST_CASE("opposite directions cancel out") {
  for (Dir d : kAllDirs) {
    CHECK(opposite(opposite(d)) == d);
    CHECK(offset(d) + offset(opposite(d)) == Vec3{});
  }
}

TEST_CASE("clockwise rotation cycles the horizontal directions") {
  CHECK(rotate_clockwise(Dir::North) == Dir::East);
  CHECK(rotate_clockwise(Dir::East) == Dir::South);
  CHECK(rotate_counter_clockwise(Dir::North) == Dir::West);
  CHECK_THROWS(rotate_clockwise(Dir::Up));
}

TEST_CASE("every symmetry maps directions and offsets consistently") {
  for (Symmetry s : Symmetry::all()) {
    for (Dir d : kAllDirs) {
      CHECK(s.apply(offset(d)) == offset(s.apply(d)));
      CHECK(s.apply_inverse(s.apply(d)) == d);
    }
    CHECK(s.apply(Dir::Up) == Dir::Up);
    CHECK(s.apply(Dir::Down) == Dir::Down);
  }
  CHECK(Symmetry(1, false).apply(Dir::North) == Dir::East);
  CHECK(Symmetry(0, true).apply(Dir::East) == Dir::West);
}

#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace rs {

// Coordinates follow Minecraft: +x east, +y up, +z south.
struct Vec3 {
  int x = 0;
  int y = 0;
  int z = 0;

  constexpr Vec3 operator+(Vec3 other) const {
    return {x + other.x, y + other.y, z + other.z};
  }
  constexpr Vec3 operator-(Vec3 other) const {
    return {x - other.x, y - other.y, z - other.z};
  }
  constexpr bool operator==(const Vec3 &) const = default;
};

// Ordinals match Java's Direction.values() so ordering tables can be copied
// verbatim.
enum class Dir : std::uint8_t { Down, Up, North, South, West, East };

inline constexpr int kDirCount = 6;
inline constexpr std::array<Dir, kDirCount> kAllDirs{
    Dir::Down, Dir::Up, Dir::North, Dir::South, Dir::West, Dir::East};
// Java's Direction.Plane.HORIZONTAL order.
inline constexpr std::array<Dir, 4> kHorizontalDirs{Dir::North, Dir::East,
                                                    Dir::South, Dir::West};

constexpr int index_of(Dir d) { return static_cast<int>(d); }

constexpr Vec3 offset(Dir d) {
  switch (d) {
  case Dir::Down:
    return {0, -1, 0};
  case Dir::Up:
    return {0, 1, 0};
  case Dir::North:
    return {0, 0, -1};
  case Dir::South:
    return {0, 0, 1};
  case Dir::West:
    return {-1, 0, 0};
  case Dir::East:
    return {1, 0, 0};
  }
  throw std::logic_error("invalid direction");
}

constexpr Dir opposite(Dir d) {
  switch (d) {
  case Dir::Down:
    return Dir::Up;
  case Dir::Up:
    return Dir::Down;
  case Dir::North:
    return Dir::South;
  case Dir::South:
    return Dir::North;
  case Dir::West:
    return Dir::East;
  case Dir::East:
    return Dir::West;
  }
  throw std::logic_error("invalid direction");
}

constexpr bool is_horizontal(Dir d) { return d != Dir::Down && d != Dir::Up; }

constexpr bool same_axis(Dir a, Dir b) { return a == b || a == opposite(b); }

// Clockwise when viewed from above: north -> east -> south -> west.
constexpr Dir rotate_clockwise(Dir d) {
  switch (d) {
  case Dir::North:
    return Dir::East;
  case Dir::East:
    return Dir::South;
  case Dir::South:
    return Dir::West;
  case Dir::West:
    return Dir::North;
  default:
    throw std::invalid_argument(
        "rotate_clockwise needs a horizontal direction");
  }
}

constexpr Dir rotate_counter_clockwise(Dir d) {
  return opposite(rotate_clockwise(d));
}

constexpr std::string_view to_string(Dir d) {
  switch (d) {
  case Dir::Down:
    return "down";
  case Dir::Up:
    return "up";
  case Dir::North:
    return "north";
  case Dir::South:
    return "south";
  case Dir::West:
    return "west";
  case Dir::East:
    return "east";
  }
  return "?";
}

// One of the 8 horizontal symmetries: optional mirror (x -> -x), then clockwise
// quarter turns.
class Symmetry {
public:
  constexpr Symmetry() = default;

  constexpr Symmetry(int quarter_turns, bool mirrored)
      : quarter_turns_(((quarter_turns % 4) + 4) % 4), mirrored_(mirrored) {}

  constexpr Vec3 apply(Vec3 v) const {
    if (mirrored_)
      v.x = -v.x;
    for (int i = 0; i < quarter_turns_; ++i)
      v = {-v.z, v.y, v.x};
    return v;
  }

  constexpr Dir apply(Dir d) const { return from_offset(apply(offset(d))); }

  constexpr Dir apply_inverse(Dir d) const {
    for (Dir candidate : kAllDirs)
      if (apply(candidate) == d)
        return candidate;
    throw std::logic_error("symmetry is not a bijection");
  }

  constexpr int quarter_turns() const { return quarter_turns_; }
  constexpr bool mirrored() const { return mirrored_; }

  static constexpr std::array<Symmetry, 8> all() {
    return {Symmetry(0, false), Symmetry(1, false), Symmetry(2, false),
            Symmetry(3, false), Symmetry(0, true),  Symmetry(1, true),
            Symmetry(2, true),  Symmetry(3, true)};
  }

private:
  static constexpr Dir from_offset(Vec3 v) {
    for (Dir candidate : kAllDirs)
      if (offset(candidate) == v)
        return candidate;
    throw std::logic_error("offset is not a unit direction");
  }

  int quarter_turns_ = 0;
  bool mirrored_ = false;
};

} // namespace rs

#include "redstone/report.hpp"

#include <format>

namespace rs {

namespace {

constexpr int kCellWidth = 4;

char dir_letter(Dir d) {
  switch (d) {
  case Dir::Down:
    return 'd';
  case Dir::Up:
    return 'u';
  case Dir::North:
    return 'n';
  case Dir::South:
    return 's';
  case Dir::West:
    return 'w';
  case Dir::East:
    return 'e';
  }
  return '?';
}

std::string code(const Block &b) {
  switch (b.type) {
  case BlockType::Air:
    return ".";
  case BlockType::Solid:
    return "#";
  case BlockType::Glass:
    return "g";
  case BlockType::Dust:
    return "-";
  case BlockType::Torch:
    return std::format("t{}", dir_letter(b.facing));
  case BlockType::RedstoneBlock:
    return "R";
  case BlockType::Repeater:
    return std::format("r{}{}", dir_letter(b.facing), b.repeater_delay());
  case BlockType::Comparator:
    return std::format("c{}{}", dir_letter(b.facing),
                       b.comparator_mode() == ComparatorMode::Subtract ? 's'
                                                                       : 'c');
  case BlockType::Target:
    return "x";
  case BlockType::NoteBlock:
    return "n";
  case BlockType::Hopper:
    return std::format("h{}", b.stored_signal());
  case BlockType::Barrel:
    return std::format("b{}", b.stored_signal());
  case BlockType::Chest:
    return std::format("k{}", b.stored_signal());
  case BlockType::Observer:
    return std::format("o{}", dir_letter(b.facing));
  case BlockType::Input:
    return std::format("I{}", b.channel + 1);
  case BlockType::Output:
    return std::format("O{}", b.channel + 1);
  }
  return "?";
}

} // namespace

std::string render_layers(const World &world) {
  std::string text;
  Vec3 size = world.size();
  for (int y = size.y - 1; y >= 0; --y) {
    text += std::format("y={} (north at top, east to the right)\n", y);
    for (int z = 0; z < size.z; ++z) {
      for (int x = 0; x < size.x; ++x)
        text += std::format("{:<{}}", code(world.at({x, y, z})), kCellWidth);
      text += '\n';
    }
    text += '\n';
  }
  text += "legend: . air  # solid  g glass  - dust  R redstone block  x target "
          " n note block\n"
          "        t<attached side> torch  r<facing><delay> repeater  "
          "c<facing><c|s> comparator\n"
          "        o<watched side> observer  h/b/k<ss> hopper/barrel/chest  "
          "I<n>/O<n> ports\n"
          "        sides: n s e w u d (repeater/comparator facing = direction "
          "the signal travels)\n";
  return text;
}

std::string list_blocks(const World &world) {
  std::string text;
  for (int i = 0; i < world.volume(); ++i) {
    Vec3 p = world.position_of(i);
    const Block &b = world.at(p);
    if (b.type != BlockType::Air)
      text += std::format("({}, {}, {}) {}\n", p.x, p.y, p.z, to_string(b));
  }
  return text;
}

} // namespace rs

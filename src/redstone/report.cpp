#include "redstone/report.hpp"

#include <format>

#include "redstone/simulator.hpp"

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

std::string block_code(const Block &block) { return code(block); }

namespace {

std::string state_name(Dir d) { return std::string(to_string(d)); }

// Java block id with state for one block; empty for ports without a block.
std::string java_block(const Block &b, const Simulator &sim, Vec3 p) {
  switch (b.type) {
  case BlockType::Solid:
    return "cyan_wool";
  case BlockType::Glass:
    return "cyan_stained_glass";
  case BlockType::RedstoneBlock:
    return "redstone_block";
  case BlockType::Target:
    return "target";
  case BlockType::NoteBlock:
    return "note_block";
  case BlockType::Hopper:
    return "hopper[facing=down]";
  case BlockType::Barrel:
    return "barrel[facing=up]";
  case BlockType::Chest:
    return "chest[facing=north]";
  case BlockType::Observer:
    return std::format("observer[facing={}]", state_name(b.facing));
  case BlockType::Torch:
    if (b.facing == Dir::Down)
      return "redstone_torch";
    return std::format("redstone_wall_torch[facing={}]",
                       state_name(opposite(b.facing)));
  // Java's diode "facing" points at the input, opposite to the signal.
  case BlockType::Repeater:
    return std::format("repeater[facing={},delay={}]",
                       state_name(opposite(b.facing)), b.repeater_delay());
  case BlockType::Comparator:
    return std::format(
        "comparator[facing={},mode={}]", state_name(opposite(b.facing)),
        b.comparator_mode() == ComparatorMode::Subtract ? "subtract"
                                                        : "compare");
  case BlockType::Dust:
  case BlockType::Output:
    if (!is_dust(b))
      return "";
    return std::format("redstone_wire[north={},south={},east={},west={}]",
                       sim.dust_points(p, Dir::North) ? "side" : "none",
                       sim.dust_points(p, Dir::South) ? "side" : "none",
                       sim.dust_points(p, Dir::East) ? "side" : "none",
                       sim.dust_points(p, Dir::West) ? "side" : "none");
  case BlockType::Input:
    return is_container_input(b) ? "barrel[facing=up]" : "";
  case BlockType::Air:
    return "";
  }
  return "";
}

bool needs_support(const Block &b) {
  return is_dust(b) || b.type == BlockType::Torch ||
         b.type == BlockType::Repeater || b.type == BlockType::Comparator;
}

} // namespace

std::string setblock_commands(const World &world) {
  Simulator sim(world);
  std::string text =
      "# Run from the layout's origin corner (lowest x, y, z).\n"
      "# Afterwards toggle the inputs once so every component updates.\n";
  for (int i = 0; i < world.volume(); ++i) {
    Vec3 p = world.position_of(i);
    const Block &b = world.at(p);
    if (b.type == BlockType::Input)
      text += std::format(
          "# IN{} at ~{} ~{} ~{}: {}\n", b.channel + 1, p.x, p.y, p.z,
          is_strong_input(b)
              ? std::format("strong source facing {}", to_string(b.facing))
              : std::string("barrel whose fill sets the "
                            "signal"));
    if (b.type == BlockType::Output)
      text += std::format("# OUT{} at ~{} ~{} ~{}: {}\n", b.channel + 1, p.x,
                          p.y, p.z,
                          is_dust(b) ? std::string("read this dust")
                                     : std::format("read like a repeater "
                                                   "facing {}",
                                                   to_string(b.facing)));
    if (b.type == BlockType::Hopper || b.type == BlockType::Barrel ||
        b.type == BlockType::Chest)
      text += std::format("# fill the {} at ~{} ~{} ~{} to comparator "
                          "signal {}\n",
                          to_string(b), p.x, p.y, p.z, b.stored_signal());
  }
  for (bool supported_pass : {false, true}) {
    for (int i = 0; i < world.volume(); ++i) {
      Vec3 p = world.position_of(i);
      const Block &b = world.at(p);
      if (needs_support(b) != supported_pass)
        continue;
      std::string id = java_block(b, sim, p);
      if (!id.empty())
        text += std::format("setblock ~{} ~{} ~{} {}\n", p.x, p.y, p.z, id);
    }
  }
  return text;
}

} // namespace rs

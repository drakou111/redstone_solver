#include "redstone/block.hpp"

#include <format>
#include <stdexcept>

namespace rs {

namespace {

constexpr int kMaxChannel = 255;

std::uint8_t checked_signal(int signal) {
  if (signal < 0 || signal > kMaxSignal)
    throw std::invalid_argument(
        std::format("signal strength {} is outside 0..{}", signal, kMaxSignal));
  return static_cast<std::uint8_t>(signal);
}

std::uint8_t checked_channel(int channel) {
  if (channel < 0 || channel > kMaxChannel)
    throw std::invalid_argument(
        std::format("channel {} is outside 0..{}", channel, kMaxChannel));
  return static_cast<std::uint8_t>(channel);
}

Dir checked_horizontal(Dir d, std::string_view what) {
  if (!is_horizontal(d))
    throw std::invalid_argument(std::format(
        "{} must face a horizontal direction, got {}", what, to_string(d)));
  return d;
}

} // namespace

Block Block::hopper(int signal) {
  return {BlockType::Hopper, Dir::Down, checked_signal(signal)};
}

Block Block::barrel(int signal) {
  return {BlockType::Barrel, Dir::Up, checked_signal(signal)};
}

Block Block::chest(int signal) {
  return {BlockType::Chest, Dir::North, checked_signal(signal)};
}

Block Block::torch(Dir attached_to) {
  if (attached_to == Dir::Up)
    throw std::invalid_argument("a torch cannot hang from a ceiling");
  return {BlockType::Torch, attached_to};
}

Block Block::repeater(Dir output, int delay) {
  if (delay < kMinRepeaterDelay || delay > kMaxRepeaterDelay)
    throw std::invalid_argument(
        std::format("repeater delay {} is outside {}..{}", delay,
                    kMinRepeaterDelay, kMaxRepeaterDelay));
  return {BlockType::Repeater, checked_horizontal(output, "repeater"),
          static_cast<std::uint8_t>(delay)};
}

Block Block::comparator(Dir output, ComparatorMode mode) {
  return {BlockType::Comparator, checked_horizontal(output, "comparator"),
          static_cast<std::uint8_t>(mode)};
}

Block Block::observer(Dir watched) { return {BlockType::Observer, watched}; }

Block Block::input(int channel, InputKind kind, Dir output) {
  return {BlockType::Input, output, static_cast<std::uint8_t>(kind),
          checked_channel(channel)};
}

Block Block::output(int channel, OutputKind kind, Dir signal_direction) {
  return {BlockType::Output, signal_direction, static_cast<std::uint8_t>(kind),
          checked_channel(channel)};
}

std::string to_string(const Block &block) {
  switch (block.type) {
  case BlockType::Air:
    return "air";
  case BlockType::Solid:
    return "solid";
  case BlockType::Glass:
    return "glass";
  case BlockType::Dust:
    return "dust";
  case BlockType::Torch:
    return std::format("torch(on {})", to_string(block.facing));
  case BlockType::RedstoneBlock:
    return "redstone_block";
  case BlockType::Repeater:
    return std::format("repeater({}, delay {})", to_string(block.facing),
                       block.repeater_delay());
  case BlockType::Comparator:
    return std::format("comparator({}, {})", to_string(block.facing),
                       block.comparator_mode() == ComparatorMode::Compare
                           ? "compare"
                           : "subtract");
  case BlockType::Target:
    return "target";
  case BlockType::NoteBlock:
    return "note_block";
  case BlockType::Hopper:
    return std::format("hopper(ss {})", block.stored_signal());
  case BlockType::Barrel:
    return std::format("barrel(ss {})", block.stored_signal());
  case BlockType::Chest:
    return std::format("chest(ss {})", block.stored_signal());
  case BlockType::Observer:
    return std::format("observer(watching {})", to_string(block.facing));
  case BlockType::Input:
    return std::format("input{}({}, {})", block.channel,
                       block.input_kind() == InputKind::Strong ? "strong"
                                                               : "container",
                       to_string(block.facing));
  case BlockType::Output:
    return std::format("output{}({}, {})", block.channel,
                       block.output_kind() == OutputKind::Dust ? "dust"
                                                               : "component",
                       to_string(block.facing));
  }
  return "?";
}

} // namespace rs

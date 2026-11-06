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

Block Block::lever(Dir attached_to, bool on) {
  return {BlockType::Lever, attached_to, static_cast<std::uint8_t>(on)};
}

Block Block::button(Dir attached_to) {
  return {BlockType::Button, attached_to};
}

Block Block::input(int channel, InputKind kind, Dir output) {
  Block block{BlockType::Input, output, static_cast<std::uint8_t>(kind),
              checked_channel(channel)};
  if (is_diode_port(block))
    checked_horizontal(output, input_kind_name(kind));
  return block;
}

Block Block::output(int channel, OutputKind kind, Dir signal_direction) {
  Block block{BlockType::Output, signal_direction,
              static_cast<std::uint8_t>(kind), checked_channel(channel)};
  if (is_diode_port(block))
    checked_horizontal(signal_direction, output_kind_name(kind));
  return block;
}

std::string_view input_kind_name(InputKind kind) {
  switch (kind) {
  case InputKind::Comparator:
    return "comparator";
  case InputKind::Container:
    return "container";
  case InputKind::Repeater:
    return "repeater";
  case InputKind::Dust:
    return "dust";
  }
  return "?";
}

std::string_view output_kind_name(OutputKind kind) {
  switch (kind) {
  case OutputKind::Dust:
    return "dust";
  case OutputKind::Repeater:
    return "repeater";
  case OutputKind::Comparator:
    return "comparator";
  }
  return "?";
}

std::optional<InputKind> parse_input_kind(std::string_view name) {
  if (name == "strong") // older spec files
    return InputKind::Comparator;
  for (InputKind kind : kInputKinds)
    if (input_kind_name(kind) == name)
      return kind;
  return std::nullopt;
}

std::optional<OutputKind> parse_output_kind(std::string_view name) {
  if (name == "component") // older spec files
    return OutputKind::Repeater;
  for (OutputKind kind : kOutputKinds)
    if (output_kind_name(kind) == name)
      return kind;
  return std::nullopt;
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
                       input_kind_name(block.input_kind()),
                       to_string(block.facing));
  case BlockType::Output:
    return std::format("output{}({}, {})", block.channel,
                       output_kind_name(block.output_kind()),
                       to_string(block.facing));
  case BlockType::Lever:
    return std::format("lever(on {}, {})", to_string(block.facing),
                       block.value != 0 ? "on" : "off");
  case BlockType::Button:
    return std::format("button(on {})", to_string(block.facing));
  case BlockType::Lamp:
    return "lamp";
  }
  return "?";
}

} // namespace rs

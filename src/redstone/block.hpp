#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "redstone/geometry.hpp"

namespace rs {

inline constexpr int kMaxSignal = 15;
inline constexpr int kMinRepeaterDelay = 1;
inline constexpr int kMaxRepeaterDelay = 4;

enum class BlockType : std::uint8_t {
  Air,
  Solid, // cyan wool: opaque, conducts power
  Glass, // cyan glass: transparent, does not conduct
  Dust,
  Torch,
  RedstoneBlock,
  Repeater,
  Comparator,
  Target,
  NoteBlock,
  Hopper, // fixed contents, always facing down
  Barrel, // fixed contents
  Chest,  // fixed contents
  Observer,
  Input,  // user-placed signal source driven by the truth table
  Output, // user-placed probe read by the truth table
  // Test blocks for building around a circuit; the solver never places them.
  Lever,  // value 1: on
  Button, // stone button: on for 20 game ticks after a press
  Lamp,   // redstone lamp: lit while powered
};

enum class ComparatorMode : std::uint8_t { Compare, Subtract };

// What drives an input port. Each behaves like the real block:
//   Comparator: outputs the input strength along `facing`.
//   Container: a barrel whose comparator signal is the input value.
//   Repeater: outputs 15 along `facing` while the input is above 0.
//   Dust: dust held at the input strength.
enum class InputKind : std::uint8_t { Comparator, Container, Repeater, Dust };

// What reads an output port:
//   Dust: the dust's power.
//   Repeater: the signal a repeater facing `facing` receives from behind.
//   Comparator: what a comparator facing `facing` reads from behind,
//   including containers (also through a block).
enum class OutputKind : std::uint8_t { Dust, Repeater, Comparator };

// Static description of one cell. Meaning of `facing` per type:
//   Torch, Lever, Button: direction from it to the block it is attached to
//   (Down = standing on the floor; levers and buttons may also hang, Up).
//   Repeater, Comparator and repeater/comparator ports: direction the signal
//   travels. Observer: direction of the watched face; it outputs on the
//   opposite side.
struct Block {
  BlockType type = BlockType::Air;
  Dir facing = Dir::Down;
  std::uint8_t value =
      0; // repeater delay, comparator mode, container signal, IO kind
  std::uint8_t channel = 0; // input/output index

  constexpr bool operator==(const Block &) const = default;

  static constexpr Block air() { return {}; }
  static constexpr Block solid() { return {BlockType::Solid}; }
  static constexpr Block glass() { return {BlockType::Glass}; }
  static constexpr Block dust() { return {BlockType::Dust}; }
  static constexpr Block redstone_block() { return {BlockType::RedstoneBlock}; }
  static constexpr Block target() { return {BlockType::Target}; }
  static constexpr Block note_block() { return {BlockType::NoteBlock}; }
  static Block hopper(int signal = 0);
  static Block barrel(int signal);
  static Block chest(int signal);
  static Block torch(Dir attached_to);
  static Block repeater(Dir output, int delay = kMinRepeaterDelay);
  static Block comparator(Dir output,
                          ComparatorMode mode = ComparatorMode::Compare);
  static Block observer(Dir watched);
  static Block lever(Dir attached_to, bool on = false);
  static Block button(Dir attached_to);
  static constexpr Block lamp() { return {BlockType::Lamp}; }
  static Block input(int channel, InputKind kind, Dir output = Dir::North);
  static Block output(int channel, OutputKind kind,
                      Dir signal_direction = Dir::North);

  int repeater_delay() const { return value; }
  ComparatorMode comparator_mode() const {
    return static_cast<ComparatorMode>(value);
  }
  int stored_signal() const { return value; }
  InputKind input_kind() const { return static_cast<InputKind>(value); }
  OutputKind output_kind() const { return static_cast<OutputKind>(value); }
};

std::string to_string(const Block &block);

inline constexpr std::array<InputKind, 4> kInputKinds{
    InputKind::Comparator, InputKind::Repeater, InputKind::Dust,
    InputKind::Container};
inline constexpr std::array<OutputKind, 3> kOutputKinds{
    OutputKind::Dust, OutputKind::Repeater, OutputKind::Comparator};

// Spec-file names: comparator, repeater, dust, container / dust, repeater,
// comparator. The old names "strong" and "component" are still accepted.
std::string_view input_kind_name(InputKind kind);
std::string_view output_kind_name(OutputKind kind);
std::optional<InputKind> parse_input_kind(std::string_view name);
std::optional<OutputKind> parse_output_kind(std::string_view name);

constexpr bool is_input(const Block &b, InputKind kind) {
  return b.type == BlockType::Input && b.value == static_cast<int>(kind);
}

constexpr bool is_output(const Block &b, OutputKind kind) {
  return b.type == BlockType::Output && b.value == static_cast<int>(kind);
}

constexpr bool is_dust(const Block &b) {
  return b.type == BlockType::Dust || is_output(b, OutputKind::Dust) ||
         is_input(b, InputKind::Dust);
}

constexpr bool is_container_input(const Block &b) {
  return is_input(b, InputKind::Container);
}

// Comparator or repeater input: a source that outputs along its facing.
constexpr bool is_diode_input(const Block &b) {
  return is_input(b, InputKind::Comparator) || is_input(b, InputKind::Repeater);
}

// Ports standing in for a repeater or comparator: they need a block below
// and a horizontal facing, like the real thing.
constexpr bool is_diode_port(const Block &b) {
  return is_diode_input(b) || is_output(b, OutputKind::Repeater) ||
         is_output(b, OutputKind::Comparator);
}

// Dust connects to these only along their axis.
constexpr bool connects_along_axis(const Block &b) {
  return b.type == BlockType::Repeater || is_input(b, InputKind::Repeater) ||
         is_output(b, OutputKind::Repeater);
}

// Java's isRedstoneConductor: the block can be powered and relay that power.
constexpr bool is_conductor(const Block &b) {
  switch (b.type) {
  case BlockType::Solid:
  case BlockType::Target:
  case BlockType::NoteBlock:
  case BlockType::Barrel:
  case BlockType::Lamp:
    return true;
  case BlockType::Input:
    return is_container_input(b);
  default:
    return false;
  }
}

constexpr bool is_container(const Block &b) {
  return b.type == BlockType::Hopper || b.type == BlockType::Barrel ||
         b.type == BlockType::Chest || is_container_input(b);
}

// Repeaters, comparators and the inputs standing in for them.
constexpr bool is_diode(const Block &b) {
  return b.type == BlockType::Repeater || b.type == BlockType::Comparator ||
         is_diode_input(b);
}

constexpr bool is_signal_source(const Block &b) {
  switch (b.type) {
  case BlockType::Torch:
  case BlockType::RedstoneBlock:
  case BlockType::Repeater:
  case BlockType::Comparator:
  case BlockType::Target:
  case BlockType::Observer:
  case BlockType::Lever:
  case BlockType::Button:
    return true;
  case BlockType::Input:
    return is_diode_input(b) || is_dust(b);
  case BlockType::Output:
    return is_output(b, OutputKind::Comparator) || is_dust(b);
  default:
    return is_dust(b);
  }
}

// Dust, repeaters, comparators and standing torches can sit on top of these.
constexpr bool supports_on_top(const Block &b) {
  switch (b.type) {
  case BlockType::Solid:
  case BlockType::Glass:
  case BlockType::RedstoneBlock:
  case BlockType::Target:
  case BlockType::NoteBlock:
  case BlockType::Barrel:
  case BlockType::Observer:
  case BlockType::Hopper:
  case BlockType::Lamp:
    return true;
  case BlockType::Input:
    return is_container_input(b);
  default:
    return false;
  }
}

// Wall torches can hang on the side of these.
constexpr bool supports_on_side(const Block &b) {
  return supports_on_top(b) && b.type != BlockType::Hopper;
}

} // namespace rs

#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
};

enum class ComparatorMode : std::uint8_t { Compare, Subtract };

// Strong: behaves like a comparator output pointing along `facing`.
// Container: behaves like a barrel whose comparator signal is the input value.
enum class InputKind : std::uint8_t { Strong, Container };

// Dust: a dust cell whose power is the output value.
// Component: reads the signal a repeater facing the same way would receive from
// behind.
enum class OutputKind : std::uint8_t { Dust, Component };

// Static description of one cell. Meaning of `facing` per type:
//   Torch: direction from the torch to the block it is attached to (Down =
//   standing torch). Repeater, Comparator, Input(Strong), Output(Component):
//   direction the signal travels. Observer: direction of the watched face; it
//   outputs on the opposite side.
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

constexpr bool is_dust(const Block &b) {
  return b.type == BlockType::Dust ||
         (b.type == BlockType::Output &&
          b.value == static_cast<int>(OutputKind::Dust));
}

constexpr bool is_strong_input(const Block &b) {
  return b.type == BlockType::Input &&
         b.value == static_cast<int>(InputKind::Strong);
}

constexpr bool is_container_input(const Block &b) {
  return b.type == BlockType::Input &&
         b.value == static_cast<int>(InputKind::Container);
}

constexpr bool is_component_output(const Block &b) {
  return b.type == BlockType::Output &&
         b.value == static_cast<int>(OutputKind::Component);
}

// Java's isRedstoneConductor: the block can be powered and relay that power.
constexpr bool is_conductor(const Block &b) {
  switch (b.type) {
  case BlockType::Solid:
  case BlockType::Target:
  case BlockType::NoteBlock:
  case BlockType::Barrel:
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

// Repeaters, comparators and comparator-like inputs.
constexpr bool is_diode(const Block &b) {
  return b.type == BlockType::Repeater || b.type == BlockType::Comparator ||
         is_strong_input(b);
}

constexpr bool is_signal_source(const Block &b) {
  switch (b.type) {
  case BlockType::Torch:
  case BlockType::RedstoneBlock:
  case BlockType::Repeater:
  case BlockType::Comparator:
  case BlockType::Target:
  case BlockType::Observer:
    return true;
  case BlockType::Input:
    return is_strong_input(b);
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

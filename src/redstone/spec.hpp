#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redstone/block.hpp"
#include "redstone/geometry.hpp"
#include "redstone/world.hpp"

namespace rs {

inline constexpr int kGameTicksPerRedstoneTick = 2;

struct SignalRange {
  int min = 0;
  int max = kMaxSignal;

  bool contains(int value) const {
    return value >= this->min && value <= this->max;
  }
  std::optional<SignalRange> intersect(SignalRange other) const;
  bool operator==(const SignalRange &) const = default;
};

std::string to_string(SignalRange range);

using InputVector = std::vector<int>;

struct TruthRow {
  std::vector<SignalRange> inputs;  // by input channel
  std::vector<SignalRange> outputs; // by output channel
};

// A port either sits at `position` facing `facing`, or (with a non-empty
// `region`) the solver picks one of the region's cells and a facing.
struct InputPort {
  InputKind kind = InputKind::Comparator;
  Vec3 position{};
  Dir facing = Dir::North;
  std::vector<Vec3> region;

  bool is_free() const { return !this->region.empty(); }
};

struct OutputPort {
  OutputKind kind = OutputKind::Dust;
  Vec3 position{};
  Dir facing = Dir::North;
  std::vector<Vec3> region;

  bool is_free() const { return !this->region.empty(); }
};

// All times are game ticks after the inputs change.
struct TimingSpec {
  std::optional<int> earliest;
  std::optional<int> latest;  // outputs must be correct from here on
  bool hold_previous = false; // before `earliest`, outputs keep their old value
  int settle_ticks = 4;       // extra ticks after `latest` to come to rest
  int horizon_ticks = 20;     // time to come to rest without a deadline

  // Ticks after an input change by which the circuit must be at rest.
  int horizon() const;
};

struct Spec {
  Vec3 size{};
  std::vector<InputPort> inputs;   // index = channel
  std::vector<OutputPort> outputs; // index = channel
  std::vector<TruthRow> rows;
  TimingSpec timing;
  std::vector<BlockType>
      palette; // blocks the solver may place (air always allowed)
  // Loops through torches/repeaters/comparators/observers allow memory and
  // clocks; truth tables do not need them and forbidding them keeps every
  // input's settled state unique.
  bool allow_feedback = false;
  // The bottom layer only holds supports (solid or air). Dust and
  // diodes cannot sit there anyway, and leaving out the rare remaining uses
  // makes solving much faster.
  bool support_layer = true;
  // Axes (x, y, z) along which copies are placed side by side. Copies must
  // not influence each other whatever their inputs are.
  std::array<bool, 3> tiled{};
  // Blocks the solver must keep exactly as given.
  std::vector<std::pair<Vec3, Block>> fixed;
  // Cells the solver must leave empty.
  std::vector<Vec3> forbidden;
  // A sketch: blocks the solver tries first but may change. Cells without
  // a hint are tried empty first.
  std::vector<std::pair<Vec3, Block>> hints;

  bool is_tiled() const {
    return this->tiled[0] || this->tiled[1] || this->tiled[2];
  }

  // Output ranges required for an input vector, or nullopt if no row covers it.
  std::optional<std::vector<SignalRange>>
  expected_outputs(const InputVector &vector) const;
  // Each row's lowest and highest inputs, used to seed the solver model.
  std::vector<InputVector> representative_vectors() const;
  // Min, middle and max of every range, combined per row; used for
  // verification.
  std::vector<InputVector> verification_vectors() const;
  // Empty world with the fixed ports and kept blocks placed (ports the
  // solver places are left out).
  World base_world() const;
  bool palette_contains(BlockType type) const;

  // Throws std::invalid_argument describing the first problem.
  void validate() const;
};

std::vector<BlockType> default_palette();

// "12", "12gt" or "6rt" -> game ticks.
int parse_ticks(std::string_view token);
std::optional<BlockType> parse_palette_block(std::string_view name);
std::string_view palette_name(BlockType type);

// "IN1(ss1..15),IN2(ss0) -> OUT1(ss0..15)". Unlisted channels accept anything.
TruthRow parse_truth_row(std::string_view text, int input_count,
                         int output_count);

// Line-based spec file; see README for the format.
Spec parse_spec(std::string_view text);
// Inverse of parse_spec (times are written in game ticks).
std::string format_spec(const Spec &spec);

// "", "*" (anything), "5", "1..15", "ss5" or "ss1..15".
SignalRange parse_signal_range(std::string_view text);
// Short form used by the editor: "*", "5" or "1..15".
std::string short_range_text(SignalRange range);

// "solid", "repeater east 2", "comparator north subtract", "barrel 7",
// "torch down"... -> the block (as written after "keep x y z").
Block parse_block_words(const std::vector<std::string_view> &words);
std::string block_words(const Block &block);

} // namespace rs

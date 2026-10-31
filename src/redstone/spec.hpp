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

struct InputPort {
  InputKind kind = InputKind::Strong;
  Vec3 position{};
  Dir facing = Dir::North;
};

struct OutputPort {
  OutputKind kind = OutputKind::Dust;
  Vec3 position{};
  Dir facing = Dir::North;
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
  // Axes (x, y, z) along which copies are placed side by side. Copies must
  // not influence each other whatever their inputs are.
  std::array<bool, 3> tiled{};

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
  // Empty world with only the input and output blocks placed.
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

} // namespace rs

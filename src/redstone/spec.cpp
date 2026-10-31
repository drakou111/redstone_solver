#include "redstone/spec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

namespace rs {

namespace {

constexpr int kMinHorizon = 1;

struct PaletteName {
  BlockType type;
  std::string_view name;
};

constexpr std::array<PaletteName, 13> kPaletteNames{{
    {BlockType::Solid, "solid"},
    {BlockType::Glass, "glass"},
    {BlockType::Dust, "dust"},
    {BlockType::Torch, "torch"},
    {BlockType::RedstoneBlock, "redstone_block"},
    {BlockType::Repeater, "repeater"},
    {BlockType::Comparator, "comparator"},
    {BlockType::Target, "target"},
    {BlockType::NoteBlock, "note_block"},
    {BlockType::Hopper, "hopper"},
    {BlockType::Barrel, "barrel"},
    {BlockType::Chest, "chest"},
    {BlockType::Observer, "observer"},
}};

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> split(std::string_view s, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    std::size_t end = s.find(separator, start);
    parts.push_back(trim(s.substr(start, end - start)));
    if (end == std::string_view::npos)
      return parts;
    start = end + 1;
  }
}

std::vector<std::string_view> words(std::string_view s) {
  std::vector<std::string_view> result;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    std::size_t start = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    if (i > start)
      result.push_back(s.substr(start, i - start));
  }
  return result;
}

int parse_int(std::string_view token, std::string_view what) {
  int value = 0;
  auto [end, error] =
      std::from_chars(token.data(), token.data() + token.size(), value);
  if (error != std::errc() || end != token.data() + token.size())
    throw std::invalid_argument(
        std::format("expected an integer for {}, got '{}'", what, token));
  return value;
}

Dir parse_dir(std::string_view token) {
  for (Dir d : kAllDirs)
    if (to_string(d) == token)
      return d;
  throw std::invalid_argument(std::format("unknown direction '{}'", token));
}

// "IN3" -> 2 for prefix "IN".
int parse_port_name(std::string_view token, std::string_view prefix) {
  if (!token.starts_with(prefix))
    throw std::invalid_argument(
        std::format("expected {}<number>, got '{}'", prefix, token));
  int number = parse_int(token.substr(prefix.size()), "port number");
  if (number < 1)
    throw std::invalid_argument(
        std::format("port numbers start at 1, got '{}'", token));
  return number - 1;
}

// "ss5" or "ss1..15"
SignalRange parse_range(std::string_view token) {
  if (!token.starts_with("ss"))
    throw std::invalid_argument(
        std::format("expected ss<min>[..<max>], got '{}'", token));
  token.remove_prefix(2);
  std::size_t dots = token.find("..");
  SignalRange range;
  if (dots == std::string_view::npos) {
    range.min = range.max = parse_int(token, "signal strength");
  } else {
    range.min = parse_int(token.substr(0, dots), "signal strength");
    range.max = parse_int(token.substr(dots + 2), "signal strength");
  }
  if (range.min < 0 || range.max > kMaxSignal || range.min > range.max)
    throw std::invalid_argument(
        std::format("invalid signal range '{}'", token));
  return range;
}

// "IN1(ss1..15)" -> (channel, range)
std::pair<int, SignalRange> parse_term(std::string_view term,
                                       std::string_view prefix) {
  std::size_t open = term.find('(');
  if (open == std::string_view::npos || !term.ends_with(")"))
    throw std::invalid_argument(
        std::format("expected {}<n>(ss..), got '{}'", prefix, term));
  int channel = parse_port_name(trim(term.substr(0, open)), prefix);
  SignalRange range =
      parse_range(trim(term.substr(open + 1, term.size() - open - 2)));
  return {channel, range};
}

void parse_side(std::string_view text, std::string_view prefix,
                std::vector<SignalRange> &ranges) {
  if (trim(text).empty())
    return;
  std::vector<bool> seen(ranges.size(), false);
  for (std::string_view term : split(text, ',')) {
    auto [channel, range] = parse_term(term, prefix);
    if (channel >= static_cast<int>(ranges.size()))
      throw std::invalid_argument(
          std::format("'{}' refers to a port that does not exist", term));
    if (seen[channel])
      throw std::invalid_argument(std::format("'{}' is listed twice", term));
    seen[channel] = true;
    ranges[channel] = range;
  }
}

void append_unique(std::vector<InputVector> &vectors,
                   std::set<InputVector> &seen, const InputVector &vector) {
  if (seen.insert(vector).second)
    vectors.push_back(vector);
}

} // namespace

std::optional<SignalRange> SignalRange::intersect(SignalRange other) const {
  SignalRange result{std::max(this->min, other.min),
                     std::min(this->max, other.max)};
  if (result.min > result.max)
    return std::nullopt;
  return result;
}

std::string to_string(SignalRange range) {
  if (range.min == range.max)
    return std::format("ss{}", range.min);
  return std::format("ss{}..{}", range.min, range.max);
}

int TimingSpec::horizon() const {
  int horizon =
      this->latest ? *this->latest + this->settle_ticks : this->horizon_ticks;
  return std::max(kMinHorizon, horizon);
}

std::optional<std::vector<SignalRange>>
Spec::expected_outputs(const InputVector &vector) const {
  std::optional<std::vector<SignalRange>> result;
  for (const TruthRow &row : this->rows) {
    bool matches = true;
    for (std::size_t i = 0; i < vector.size() && matches; ++i)
      matches = row.inputs[i].contains(vector[i]);
    if (!matches)
      continue;
    if (!result) {
      result = row.outputs;
      continue;
    }
    for (std::size_t o = 0; o < result->size(); ++o) {
      std::optional<SignalRange> both = (*result)[o].intersect(row.outputs[o]);
      if (!both) {
        std::string vector_text;
        for (int value : vector)
          vector_text += std::format("{} ", value);
        throw std::invalid_argument(std::format(
            "rows disagree on OUT{} for inputs [ {}]", o + 1, vector_text));
      }
      (*result)[o] = *both;
    }
  }
  return result;
}

std::vector<InputVector> Spec::representative_vectors() const {
  std::vector<InputVector> vectors;
  std::set<InputVector> seen;
  for (const TruthRow &row : this->rows) {
    InputVector lowest;
    InputVector highest;
    for (SignalRange range : row.inputs) {
      lowest.push_back(range.min);
      highest.push_back(range.max);
    }
    append_unique(vectors, seen, lowest);
    append_unique(vectors, seen, highest);
  }
  return vectors;
}

std::vector<InputVector> Spec::verification_vectors() const {
  std::vector<InputVector> vectors;
  std::set<InputVector> seen;
  for (const TruthRow &row : this->rows) {
    std::vector<std::vector<int>> choices;
    for (SignalRange range : row.inputs) {
      std::vector<int> values{range.min, (range.min + range.max) / 2,
                              range.max};
      values.erase(std::unique(values.begin(), values.end()), values.end());
      choices.push_back(values);
    }
    std::vector<std::size_t> pick(choices.size(), 0);
    while (true) {
      InputVector vector;
      for (std::size_t i = 0; i < choices.size(); ++i)
        vector.push_back(choices[i][pick[i]]);
      append_unique(vectors, seen, vector);

      std::size_t i = 0;
      while (i < pick.size() && ++pick[i] == choices[i].size())
        pick[i++] = 0;
      if (i == pick.size())
        break;
    }
  }
  return vectors;
}

World Spec::base_world() const {
  World world(this->size);
  for (std::size_t i = 0; i < this->inputs.size(); ++i) {
    const InputPort &port = this->inputs[i];
    world.set(port.position,
              Block::input(static_cast<int>(i), port.kind, port.facing));
  }
  for (std::size_t i = 0; i < this->outputs.size(); ++i) {
    const OutputPort &port = this->outputs[i];
    world.set(port.position,
              Block::output(static_cast<int>(i), port.kind, port.facing));
  }
  return world;
}

bool Spec::palette_contains(BlockType type) const {
  return std::find(this->palette.begin(), this->palette.end(), type) !=
         this->palette.end();
}

void Spec::validate() const {
  if (this->size.x <= 0 || this->size.y <= 0 || this->size.z <= 0)
    throw std::invalid_argument("size must be positive in every dimension");
  if (this->inputs.empty())
    throw std::invalid_argument("at least one input is required");
  if (this->outputs.empty())
    throw std::invalid_argument("at least one output is required");
  if (this->rows.empty())
    throw std::invalid_argument("at least one truth table row is required");

  std::set<std::tuple<int, int, int>> occupied;
  auto check_position = [&](Vec3 p, std::string_view name) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= this->size.x ||
        p.y >= this->size.y || p.z >= this->size.z)
      throw std::invalid_argument(
          std::format("{} is outside the volume", name));
    if (!occupied.insert({p.x, p.y, p.z}).second)
      throw std::invalid_argument(
          std::format("{} shares its position with another port", name));
  };
  for (std::size_t i = 0; i < this->inputs.size(); ++i)
    check_position(this->inputs[i].position, std::format("IN{}", i + 1));
  for (std::size_t i = 0; i < this->outputs.size(); ++i)
    check_position(this->outputs[i].position, std::format("OUT{}", i + 1));

  for (const TruthRow &row : this->rows) {
    if (row.inputs.size() != this->inputs.size() ||
        row.outputs.size() != this->outputs.size())
      throw std::invalid_argument(
          "a truth table row does not match the port count");
  }
  for (BlockType type : this->palette) {
    if (type == BlockType::Air || type == BlockType::Input ||
        type == BlockType::Output)
      throw std::invalid_argument("the palette may only list placeable blocks");
  }

  const TimingSpec &t = this->timing;
  if ((t.earliest && *t.earliest < 0) || (t.latest && *t.latest < 0))
    throw std::invalid_argument("timing must not be negative");
  if (t.earliest && t.latest && *t.earliest > *t.latest)
    throw std::invalid_argument("timing: earliest is after latest");
  if (t.hold_previous && !t.earliest)
    throw std::invalid_argument("hold_previous needs an earliest time");
  if (t.settle_ticks < 0 || t.horizon_ticks < kMinHorizon)
    throw std::invalid_argument("settle must be >= 0 and horizon >= 1");

  // Surfaces contradicting rows early.
  for (const InputVector &vector : this->verification_vectors())
    this->expected_outputs(vector);
}

std::vector<BlockType> default_palette() {
  std::vector<BlockType> palette;
  for (const PaletteName &entry : kPaletteNames)
    palette.push_back(entry.type);
  return palette;
}

int parse_ticks(std::string_view token) {
  int scale = 1;
  if (token.ends_with("rt")) {
    scale = kGameTicksPerRedstoneTick;
    token.remove_suffix(2);
  } else if (token.ends_with("gt")) {
    token.remove_suffix(2);
  }
  int value = parse_int(token, "ticks");
  if (value < 0)
    throw std::invalid_argument("ticks must not be negative");
  return value * scale;
}

std::optional<BlockType> parse_palette_block(std::string_view name) {
  for (const PaletteName &entry : kPaletteNames)
    if (entry.name == name)
      return entry.type;
  return std::nullopt;
}

std::string_view palette_name(BlockType type) {
  for (const PaletteName &entry : kPaletteNames)
    if (entry.type == type)
      return entry.name;
  return "air";
}

TruthRow parse_truth_row(std::string_view text, int input_count,
                         int output_count) {
  std::size_t arrow = text.find("->");
  if (arrow == std::string_view::npos)
    throw std::invalid_argument(
        std::format("truth table row needs '->': '{}'", text));
  TruthRow row;
  row.inputs.assign(input_count, SignalRange{});
  row.outputs.assign(output_count, SignalRange{});
  parse_side(text.substr(0, arrow), "IN", row.inputs);
  parse_side(text.substr(arrow + 2), "OUT", row.outputs);
  return row;
}

Spec parse_spec(std::string_view text) {
  Spec spec;
  spec.palette = default_palette();
  std::vector<std::pair<int, std::string_view>> row_lines;
  std::vector<std::optional<InputPort>> inputs;
  std::vector<std::optional<OutputPort>> outputs;

  int line_number = 0;
  for (std::string_view raw_line : split(text, '\n')) {
    ++line_number;
    std::string_view line = trim(raw_line.substr(0, raw_line.find('#')));
    if (line.empty())
      continue;
    std::vector<std::string_view> w = words(line);
    std::string_view key = w[0];
    auto need = [&](std::size_t lo, std::size_t hi) {
      if (w.size() < lo || w.size() > hi)
        throw std::invalid_argument(std::format(
            "line {}: wrong number of values for '{}'", line_number, key));
    };
    auto coordinate = [&](std::size_t i) {
      return parse_int(w[i], "coordinate");
    };

    try {
      if (key == "size") {
        need(4, 4);
        spec.size = {coordinate(1), coordinate(2), coordinate(3)};
      } else if (key == "input") {
        need(6, 7);
        int channel = parse_port_name(w[1], "IN");
        InputPort port;
        if (w[2] == "strong")
          port.kind = InputKind::Strong;
        else if (w[2] == "container")
          port.kind = InputKind::Container;
        else
          throw std::invalid_argument(
              std::format("unknown input kind '{}'", w[2]));
        port.position = {coordinate(3), coordinate(4), coordinate(5)};
        if (port.kind == InputKind::Strong && w.size() != 7)
          throw std::invalid_argument("a strong input needs a facing");
        if (w.size() == 7)
          port.facing = parse_dir(w[6]);
        if (static_cast<int>(inputs.size()) <= channel)
          inputs.resize(channel + 1);
        if (inputs[channel])
          throw std::invalid_argument(std::format("{} defined twice", w[1]));
        inputs[channel] = port;
      } else if (key == "output") {
        need(6, 7);
        int channel = parse_port_name(w[1], "OUT");
        OutputPort port;
        if (w[2] == "dust")
          port.kind = OutputKind::Dust;
        else if (w[2] == "component")
          port.kind = OutputKind::Component;
        else
          throw std::invalid_argument(
              std::format("unknown output kind '{}'", w[2]));
        port.position = {coordinate(3), coordinate(4), coordinate(5)};
        if (port.kind == OutputKind::Component && w.size() != 7)
          throw std::invalid_argument("a component output needs a facing");
        if (w.size() == 7)
          port.facing = parse_dir(w[6]);
        if (static_cast<int>(outputs.size()) <= channel)
          outputs.resize(channel + 1);
        if (outputs[channel])
          throw std::invalid_argument(std::format("{} defined twice", w[1]));
        outputs[channel] = port;
      } else if (key == "timing") {
        need(2, 3);
        spec.timing.earliest.reset();
        spec.timing.latest.reset();
        if (w.size() == 2 && w[1] != "none") {
          spec.timing.latest = parse_ticks(w[1]);
        } else if (w.size() == 3) {
          spec.timing.earliest = parse_ticks(w[1]);
          spec.timing.latest = parse_ticks(w[2]);
        }
      } else if (key == "hold_previous") {
        need(2, 2);
        if (w[1] != "on" && w[1] != "off")
          throw std::invalid_argument("hold_previous is on or off");
        spec.timing.hold_previous = w[1] == "on";
      } else if (key == "feedback") {
        need(2, 2);
        if (w[1] != "on" && w[1] != "off")
          throw std::invalid_argument("feedback is on or off");
        spec.allow_feedback = w[1] == "on";
      } else if (key == "tile") {
        need(2, 4);
        spec.tiled = {};
        for (std::size_t i = 1; i < w.size(); ++i) {
          if (w[i] == "none" && w.size() == 2)
            break;
          if (w[i] != "x" && w[i] != "y" && w[i] != "z")
            throw std::invalid_argument(
                std::format("tile axes are x, y, z or none, got '{}'", w[i]));
          spec.tiled[w[i][0] - 'x'] = true;
        }
      } else if (key == "settle") {
        need(2, 2);
        spec.timing.settle_ticks = parse_ticks(w[1]);
      } else if (key == "horizon") {
        need(2, 2);
        spec.timing.horizon_ticks = parse_ticks(w[1]);
      } else if (key == "palette") {
        spec.palette.clear();
        for (std::size_t i = 1; i < w.size(); ++i) {
          if (w[i] == "air")
            continue;
          std::optional<BlockType> type = parse_palette_block(w[i]);
          if (!type)
            throw std::invalid_argument(
                std::format("unknown block '{}'", w[i]));
          spec.palette.push_back(*type);
        }
      } else if (key == "row") {
        row_lines.emplace_back(line_number, trim(line.substr(3)));
      } else {
        throw std::invalid_argument(std::format("unknown keyword '{}'", key));
      }
    } catch (const std::invalid_argument &error) {
      std::string message = error.what();
      if (message.starts_with("line "))
        throw;
      throw std::invalid_argument(
          std::format("line {}: {}", line_number, message));
    }
  }

  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (!inputs[i])
      throw std::invalid_argument(std::format("IN{} is missing", i + 1));
    spec.inputs.push_back(*inputs[i]);
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (!outputs[i])
      throw std::invalid_argument(std::format("OUT{} is missing", i + 1));
    spec.outputs.push_back(*outputs[i]);
  }
  for (auto [number, row_text] : row_lines) {
    try {
      spec.rows.push_back(
          parse_truth_row(row_text, static_cast<int>(spec.inputs.size()),
                          static_cast<int>(spec.outputs.size())));
    } catch (const std::invalid_argument &error) {
      throw std::invalid_argument(
          std::format("line {}: {}", number, error.what()));
    }
  }
  spec.validate();
  return spec;
}

} // namespace rs

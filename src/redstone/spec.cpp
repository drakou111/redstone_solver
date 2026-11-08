#include "redstone/spec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
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

constexpr std::array<PaletteName, 10> kPaletteNames{{
    {BlockType::Solid, "solid"},
    {BlockType::Glass, "glass"},
    {BlockType::Dust, "dust"},
    {BlockType::Torch, "torch"},
    {BlockType::Repeater, "repeater"},
    {BlockType::Comparator, "comparator"},
    {BlockType::Target, "target"},
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
// "3" or "1..4" (inclusive).
std::pair<int, int> parse_span(std::string_view token) {
  std::size_t dots = token.find("..");
  if (dots == std::string_view::npos) {
    int value = parse_int(token, "coordinate");
    return {value, value};
  }
  int lo = parse_int(token.substr(0, dots), "coordinate");
  int hi = parse_int(token.substr(dots + 2), "coordinate");
  if (lo > hi)
    throw std::invalid_argument(
        std::format("coordinate range '{}' is backwards", token));
  return {lo, hi};
}

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
    if (port.is_free())
      continue;
    world.set(port.position,
              Block::input(static_cast<int>(i), port.kind, port.facing));
  }
  for (std::size_t i = 0; i < this->outputs.size(); ++i) {
    const OutputPort &port = this->outputs[i];
    if (port.is_free())
      continue;
    world.set(port.position,
              Block::output(static_cast<int>(i), port.kind, port.facing));
  }
  for (const auto &[position, block] : this->fixed)
    world.set(position, block);
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
          std::format("{} shares its position with another port or a "
                      "kept/empty cell",
                      name));
  };
  for (std::size_t i = 0; i < this->inputs.size(); ++i)
    if (!this->inputs[i].is_free())
      check_position(this->inputs[i].position, std::format("IN{}", i + 1));
  for (std::size_t i = 0; i < this->outputs.size(); ++i)
    if (!this->outputs[i].is_free())
      check_position(this->outputs[i].position, std::format("OUT{}", i + 1));
  for (const auto &[position, block] : this->fixed) {
    check_position(position,
                   std::format("kept {} at ({}, {}, {})", to_string(block),
                               position.x, position.y, position.z));
    if (block.type == BlockType::Air || block.type == BlockType::Input ||
        block.type == BlockType::Output)
      throw std::invalid_argument("kept cells must hold a placeable block");
  }
  for (const auto &[position, block] : this->hints) {
    check_position(position,
                   std::format("hint {} at ({}, {}, {})", to_string(block),
                               position.x, position.y, position.z));
    if (block.type == BlockType::Input || block.type == BlockType::Output)
      throw std::invalid_argument("hints must be placeable blocks or air");
  }
  for (Vec3 position : this->forbidden)
    check_position(position, std::format("empty cell ({}, {}, {})", position.x,
                                         position.y, position.z));

  // Regions may overlap each other but not fixed cells.
  auto check_region = [&](const std::vector<Vec3> &region,
                          std::string_view name) {
    std::set<std::tuple<int, int, int>> seen;
    for (Vec3 p : region) {
      if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= this->size.x ||
          p.y >= this->size.y || p.z >= this->size.z)
        throw std::invalid_argument(
            std::format("{}'s region is outside the volume", name));
      if (occupied.contains({p.x, p.y, p.z}))
        throw std::invalid_argument(std::format(
            "{}'s region includes a fixed port or a kept/empty cell", name));
      if (!seen.insert({p.x, p.y, p.z}).second)
        throw std::invalid_argument(
            std::format("{}'s region lists a cell twice", name));
    }
  };
  for (std::size_t i = 0; i < this->inputs.size(); ++i)
    check_region(this->inputs[i].region, std::format("IN{}", i + 1));
  for (std::size_t i = 0; i < this->outputs.size(); ++i)
    check_region(this->outputs[i].region, std::format("OUT{}", i + 1));

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

  // Port blocks reject invalid settings (e.g. a vertical repeater input).
  this->base_world();

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
  // (input?, channel) of ports declared "free" and their region cells.
  std::set<std::pair<bool, int>> free_ports;
  std::map<std::pair<bool, int>, std::vector<Vec3>> region_cells;
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
        bool free = w.size() == 4 && w[3] == "free";
        if (!free)
          need(6, 7);
        int channel = parse_port_name(w[1], "IN");
        InputPort port;
        std::optional<InputKind> kind = parse_input_kind(w[2]);
        if (!kind)
          throw std::invalid_argument(std::format(
              "unknown input kind '{}' (comparator, repeater, dust, container)",
              w[2]));
        port.kind = *kind;
        if (free) {
          free_ports.insert({true, channel});
          if (static_cast<int>(inputs.size()) <= channel)
            inputs.resize(channel + 1);
          if (inputs[channel])
            throw std::invalid_argument(std::format("{} defined twice", w[1]));
          inputs[channel] = port;
          continue;
        }
        port.position = {coordinate(3), coordinate(4), coordinate(5)};
        bool directional = port.kind == InputKind::Comparator ||
                           port.kind == InputKind::Repeater;
        if (directional && w.size() != 7)
          throw std::invalid_argument(
              std::format("a {} input needs a facing", w[2]));
        if (w.size() == 7)
          port.facing = parse_dir(w[6]);
        if (static_cast<int>(inputs.size()) <= channel)
          inputs.resize(channel + 1);
        if (inputs[channel])
          throw std::invalid_argument(std::format("{} defined twice", w[1]));
        inputs[channel] = port;
      } else if (key == "output") {
        bool free = w.size() == 4 && w[3] == "free";
        if (!free)
          need(6, 7);
        int channel = parse_port_name(w[1], "OUT");
        OutputPort port;
        std::optional<OutputKind> kind = parse_output_kind(w[2]);
        if (!kind)
          throw std::invalid_argument(std::format(
              "unknown output kind '{}' (dust, repeater, comparator)", w[2]));
        port.kind = *kind;
        if (free) {
          free_ports.insert({false, channel});
          if (static_cast<int>(outputs.size()) <= channel)
            outputs.resize(channel + 1);
          if (outputs[channel])
            throw std::invalid_argument(std::format("{} defined twice", w[1]));
          outputs[channel] = port;
          continue;
        }
        port.position = {coordinate(3), coordinate(4), coordinate(5)};
        if (port.kind != OutputKind::Dust && w.size() != 7)
          throw std::invalid_argument(
              std::format("a {} output needs a facing", w[2]));
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
      } else if (key == "support_layer") {
        need(2, 2);
        if (w[1] != "on" && w[1] != "off")
          throw std::invalid_argument("support_layer is on or off");
        spec.support_layer = w[1] == "on";
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
      } else if (key == "keep") {
        if (w.size() < 5)
          throw std::invalid_argument("keep needs x y z and a block");
        Vec3 position{coordinate(1), coordinate(2), coordinate(3)};
        spec.fixed.emplace_back(position,
                                parse_block_words({w.begin() + 4, w.end()}));
      } else if (key == "hint") {
        if (w.size() < 5)
          throw std::invalid_argument("hint needs x y z and a block");
        Vec3 position{coordinate(1), coordinate(2), coordinate(3)};
        spec.hints.emplace_back(position,
                                parse_block_words({w.begin() + 4, w.end()}));
      } else if (key == "region") {
        need(5, 5);
        bool input = w[1].starts_with("IN");
        int channel = parse_port_name(w[1], input ? "IN" : "OUT");
        std::array<std::pair<int, int>, 3> span;
        for (int axis = 0; axis < 3; ++axis)
          span[axis] = parse_span(w[2 + axis]);
        std::vector<Vec3> &cells = region_cells[{input, channel}];
        for (int x = span[0].first; x <= span[0].second; ++x)
          for (int y = span[1].first; y <= span[1].second; ++y)
            for (int z = span[2].first; z <= span[2].second; ++z)
              if (std::find(cells.begin(), cells.end(), Vec3{x, y, z}) ==
                  cells.end())
                cells.push_back({x, y, z});
      } else if (key == "empty") {
        need(4, 4);
        spec.forbidden.push_back({coordinate(1), coordinate(2), coordinate(3)});
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
  for (const auto &[port, cells] : region_cells) {
    std::string name =
        std::format("{}{}", port.first ? "IN" : "OUT", port.second + 1);
    if (!free_ports.contains(port))
      throw std::invalid_argument(
          std::format("region for {}, which is not declared free", name));
  }
  for (const auto &port : free_ports) {
    auto it = region_cells.find(port);
    std::string name =
        std::format("{}{}", port.first ? "IN" : "OUT", port.second + 1);
    if (it == region_cells.end())
      throw std::invalid_argument(
          std::format("{} is free but has no region lines", name));
    if (port.first)
      spec.inputs[port.second].region = it->second;
    else
      spec.outputs[port.second].region = it->second;
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

SignalRange parse_signal_range(std::string_view text) {
  text = trim(text);
  if (text.empty() || text == "*")
    return SignalRange{};
  if (text.starts_with("ss"))
    return parse_range(text);
  return parse_range(std::string("ss") + std::string(text));
}

std::string short_range_text(SignalRange range) {
  if (range == SignalRange{})
    return "*";
  if (range.min == range.max)
    return std::to_string(range.min);
  return std::format("{}..{}", range.min, range.max);
}

std::string format_spec(const Spec &spec) {
  std::string text;
  text += std::format("size {} {} {}\n", spec.size.x, spec.size.y, spec.size.z);
  for (std::size_t i = 0; i < spec.inputs.size(); ++i) {
    const InputPort &port = spec.inputs[i];
    if (port.is_free()) {
      text += std::format("input IN{} {} free\n", i + 1,
                          input_kind_name(port.kind));
      continue;
    }
    Vec3 p = port.position;
    bool directional =
        port.kind == InputKind::Comparator || port.kind == InputKind::Repeater;
    text += std::format("input IN{} {} {} {} {}{}\n", i + 1,
                        input_kind_name(port.kind), p.x, p.y, p.z,
                        directional ? " " + std::string(to_string(port.facing))
                                    : std::string());
  }
  for (std::size_t i = 0; i < spec.outputs.size(); ++i) {
    const OutputPort &port = spec.outputs[i];
    if (port.is_free()) {
      text += std::format("output OUT{} {} free\n", i + 1,
                          output_kind_name(port.kind));
      continue;
    }
    Vec3 p = port.position;
    text += std::format("output OUT{} {} {} {} {}{}\n", i + 1,
                        output_kind_name(port.kind), p.x, p.y, p.z,
                        port.kind != OutputKind::Dust
                            ? " " + std::string(to_string(port.facing))
                            : std::string());
  }

  const TimingSpec &t = spec.timing;
  if (t.earliest && t.latest)
    text += std::format("timing {} {}\n", *t.earliest, *t.latest);
  else if (t.latest)
    text += std::format("timing {}\n", *t.latest);
  else
    text += "timing none\n";
  text += std::format("hold_previous {}\n", t.hold_previous ? "on" : "off");
  text +=
      std::format("settle {}\nhorizon {}\n", t.settle_ticks, t.horizon_ticks);
  text += std::format("feedback {}\n", spec.allow_feedback ? "on" : "off");
  text += std::format("support_layer {}\n", spec.support_layer ? "on" : "off");
  std::string axes;
  for (int axis = 0; axis < 3; ++axis)
    if (spec.tiled[axis])
      axes += std::format(" {}", static_cast<char>('x' + axis));
  text += "tile" + (axes.empty() ? std::string(" none") : axes) + "\n";
  for (const auto &[p, block] : spec.fixed)
    text +=
        std::format("keep {} {} {} {}\n", p.x, p.y, p.z, block_words(block));
  for (Vec3 p : spec.forbidden)
    text += std::format("empty {} {} {}\n", p.x, p.y, p.z);
  for (const auto &[p, block] : spec.hints)
    text +=
        std::format("hint {} {} {} {}\n", p.x, p.y, p.z, block_words(block));
  for (std::size_t i = 0; i < spec.inputs.size(); ++i)
    for (Vec3 p : spec.inputs[i].region)
      text += std::format("region IN{} {} {} {}\n", i + 1, p.x, p.y, p.z);
  for (std::size_t i = 0; i < spec.outputs.size(); ++i)
    for (Vec3 p : spec.outputs[i].region)
      text += std::format("region OUT{} {} {} {}\n", i + 1, p.x, p.y, p.z);
  text += "palette";
  for (BlockType type : spec.palette)
    text += std::format(" {}", palette_name(type));
  text += "\n";

  for (const TruthRow &row : spec.rows) {
    auto side = [](const std::vector<SignalRange> &ranges,
                   std::string_view prefix) {
      std::string out;
      for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i] == SignalRange{})
          continue;
        out += std::format("{}{}{}({})", out.empty() ? "" : ",", prefix, i + 1,
                           to_string(ranges[i]));
      }
      return out;
    };
    text += std::format("row {} -> {}\n", side(row.inputs, "IN"),
                        side(row.outputs, "OUT"));
  }
  return text;
}

Block parse_block_words(const std::vector<std::string_view> &words) {
  if (words.empty())
    throw std::invalid_argument("missing block name");
  std::optional<BlockType> type = parse_palette_block(words[0]);
  if (!type)
    throw std::invalid_argument(std::format("unknown block '{}'", words[0]));
  auto expect = [&](std::size_t count, std::string_view form) {
    if (words.size() != count)
      throw std::invalid_argument(
          std::format("write {} as '{}'", words[0], form));
  };
  switch (*type) {
  case BlockType::Torch:
    expect(2, "torch <attached side>");
    return Block::torch(parse_dir(words[1]));
  case BlockType::Observer:
    expect(2, "observer <watched side>");
    return Block::observer(parse_dir(words[1]));
  case BlockType::Repeater:
    expect(3, "repeater <facing> <delay 1-4>");
    return Block::repeater(parse_dir(words[1]),
                           parse_int(words[2], "repeater delay"));
  case BlockType::Comparator:
    expect(3, "comparator <facing> compare|subtract");
    if (words[2] != "compare" && words[2] != "subtract")
      throw std::invalid_argument("comparator mode is compare or subtract");
    return Block::comparator(parse_dir(words[1]),
                             words[2] == "subtract" ? ComparatorMode::Subtract
                                                    : ComparatorMode::Compare);
  case BlockType::Hopper:
  case BlockType::Barrel:
  case BlockType::Chest: {
    expect(2, std::format("{} <signal 0-15>", words[0]));
    int signal = parse_int(words[1], "container signal");
    if (*type == BlockType::Hopper)
      return Block::hopper(signal);
    return *type == BlockType::Barrel ? Block::barrel(signal)
                                      : Block::chest(signal);
  }
  default:
    expect(1, words[0]);
    return Block{*type};
  }
}

std::string block_words(const Block &b) {
  std::string name(palette_name(b.type));
  switch (b.type) {
  case BlockType::Torch:
  case BlockType::Observer:
    return std::format("{} {}", name, to_string(b.facing));
  case BlockType::Repeater:
    return std::format("{} {} {}", name, to_string(b.facing),
                       b.repeater_delay());
  case BlockType::Comparator:
    return std::format("{} {} {}", name, to_string(b.facing),
                       b.comparator_mode() == ComparatorMode::Subtract
                           ? "subtract"
                           : "compare");
  case BlockType::Hopper:
  case BlockType::Barrel:
  case BlockType::Chest:
    return std::format("{} {}", name, b.stored_signal());
  default:
    return name;
  }
}

} // namespace rs

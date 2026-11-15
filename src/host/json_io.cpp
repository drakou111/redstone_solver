#include "host/json_io.hpp"

#include <format>
#include <stdexcept>

namespace rs::host {

namespace {

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument(message);
}

const Json &field(const Json &json, const char *name) {
  if (!json.is_object() || !json.contains(name))
    fail(std::format("missing field '{}'", name));
  return json.at(name);
}

int integer(const Json &json, const char *what) {
  if (!json.is_number_integer())
    fail(std::format("'{}' must be an integer", what));
  return json.get<int>();
}

Json vec3(Vec3 v) { return Json::array({v.x, v.y, v.z}); }

Vec3 vec3_from(const Json &json, const char *what) {
  if (!json.is_array() || json.size() != 3)
    fail(std::format("'{}' must be [x, y, z]", what));
  return {integer(json[0], what), integer(json[1], what),
          integer(json[2], what)};
}

Dir dir_from(const Json &json) {
  if (!json.is_string())
    fail("a facing must be a direction name");
  std::string name = json.get<std::string>();
  for (Dir d : kAllDirs)
    if (to_string(d) == name)
      return d;
  fail(std::format("unknown direction '{}'", name));
}

std::string_view type_name(BlockType type) {
  switch (type) {
  case BlockType::Air:
    return "air";
  case BlockType::Input:
    return "input";
  case BlockType::Output:
    return "output";
  case BlockType::Lever:
    return "lever";
  case BlockType::Button:
    return "button";
  case BlockType::Lamp:
    return "lamp";
  default:
    return palette_name(type);
  }
}

BlockType type_from(const std::string &name) {
  if (name == "air")
    return BlockType::Air;
  if (name == "input")
    return BlockType::Input;
  if (name == "output")
    return BlockType::Output;
  if (name == "lever")
    return BlockType::Lever;
  if (name == "button")
    return BlockType::Button;
  if (name == "lamp")
    return BlockType::Lamp;
  if (std::optional<BlockType> type = parse_palette_block(name))
    return *type;
  fail(std::format("unknown block type '{}'", name));
}

Json range_json(SignalRange r) { return Json::array({r.min, r.max}); }

SignalRange range_from(const Json &json) {
  if (!json.is_array() || json.size() != 2)
    fail("a signal range must be [min, max]");
  SignalRange r{integer(json[0], "range"), integer(json[1], "range")};
  if (r.min < 0 || r.max > kMaxSignal || r.min > r.max)
    fail(std::format("invalid signal range [{}, {}]", r.min, r.max));
  return r;
}

// Cells a free port may occupy; missing or empty for a fixed port.
std::vector<Vec3> region_from(const Json &port) {
  std::vector<Vec3> cells;
  for (const Json &cell : port.value("region", Json::array()))
    cells.push_back(vec3_from(cell, "region cell"));
  return cells;
}

Json optional_int(const std::optional<int> &value) {
  return value ? Json(*value) : Json(nullptr);
}

std::optional<int> optional_int_from(const Json &json, const char *what) {
  if (json.is_null())
    return std::nullopt;
  return integer(json, what);
}

} // namespace

Json to_json(const Spec &spec) {
  auto region = [](const std::vector<Vec3> &cells) {
    Json out = Json::array();
    for (Vec3 p : cells)
      out.push_back(vec3(p));
    return out;
  };
  Json inputs = Json::array();
  for (const InputPort &port : spec.inputs)
    inputs.push_back({{"kind", input_kind_name(port.kind)},
                      {"position", vec3(port.position)},
                      {"facing", to_string(port.facing)},
                      {"region", region(port.region)}});
  Json outputs = Json::array();
  for (const OutputPort &port : spec.outputs)
    outputs.push_back({{"kind", output_kind_name(port.kind)},
                       {"position", vec3(port.position)},
                       {"facing", to_string(port.facing)},
                       {"region", region(port.region)}});
  Json rows = Json::array();
  for (const TruthRow &row : spec.rows) {
    Json in = Json::array();
    Json out = Json::array();
    for (SignalRange r : row.inputs)
      in.push_back(range_json(r));
    for (SignalRange r : row.outputs)
      out.push_back(range_json(r));
    rows.push_back({{"inputs", in}, {"outputs", out}});
  }
  Json palette = Json::array();
  for (BlockType type : spec.palette)
    palette.push_back(palette_name(type));
  Json fixed = Json::array();
  for (const auto &[position, block] : spec.fixed) {
    Json entry = to_json(block);
    entry["position"] = vec3(position);
    fixed.push_back(std::move(entry));
  }
  Json forbidden = Json::array();
  for (Vec3 p : spec.forbidden)
    forbidden.push_back(vec3(p));
  Json hints = Json::array();
  for (const auto &[position, block] : spec.hints) {
    Json entry = to_json(block);
    entry["position"] = vec3(position);
    hints.push_back(std::move(entry));
  }

  const TimingSpec &t = spec.timing;
  return {{"size", vec3(spec.size)},
          {"inputs", inputs},
          {"outputs", outputs},
          {"rows", rows},
          {"timing",
           {{"earliest", optional_int(t.earliest)},
            {"latest", optional_int(t.latest)},
            {"holdPrevious", t.hold_previous},
            {"settle", t.settle_ticks},
            {"horizon", t.horizon_ticks}}},
          {"palette", palette},
          {"feedback", spec.allow_feedback},
          {"supportLayer", spec.support_layer},
          {"tiled", Json::array({spec.tiled[0], spec.tiled[1], spec.tiled[2]})},
          {"fixed", fixed},
          {"forbidden", forbidden},
          {"hints", hints}};
}

Spec spec_from_json(const Json &json) {
  Spec spec;
  spec.size = vec3_from(field(json, "size"), "size");
  for (const Json &port : field(json, "inputs")) {
    std::optional<InputKind> kind =
        parse_input_kind(field(port, "kind").get<std::string>());
    if (!kind)
      fail("unknown input kind");
    spec.inputs.push_back({*kind,
                           vec3_from(field(port, "position"), "position"),
                           dir_from(field(port, "facing")), region_from(port)});
  }
  for (const Json &port : field(json, "outputs")) {
    std::optional<OutputKind> kind =
        parse_output_kind(field(port, "kind").get<std::string>());
    if (!kind)
      fail("unknown output kind");
    spec.outputs.push_back(
        {*kind, vec3_from(field(port, "position"), "position"),
         dir_from(field(port, "facing")), region_from(port)});
  }
  for (const Json &row : field(json, "rows")) {
    TruthRow truth;
    for (const Json &r : field(row, "inputs"))
      truth.inputs.push_back(range_from(r));
    for (const Json &r : field(row, "outputs"))
      truth.outputs.push_back(range_from(r));
    spec.rows.push_back(std::move(truth));
  }
  const Json &timing = field(json, "timing");
  spec.timing.earliest =
      optional_int_from(field(timing, "earliest"), "earliest");
  spec.timing.latest = optional_int_from(field(timing, "latest"), "latest");
  spec.timing.hold_previous = field(timing, "holdPrevious").get<bool>();
  spec.timing.settle_ticks = integer(field(timing, "settle"), "settle");
  spec.timing.horizon_ticks = integer(field(timing, "horizon"), "horizon");
  for (const Json &name : field(json, "palette")) {
    std::optional<BlockType> type =
        parse_palette_block(name.get<std::string>());
    if (!type)
      fail(std::format("unknown block '{}'", name.get<std::string>()));
    spec.palette.push_back(*type);
  }
  spec.allow_feedback = field(json, "feedback").get<bool>();
  spec.support_layer = json.value("supportLayer", true);
  const Json &tiled = field(json, "tiled");
  if (!tiled.is_array() || tiled.size() != 3)
    fail("'tiled' must be [x, y, z] booleans");
  for (int axis = 0; axis < 3; ++axis)
    spec.tiled[axis] = tiled[axis].get<bool>();
  for (const Json &entry : json.value("fixed", Json::array()))
    spec.fixed.emplace_back(vec3_from(field(entry, "position"), "position"),
                            block_from_json(entry));
  for (const Json &cell : json.value("forbidden", Json::array()))
    spec.forbidden.push_back(vec3_from(cell, "forbidden cell"));
  for (const Json &entry : json.value("hints", Json::array()))
    spec.hints.emplace_back(vec3_from(field(entry, "position"), "position"),
                            block_from_json(entry));
  spec.validate();
  return spec;
}

Json to_json(const Block &b) {
  Json json{{"type", type_name(b.type)}};
  switch (b.type) {
  case BlockType::Torch:
  case BlockType::Observer:
  case BlockType::Button:
    json["facing"] = to_string(b.facing);
    break;
  case BlockType::Lever:
    json["facing"] = to_string(b.facing);
    json["on"] = b.value != 0;
    break;
  case BlockType::Repeater:
    json["facing"] = to_string(b.facing);
    json["delay"] = b.repeater_delay();
    break;
  case BlockType::Comparator:
    json["facing"] = to_string(b.facing);
    json["mode"] = b.comparator_mode() == ComparatorMode::Subtract ? "subtract"
                                                                   : "compare";
    break;
  case BlockType::Hopper:
  case BlockType::Barrel:
  case BlockType::Chest:
    json["signal"] = b.stored_signal();
    break;
  case BlockType::Input:
    json["kind"] = input_kind_name(b.input_kind());
    json["channel"] = b.channel;
    json["facing"] = to_string(b.facing);
    break;
  case BlockType::Output:
    json["kind"] = output_kind_name(b.output_kind());
    json["channel"] = b.channel;
    json["facing"] = to_string(b.facing);
    break;
  default:
    break;
  }
  return json;
}

Block block_from_json(const Json &json) {
  BlockType type = type_from(field(json, "type").get<std::string>());
  auto facing = [&] { return dir_from(field(json, "facing")); };
  switch (type) {
  case BlockType::Air:
    return Block::air();
  case BlockType::Solid:
    return Block::solid();
  case BlockType::Glass:
    return Block::glass();
  case BlockType::Dust:
    return Block::dust();
  case BlockType::RedstoneBlock:
    return Block::redstone_block();
  case BlockType::Target:
    return Block::target();
  case BlockType::NoteBlock:
    return Block::note_block();
  case BlockType::Torch:
    return Block::torch(facing());
  case BlockType::Observer:
    return Block::observer(facing());
  case BlockType::Repeater:
    return Block::repeater(facing(), integer(field(json, "delay"), "delay"));
  case BlockType::Comparator:
    return Block::comparator(facing(), field(json, "mode") == "subtract"
                                           ? ComparatorMode::Subtract
                                           : ComparatorMode::Compare);
  case BlockType::Hopper:
    return Block::hopper(integer(field(json, "signal"), "signal"));
  case BlockType::Barrel:
    return Block::barrel(integer(field(json, "signal"), "signal"));
  case BlockType::Chest:
    return Block::chest(integer(field(json, "signal"), "signal"));
  case BlockType::Input: {
    std::optional<InputKind> kind =
        parse_input_kind(field(json, "kind").get<std::string>());
    if (!kind)
      fail("unknown input kind");
    return Block::input(integer(field(json, "channel"), "channel"), *kind,
                        facing());
  }
  case BlockType::Output: {
    std::optional<OutputKind> kind =
        parse_output_kind(field(json, "kind").get<std::string>());
    if (!kind)
      fail("unknown output kind");
    return Block::output(integer(field(json, "channel"), "channel"), *kind,
                         facing());
  }
  case BlockType::Lever:
    return Block::lever(facing(), json.value("on", false));
  case BlockType::Button:
    return Block::button(facing());
  case BlockType::Lamp:
    return Block::lamp();
  }
  fail("unknown block type");
}

Json to_json(const World &world) {
  Json blocks = Json::array();
  for (int i = 0; i < world.volume(); ++i) {
    Vec3 p = world.position_of(i);
    const Block &b = world.at(p);
    if (b.type == BlockType::Air)
      continue;
    Json entry = to_json(b);
    entry["position"] = vec3(p);
    blocks.push_back(std::move(entry));
  }
  return {{"size", vec3(world.size())}, {"blocks", blocks}};
}

World world_from_json(const Json &json) {
  World world(vec3_from(field(json, "size"), "size"));
  for (const Json &entry : field(json, "blocks")) {
    Vec3 p = vec3_from(field(entry, "position"), "position");
    if (!world.in_bounds(p))
      fail(std::format("block at [{}, {}, {}] is outside the world", p.x, p.y,
                       p.z));
    world.set(p, block_from_json(entry));
  }
  return world;
}

Json to_json(const SolveReport &report) {
  Json json{{"status", to_string(report.status)},
            {"optimal", report.status == SolveStatus::Optimal},
            {"seconds", report.seconds},
            {"refinements", report.refinements}};
  if (report.solution) {
    json["blockCount"] = report.solution->block_count;
    json["layout"] = to_json(report.solution->layout);
  }
  return json;
}

Json to_json(const VerificationFailure &failure) {
  const Placement &placement = failure.placement;
  return {{"from", failure.from ? Json(*failure.from) : Json(nullptr)},
          {"to", failure.to},
          {"reason", failure.reason},
          {"interference", failure.tiling_interference},
          {"placement",
           {{"origin", vec3(placement.origin)},
            {"quarterTurns", placement.symmetry.quarter_turns()},
            {"mirrored", placement.symmetry.mirrored()}}}};
}

} // namespace rs::host

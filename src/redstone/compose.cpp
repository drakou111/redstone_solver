#include "redstone/compose.hpp"

#include "redstone/compose_parts.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace rs {

namespace compose_parts {

Spec make_spec(Vec3 size, const std::vector<Vec3> &inputs,
               const std::vector<Vec3> &outputs,
               const std::vector<StageRow> &rows, int horizon) {
  std::string text = std::format("size {} {} {}\n", size.x, size.y, size.z);
  for (std::size_t i = 0; i < inputs.size(); ++i)
    text += std::format("input IN{} repeater {} {} {} east\n", i + 1,
                        inputs[i].x, inputs[i].y, inputs[i].z);
  for (std::size_t o = 0; o < outputs.size(); ++o)
    text += std::format("output OUT{} dust {} {} {}\n", o + 1, outputs[o].x,
                        outputs[o].y, outputs[o].z);
  text += std::format("timing none\nhorizon {}\n", horizon);
  for (const StageRow &row : rows) {
    text += "row ";
    for (std::size_t i = 0; i < row.inputs.size(); ++i)
      text += std::format("{}IN{}(ss{})", i ? "," : "", i + 1,
                          row.inputs[i] ? kMaxSignal : 0);
    text += " ->";
    for (std::size_t o = 0; o < row.outputs.size(); ++o) {
      std::string range = !row.outputs[o]   ? "ss0..15"
                          : *row.outputs[o] ? "ss1..15"
                                            : "ss0";
      text += std::format("{}OUT{}({})", o ? "," : " ", o + 1, range);
    }
    text += "\n";
  }
  return parse_spec(text);
}

void add_layout(Cells &cells, const World &layout, Vec3 offset) {
  for (int i = 0; i < layout.volume(); ++i) {
    Vec3 p = layout.position_of(i);
    Block block = layout.at(p);
    if (block.type == BlockType::Air)
      continue;
    if (block.type == BlockType::Input)
      block = Block::repeater(block.facing);
    else if (block.type == BlockType::Output)
      block = Block::dust();
    cells[key(p + offset)] = block;
  }
}

void pin(Spec &spec, const Cells &cells,
         const std::function<bool(Vec3)> &free) {
  World base = spec.base_world();
  for (int i = 0; i < base.volume(); ++i) {
    Vec3 p = base.position_of(i);
    BlockType t = base.at(p).type;
    if (t == BlockType::Input || t == BlockType::Output)
      continue;
    if (auto it = cells.find(key(p)); it != cells.end())
      spec.fixed.emplace_back(p, it->second);
    else if (!free(p))
      spec.forbidden.push_back(p);
  }
}

std::vector<Group> group_gates(const Netlist &netlist,
                               const std::vector<int> &level) {
  const int n = netlist.inputs;
  std::vector<Group> groups;
  for (std::size_t g = 0; g < netlist.gates.size(); ++g) {
    const Gate &gate = netlist.gates[g];
    int node = n + static_cast<int>(g);
    auto same = std::find_if(groups.begin(), groups.end(), [&](const Group &x) {
      return x.a == gate.a && x.b == gate.b && level[x.nodes[0]] == level[node];
    });
    if (same == groups.end()) {
      groups.push_back({gate.a, gate.b, {}, {}});
      same = groups.end() - 1;
    }
    same->nodes.push_back(node);
    same->functions.push_back(gate.function);
  }
  return groups;
}

std::optional<std::map<std::vector<std::uint8_t>, GateModule>>
build_modules(const std::vector<Group> &groups,
              const NetlistComposeOptions &options, int &module_height,
              const ProgressCallback &progress) {
  GateLibraryOptions gate_options =
      gates_within_height(options.gates, options.max_height);
  gate_options.solver = options.solver;
  gate_options.cancel = options.cancel;
  std::map<std::vector<std::uint8_t>, GateModule> modules;
  module_height = 2;
  for (const Group &group : groups) {
    if (modules.contains(group.functions))
      continue;
    std::optional<GateModule> m =
        build_gate(group.functions, gate_options, progress);
    if (!m)
      return std::nullopt;
    module_height = std::max(module_height, m->layout.size().y);
    modules.emplace(group.functions, std::move(*m));
  }
  return modules;
}

} // namespace compose_parts

namespace {

using namespace compose_parts;

// What a column holds along z: a gate group's module or a pass-through
// track for one signal.
struct Slot {
  int group = -1; // index into groups; -1: a track
  int node = 0;   // the track's signal
  int z = 0;
};

struct Column {
  int width = 1;
  std::vector<Slot> slots;
};

} // namespace

std::optional<ComposeResult>
compose_netlist(const Netlist &netlist, const BoolTable &table,
                const NetlistComposeOptions &options,
                const ProgressCallback &progress) {
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  const int n = netlist.inputs;
  const std::vector<int> level = netlist.levels();
  int depth_levels = 0;
  for (int node = n; node < static_cast<int>(level.size()); ++node)
    depth_levels = std::max(depth_levels, level[node]);

  // Group gates by level and operands; build one module per group.
  std::vector<Group> groups = group_gates(netlist, level);
  int module_height = 2;
  std::optional<std::map<std::vector<std::uint8_t>, GateModule>> built =
      build_modules(groups, options, module_height, progress);
  if (!built) {
    say("no module for one of the gate groups");
    return std::nullopt;
  }
  const std::map<std::vector<std::uint8_t>, GateModule> &modules = *built;
  auto module_of = [&](int group) -> const GateModule & {
    return modules.at(groups[group].functions);
  };

  // Z layout, column by column: items sorted by where their sources are.
  // Column 0 holds the inputs, 1..D the gate levels, D + 1 the outputs.
  auto needed_after = [&](int node, int column) {
    for (std::size_t g = 0; g < netlist.gates.size(); ++g) {
      int user = n + static_cast<int>(g);
      if (level[user] > column &&
          (netlist.gates[g].a == node || netlist.gates[g].b == node))
        return true;
    }
    return std::find(netlist.outputs.begin(), netlist.outputs.end(), node) !=
           netlist.outputs.end();
  };
  std::vector<Column> columns(depth_levels + 2);
  std::vector<double> at(level.size(), 0.0); // z a node's signal leaves at
  for (int i = 0; i < n; ++i) {
    columns[0].slots.push_back({-1, i, 2 * i});
    at[i] = 2 * i;
  }
  int depth = 2 * n - 1;
  for (int c = 1; c <= depth_levels; ++c) {
    struct Item {
      double key;
      Slot slot;
    };
    std::vector<Item> items;
    for (std::size_t gi = 0; gi < groups.size(); ++gi)
      if (level[groups[gi].nodes[0]] == c)
        items.push_back({(at[groups[gi].a] + at[groups[gi].b]) / 2,
                         {static_cast<int>(gi), 0, 0}});
    for (int node = 0; node < static_cast<int>(level.size()); ++node)
      if (level[node] < c && needed_after(node, c))
        items.push_back({at[node], {-1, node, 0}});
    std::stable_sort(
        items.begin(), items.end(),
        [](const Item &x, const Item &y) { return x.key < y.key; });
    int cursor = 0;
    for (Item &item : items) {
      item.slot.z = cursor;
      if (item.slot.group >= 0) {
        const GateModule &m = module_of(item.slot.group);
        const Group &group = groups[item.slot.group];
        columns[c].width = std::max(columns[c].width, m.layout.size().x);
        for (std::size_t o = 0; o < group.nodes.size(); ++o)
          at[group.nodes[o]] = cursor + m.outs[o].z;
        cursor += m.layout.size().z + 1;
      } else {
        at[item.slot.node] = cursor;
        cursor += 2;
      }
      columns[c].slots.push_back(item.slot);
    }
    depth = std::max(depth, cursor - 1);
  }
  {
    std::vector<std::pair<double, int>> outs;
    for (std::size_t o = 0; o < netlist.outputs.size(); ++o)
      outs.emplace_back(at[netlist.outputs[o]], static_cast<int>(o));
    std::stable_sort(outs.begin(), outs.end());
    std::vector<Slot> slots(netlist.outputs.size());
    for (std::size_t k = 0; k < outs.size(); ++k)
      slots[outs[k].second] = {-1, netlist.outputs[outs[k].second],
                               2 * static_cast<int>(k)};
    columns.back().slots = slots;
    depth = std::max(depth, 2 * static_cast<int>(outs.size()) - 1);
  }
  const int size_z = depth + 1;

  std::vector<Vec3> input_ports;
  for (const Slot &s : columns[0].slots)
    input_ports.push_back({0, 1, s.z});
  const int vectors = 1 << n;

  // A spare layer above the modules lets wires cross. Each channel may use
  // it from the start (a flat-only try that fails costs a full time limit);
  // an unused top layer is dropped at the end.
  int extra_layers = options.extra_layers;
  if (options.max_height > 0)
    extra_layers =
        std::clamp(options.max_height - module_height, 0, extra_layers);
  if (extra_layers < options.extra_layers)
    say(std::format("no spare layer for crossings (height limit {})",
                    options.max_height));
  const int height = module_height + extra_layers;
  Cells placed; // everything verified so far
  int x = 1;    // first cell east of column 0
  for (int k = 0; k <= depth_levels; ++k) {
    const Column &next = columns[k + 1];
    const bool last = k == depth_levels;
    bool done = false;
    for (int width = options.channel_width;
         width <= options.max_channel_width && !done; ++width)
      for (bool upper : {extra_layers > 0}) {
        if (done)
          continue;
        if (cancelled())
          return std::nullopt;
        const int x0 = x + width; // where column k + 1 starts
        std::vector<Vec3> probes;
        std::vector<int> probe_nodes;
        Cells cells = placed;
        std::vector<std::array<int, 2>> track_rows; // {z, x end}
        for (const Slot &s : next.slots) {
          if (last) {
            probes.push_back({x0, 1, s.z});
            probe_nodes.push_back(s.node);
          } else if (s.group >= 0) {
            const GateModule &m = module_of(s.group);
            probes.push_back({x0 - 1, 1, s.z + m.in1.z});
            probe_nodes.push_back(groups[s.group].a);
            probes.push_back({x0 - 1, 1, s.z + m.in2.z});
            probe_nodes.push_back(groups[s.group].b);
            add_layout(cells, m.layout, {x0, 0, s.z});
          } else {
            probes.push_back({x0 + next.width - 1, 1, s.z});
            probe_nodes.push_back(s.node);
            track_rows.push_back({s.z, x0 + next.width - 1});
          }
        }
        std::vector<StageRow> rows;
        for (int t = 0; t < vectors; ++t) {
          StageRow row;
          std::vector<bool> v = netlist.evaluate(t);
          for (int i = 0; i < n; ++i)
            row.inputs.push_back(v[i]);
          if (last) {
            for (const auto &column : table.values)
              row.outputs.push_back(column[t]);
          } else {
            for (int node : probe_nodes)
              row.outputs.push_back(v[node]);
          }
          rows.push_back(std::move(row));
        }
        const Vec3 size{last ? x0 + 1 : x0 + next.width, height, size_z};
        Spec spec = make_spec(size, input_ports, probes, rows, options.horizon);
        pin(spec, cells, [&](Vec3 p) {
          if (!upper && p.y >= module_height)
            return false;
          if (p.x >= x && p.x < x0)
            return true; // the channel
          if (p.y == 0 && (p.x == 0 || (last && p.x == x0)))
            return true; // supports under the input and output ports
          for (const auto &[z, x_end] : track_rows)
            if (p.z == z && p.x >= x0 && p.x <= x_end)
              return true; // a pass-through track
          return false;
        });
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = width < options.comfortable_width
                                        ? options.narrow_seconds
                                        : options.stage_seconds;
        solver.stop_at_first = true;
        solver.cancel = options.cancel;
        SolveReport report = solve(spec, solver);
        say(std::format("{} {} (width {}{}): {}", last ? "outputs" : "channel",
                        k + 1, width, upper ? ", upper layer" : "",
                        report.solution ? std::string_view("wired")
                                        : to_string(report.status)));
        if (!report.solution)
          continue;
        done = true;
        if (last) {
          // The finished circuit: the table's rows, the layout as hints,
          // without empty top layers.
          const World &full = report.solution->layout;
          int top = 0;
          for (int i = 0; i < full.volume(); ++i) {
            Vec3 p = full.position_of(i);
            if (full.at(p).type != BlockType::Air)
              top = std::max(top, p.y);
          }
          Vec3 cropped{size.x, top + 1, size.z};
          World layout(cropped);
          Spec result =
              make_spec(cropped, input_ports, probes, rows, options.horizon);
          for (int i = 0; i < full.volume(); ++i) {
            Vec3 p = full.position_of(i);
            const Block &b = full.at(p);
            if (b.type == BlockType::Air)
              continue;
            layout.set(p, b);
            if (b.type != BlockType::Input && b.type != BlockType::Output)
              result.hints.emplace_back(p, b);
          }
          return ComposeResult{std::move(result), std::move(layout)};
        }
        placed.clear();
        add_layout(placed, report.solution->layout, {0, 0, 0});
        x = x0 + next.width;
      }
    if (!done)
      return std::nullopt;
  }
  return std::nullopt;
}

GateLibraryOptions gates_within_height(GateLibraryOptions gates,
                                       int max_height) {
  if (max_height <= 0)
    return gates;
  std::erase_if(gates.sizes, [&](Vec3 size) { return size.y > max_height; });
  if (gates.sizes.empty())
    throw std::invalid_argument(std::format(
        "no gate fits in {} layers; Compose needs at least 2", max_height));
  return gates;
}

std::optional<ComposeResult> compose_spec(const Spec &spec,
                                          const NetlistComposeOptions &options,
                                          const ProgressCallback &progress) {
  BoolTable table = boolean_table(spec);
  SynthesisOptions synthesis;
  synthesis.cancel = options.cancel;
  std::optional<Netlist> netlist = synthesize(table, synthesis);
  if (!netlist) {
    if (progress)
      progress("no gate network found (too many gates)");
    return std::nullopt;
  }
  if (progress)
    progress(std::format("gate network:\n{}", netlist->describe()));
  if (options.flush) {
    if (std::optional<ComposeResult> flush =
            compose_flush(*netlist, table, options, progress))
      return flush;
    if (options.cancel && options.cancel->load())
      return std::nullopt;
    if (progress)
      progress("flush placement failed; laying out in columns");
  }
  return compose_netlist(*netlist, table, options, progress);
}

} // namespace rs

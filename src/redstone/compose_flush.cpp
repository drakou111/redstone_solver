// Flush composition: gate modules are placed one at a time, in signal order,
// right against the circuit built so far and shifted so one input sits on
// the cell its signal leaves from. Only the other input needs a wire, solved
// in a narrow corridor with everything else pinned, so each solve is small.

#include <algorithm>
#include <format>
#include <limits>
#include <set>
#include <string>

#include "redstone/compose.hpp"
#include "redstone/compose_parts.hpp"
#include "redstone/verifier.hpp"

namespace rs {

namespace {

using namespace compose_parts;

// Rows kept free north of the first modules (lower z) for wires.
// Generous: lining a module up may move it north; empty rows are cropped.
constexpr int kMarginZ = 16;

// A placed module's cells, both ends included.
struct Footprint {
  int x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};

// Modules side by side along z need an empty row between them (their dust
// would join); touching along x is fine (their faces are clean).
bool clashes(const Footprint &a, const Footprint &b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.z0 <= b.z1 + 1 && b.z0 <= a.z1 + 1;
}

bool inside(const Footprint &f, Vec3 p) {
  return p.x >= f.x0 && p.x <= f.x1 && p.z >= f.z0 && p.z <= f.z1;
}

// A wire to solve: from the cell a signal is at to the cell in front of a
// module input.
struct Route {
  Vec3 from;
  Vec3 to;
};

struct Spot {
  int x0 = 0;
  int z = 0;
  Footprint footprint;
  std::vector<Route> routes;
  std::vector<std::pair<int, Vec3>> new_ports; // input index, port cell
  long score = 0;
};

} // namespace

std::optional<ComposeResult> compose_flush(const Netlist &netlist,
                                           const BoolTable &table,
                                           const NetlistComposeOptions &options,
                                           const ProgressCallback &progress) {
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  const int n = netlist.inputs;
  const std::vector<int> level = netlist.levels();
  const std::vector<Group> groups = group_gates(netlist, level);
  int module_height = 2;
  std::optional<std::map<std::vector<std::uint8_t>, GateModule>> modules =
      build_modules(groups, options, module_height, progress);
  if (!modules) {
    say("flush: no module for one of the gate groups");
    return std::nullopt;
  }
  int extra_layers = options.extra_layers;
  if (options.max_height > 0)
    extra_layers =
        std::clamp(options.max_height - module_height, 0, extra_layers);
  const int height = module_height + extra_layers;

  // A signal is kept correct until its last reader is placed.
  const int nodes = n + static_cast<int>(netlist.gates.size());
  const int forever = static_cast<int>(groups.size());
  std::vector<int> last_use(nodes, -1);
  for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
    last_use[groups[gi].a] = std::max(last_use[groups[gi].a], gi);
    last_use[groups[gi].b] = std::max(last_use[groups[gi].b], gi);
  }
  for (int out : netlist.outputs)
    last_use[out] = forever;

  Cells placed;
  std::vector<Footprint> footprints;
  std::vector<std::optional<Vec3>> source(nodes); // where a signal leaves
  std::vector<std::optional<Vec3>> port(n);       // input port cells
  int next_free_z = kMarginZ;

  auto occupied = [&](Vec3 p) {
    if (placed.contains(key(p)))
      return true;
    return std::any_of(footprints.begin(), footprints.end(),
                       [&](const Footprint &f) { return inside(f, p); });
  };

  for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
    if (cancelled())
      return std::nullopt;
    const Group &group = groups[gi];
    const GateModule &m = modules->at(group.functions);
    const Vec3 msize = m.layout.size();
    const std::array<int, 2> operands{group.a, group.b};
    const std::array<int, 2> input_z{m.in1.z, m.in2.z};
    std::string names;
    for (std::uint8_t f : group.functions)
      names += (names.empty() ? "" : "+") + function_name(f);

    // When no function cares about input order, the operands may swap
    // inputs: twice the ways to line a signal up.
    const bool symmetric = std::all_of(
        group.functions.begin(), group.functions.end(),
        [](std::uint8_t f) { return ((f >> 1) & 1) == ((f >> 2) & 1); });

    // Spots: line an input up with its signal, or leave one cell between.
    struct Choice {
      int x0 = 0;
      int z = 0;
      bool swapped = false;
    };
    std::vector<Choice> spots;
    for (bool swapped : {false, true}) {
      if (swapped && !symmetric)
        continue;
      for (int k = 0; k < 2; ++k) {
        const int op = swapped ? operands[1 - k] : operands[k];
        if (!source[op])
          continue;
        Vec3 s = *source[op];
        spots.push_back({s.x + 1, s.z - input_z[k], swapped});
        spots.push_back({s.x + 2, s.z - input_z[k], swapped});
      }
    }
    if (spots.empty())
      spots.push_back({1, next_free_z, false});

    std::vector<Spot> candidates;
    for (auto [x0, z, swapped] : spots) {
      if (z < 0)
        continue;
      const std::array<int, 2> ops =
          swapped ? std::array<int, 2>{operands[1], operands[0]} : operands;
      Spot p{x0, z, {x0, x0 + msize.x - 1, z, z + msize.z - 1}, {}, {}, 0};
      bool ok = std::none_of(
          footprints.begin(), footprints.end(),
          [&](const Footprint &f) { return clashes(f, p.footprint); });
      for (int x = p.footprint.x0; ok && x <= p.footprint.x1; ++x)
        for (int y = 0; ok && y < height; ++y)
          for (int zz = p.footprint.z0; ok && zz <= p.footprint.z1; ++zz)
            ok = !placed.contains(key({x, y, zz}));
      long route_length = 0;
      for (int k = 0; ok && k < 2; ++k) {
        const Vec3 target{x0 - 1, 1, z + input_z[k]};
        Vec3 from;
        if (source[ops[k]]) {
          from = *source[ops[k]];
        } else {
          from = {0, 1, target.z}; // a new input port on the west side
          bool taken = occupied(from);
          for (const auto &[index, cell] : p.new_ports)
            taken |= cell == from;
          if (taken) {
            ok = false;
            continue;
          }
          p.new_ports.push_back({ops[k], from});
        }
        if (from == target)
          continue; // lined up: no wire
        if (occupied(target)) {
          ok = false;
          continue;
        }
        p.routes.push_back({from, target});
        route_length +=
            std::abs(from.x - target.x) + std::abs(from.z - target.z);
      }
      if (!ok)
        continue;
      int max_x = p.footprint.x1;
      int max_z = p.footprint.z1;
      for (const auto &[cell, block] : placed) {
        max_x = std::max(max_x, cell[0]);
        max_z = std::max(max_z, cell[2]);
      }
      p.score = static_cast<long>(max_x + 1) * (max_z + 1) * 4 + route_length;
      candidates.push_back(std::move(p));
    }
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Spot &a, const Spot &b) { return a.score < b.score; });
    if (static_cast<int>(candidates.size()) > options.flush_candidates)
      candidates.resize(options.flush_candidates);

    bool done = false;
    for (const Spot &p : candidates) {
      if (cancelled())
        return std::nullopt;
      Cells cells = placed;
      add_layout(cells, m.layout, {p.x0, 0, p.z});
      std::set<std::array<int, 3>> free_cells;
      for (const Route &r : p.routes)
        for (int x = std::min(r.from.x, r.to.x);
             x <= std::max(r.from.x, r.to.x); ++x)
          for (int z = std::max(0, std::min(r.from.z, r.to.z) - 1);
               z <= std::max(r.from.z, r.to.z) + 1; ++z)
            for (int y = 0; y < height; ++y)
              if (!cells.contains(key({x, y, z})))
                free_cells.insert(key({x, y, z}));

      // Ports: every input placed so far plus the new ones, in input order.
      std::vector<std::optional<Vec3>> ports = port;
      for (const auto &[index, cell] : p.new_ports)
        ports[index] = cell;
      std::vector<int> stage_inputs;
      std::vector<Vec3> input_cells;
      for (int i = 0; i < n; ++i)
        if (ports[i]) {
          stage_inputs.push_back(i);
          input_cells.push_back(*ports[i]);
        }
      // Outputs: the new module's, and older signals still to be read.
      std::vector<int> watched;
      std::vector<Vec3> output_cells;
      for (std::size_t o = 0; o < group.nodes.size(); ++o) {
        watched.push_back(group.nodes[o]);
        output_cells.push_back({p.x0 + m.outs[o].x, 1, p.z + m.outs[o].z});
      }
      for (int node = n; node < nodes; ++node)
        if (source[node] && last_use[node] > gi) {
          watched.push_back(node);
          output_cells.push_back(*source[node]);
        }

      Vec3 size{p.footprint.x1 + 1, height, p.footprint.z1 + 1};
      auto grow = [&](const std::array<int, 3> &c) {
        size.x = std::max(size.x, c[0] + 1);
        size.z = std::max(size.z, c[2] + 1);
      };
      for (const auto &[cell, block] : cells)
        grow(cell);
      for (const auto &cell : free_cells)
        grow(cell);
      for (Vec3 c : input_cells)
        grow(key(c));

      std::vector<StageRow> rows;
      for (int mask = 0; mask < (1 << stage_inputs.size()); ++mask) {
        int vector = 0;
        StageRow row;
        for (std::size_t i = 0; i < stage_inputs.size(); ++i) {
          bool on = ((mask >> i) & 1) != 0;
          row.inputs.push_back(on);
          if (on)
            vector |= 1 << stage_inputs[i];
        }
        std::vector<bool> values = netlist.evaluate(vector);
        for (int node : watched)
          row.outputs.push_back(values[node]);
        rows.push_back(std::move(row));
      }
      Spec spec =
          make_spec(size, input_cells, output_cells, rows, options.horizon);
      // Input ports are repeaters: the floor under them may hold a support.
      pin(spec, cells, [&](Vec3 c) {
        return free_cells.contains(key(c)) || (c.y == 0 && c.x == 0);
      });

      SolverOptions solver = options.solver;
      solver.time_limit_seconds = options.flush_seconds;
      solver.stop_at_first = true;
      solver.cancel = options.cancel;
      SolveReport report = solve(spec, solver);
      say(std::format("gate {} ({}) at x {} z {}{}: {}", gi + 1, names, p.x0,
                      p.z, p.routes.empty() ? ", no wire" : "",
                      report.solution ? std::string_view("wired")
                                      : to_string(report.status)));
      if (!report.solution)
        continue;
      placed.clear();
      add_layout(placed, report.solution->layout, {0, 0, 0});
      footprints.push_back(p.footprint);
      for (const auto &[index, cell] : p.new_ports) {
        port[index] = cell;
        source[index] = cell;
      }
      for (std::size_t o = 0; o < group.nodes.size(); ++o)
        source[group.nodes[o]] = Vec3{p.x0 + m.outs[o].x, 1, p.z + m.outs[o].z};
      next_free_z = std::max(next_free_z, p.footprint.z1 + 2);
      done = true;
      break;
    }
    if (!done) {
      say(std::format("flush: no place for gate {} ({})", gi + 1, names));
      return std::nullopt;
    }
  }

  // The finished circuit: every input and output must be a placed cell.
  std::vector<Vec3> inputs;
  for (int i = 0; i < n; ++i) {
    if (!port[i]) {
      say("flush: an input no gate reads is not supported");
      return std::nullopt;
    }
    inputs.push_back(*port[i]);
  }
  std::vector<Vec3> outputs;
  for (int out : netlist.outputs) {
    if (out < n || !source[out] ||
        std::find(outputs.begin(), outputs.end(), *source[out]) !=
            outputs.end()) {
      say("flush: an output that is an input or repeats another is not "
          "supported");
      return std::nullopt;
    }
    outputs.push_back(*source[out]);
  }
  int min_z = std::numeric_limits<int>::max();
  int max_x = 0;
  int max_z = 0;
  int top = 1;
  for (const auto &[cell, block] : placed) {
    min_z = std::min(min_z, cell[2]);
    max_x = std::max(max_x, cell[0]);
    max_z = std::max(max_z, cell[2]);
    top = std::max(top, cell[1]);
  }
  const Vec3 shift{0, 0, -min_z};
  for (Vec3 &c : inputs)
    c = c + shift;
  for (Vec3 &c : outputs)
    c = c + shift;
  const Vec3 size{max_x + 1, top + 1, max_z - min_z + 1};

  std::vector<StageRow> rows;
  for (int t = 0; t < (1 << n); ++t) {
    StageRow row;
    for (int i = 0; i < n; ++i)
      row.inputs.push_back(((t >> i) & 1) != 0);
    for (const auto &column : table.values)
      row.outputs.push_back(column[t]);
    rows.push_back(std::move(row));
  }
  Spec spec = make_spec(size, inputs, outputs, rows, options.horizon);
  World layout = spec.base_world();
  for (const auto &[cell, block] : placed) {
    Vec3 p = Vec3{cell[0], cell[1], cell[2]} + shift;
    BlockType here = layout.at(p).type;
    if (here == BlockType::Input || here == BlockType::Output)
      continue;
    layout.set(p, block);
    spec.hints.emplace_back(p, block);
  }
  if (std::optional<VerificationFailure> failure =
          verify_layout(layout, spec, spec.verification_vectors(),
                        options.solver.verification)) {
    say("flush: the finished circuit failed its check: " + failure->reason);
    return std::nullopt;
  }
  say(std::format("flush: {}x{}x{}", size.x, size.y, size.z));
  return ComposeResult{std::move(spec), std::move(layout)};
}

} // namespace rs

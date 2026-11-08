// Flush composition: gate modules are placed one at a time, in signal order,
// right against the circuit built so far and shifted so one input sits on
// the cell its signal leaves from. Only the other input needs a wire, solved
// in a narrow corridor with everything else pinned, so each solve is small.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <future>
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
constexpr std::chrono::milliseconds kPollInterval{50};

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

// Where a gate goes: a module at (x0, z), or (join) no module at all, its
// two signals wired into one dust cell.
struct Spot {
  int x0 = 0;
  int z = 0;
  Footprint footprint;
  std::vector<Route> routes;
  std::vector<std::pair<int, Vec3>> new_ports; // input index, port cell
  long score = 0;
  bool join = false;
  std::vector<Vec3> outputs; // where the gate's signals leave
};

// A partial circuit: every gate so far placed and wired.
struct FlushState {
  Cells placed;
  std::vector<Footprint> footprints;
  std::vector<std::optional<Vec3>> source; // where a signal leaves
  std::vector<std::optional<Vec3>> port;   // input port cells
  int next_free_z = 0;
};

// Floor area of everything placed: smaller partial circuits finish smaller.
long area_of(const FlushState &state) {
  if (state.placed.empty())
    return 0;
  int min_z = std::numeric_limits<int>::max();
  int max_x = 0;
  int max_z = 0;
  for (const auto &[cell, block] : state.placed) {
    min_z = std::min(min_z, cell[2]);
    max_x = std::max(max_x, cell[0]);
    max_z = std::max(max_z, cell[2]);
  }
  return static_cast<long>(max_x + 1) * (max_z - min_z + 1);
}

constexpr std::uint8_t kOr = 0b1110;
constexpr std::uint8_t kXor = 0b0110;

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

  // A beam of the best partial circuits: each gate is placed in every one of
  // them, and the smallest few results go on to the next gate.
  std::vector<FlushState> beam(1);
  beam[0].source.resize(nodes);
  beam[0].port.resize(n);
  beam[0].next_free_z = kMarginZ;
  const int beam_width = std::max(1, options.flush_beam);
  // Rows beside a wire's straight path it may also use (to go around).
  const int margin = std::max(1, options.flush_margin);

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

    std::vector<FlushState> next;
    for (const FlushState &state : beam) {
      const Cells &placed = state.placed;
      const std::vector<Footprint> &footprints = state.footprints;
      const std::vector<std::optional<Vec3>> &source = state.source;
      const std::vector<std::optional<Vec3>> &port = state.port;
      const int next_free_z = state.next_free_z;
      auto occupied = [&](Vec3 p) {
        if (placed.contains(key(p)))
          return true;
        return std::any_of(footprints.begin(), footprints.end(),
                           [&](const Footprint &f) { return inside(f, p); });
      };

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
          for (int gap = 1; gap <= 1 + options.flush_spacing; ++gap)
            spots.push_back({s.x + 1 + gap, s.z - input_z[k], swapped});
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
        for (Vec3 out : m.outs)
          p.outputs.push_back({x0 + out.x, 1, z + out.z});
        candidates.push_back(std::move(p));
      }
      std::stable_sort(
          candidates.begin(), candidates.end(),
          [](const Spot &a, const Spot &b) { return a.score < b.score; });
      if (static_cast<int>(candidates.size()) > options.flush_candidates)
        candidates.resize(options.flush_candidates);

      // An OR of on/off signals is two dust lines meeting (dust carries the
      // stronger signal); so is an XOR whose inputs are never on together.
      // Tried first: it needs no module at all.
      if (options.flush_joins && group.functions.size() == 1 &&
          source[group.a] && source[group.b]) {
        const std::uint8_t f = group.functions[0];
        bool joinable = f == kOr;
        if (f == kXor) {
          joinable = true;
          for (int t = 0; t < (1 << n) && joinable; ++t) {
            std::vector<bool> v = netlist.evaluate(t);
            joinable = !(v[group.a] && v[group.b]);
          }
        }
        const Vec3 sa = *source[group.a];
        const Vec3 sb = *source[group.b];
        std::vector<Spot> joins;
        for (int z : {sa.z, sb.z}) {
          if (!joinable)
            break;
          const Vec3 meet{std::max(sa.x, sb.x) + 1, 1, z};
          if (occupied(meet))
            continue;
          Spot j;
          j.join = true;
          j.x0 = meet.x;
          j.z = meet.z;
          j.footprint = {meet.x, meet.x, meet.z, meet.z};
          for (Vec3 from : {sa, sb})
            if (from != meet)
              j.routes.push_back({from, meet});
          j.outputs = {meet};
          int max_x = meet.x;
          int max_z = meet.z;
          for (const auto &[cell, block] : placed) {
            max_x = std::max(max_x, cell[0]);
            max_z = std::max(max_z, cell[2]);
          }
          j.score = static_cast<long>(max_x + 1) * (max_z + 1) * 4;
          joins.push_back(std::move(j));
        }
        candidates.insert(candidates.begin(), joins.begin(), joins.end());
      }

      // Every position's stage is built, then all are solved side by side; the
      // best-ranked one that wires up is kept and the rest are stopped.
      std::vector<Spec> stages;
      for (const Spot &p : candidates) {
        Cells cells = placed;
        if (!p.join)
          add_layout(cells, m.layout, {p.x0, 0, p.z});
        std::set<std::array<int, 3>> free_cells;
        for (const Route &r : p.routes)
          for (int x = std::min(r.from.x, r.to.x);
               x <= std::max(r.from.x, r.to.x); ++x)
            for (int z = std::max(0, std::min(r.from.z, r.to.z) - margin);
                 z <= std::max(r.from.z, r.to.z) + margin; ++z)
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
          output_cells.push_back(p.outputs[o]);
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
        Spec spec = make_spec(size, input_cells, output_cells, rows,
                              options.horizon, options.palette);
        // Input ports are repeaters: the floor under them may hold a support.
        pin(spec, cells, [&](Vec3 c) {
          return free_cells.contains(key(c)) || (c.y == 0 && c.x == 0);
        });
        stages.push_back(std::move(spec));
      }

      // One position at a time, best-ranked first: solving them side by side
      // made every one run out of time (the multiplexer's third gate).
      int children = 0;
      for (std::size_t i = 0; i < stages.size() && children < beam_width; ++i) {
        if (cancelled())
          return std::nullopt;
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = options.flush_seconds;
        solver.stop_at_first = true;
        solver.cancel = options.cancel;
        SolveReport report = solve(stages[i], solver);
        const Spot &p = candidates[i];
        say(std::format("gate {} ({}){} at x {} z {}{}: {}", gi + 1, names,
                        p.join ? " as joined wires" : "", p.x0, p.z,
                        p.routes.empty() ? ", no wire" : "",
                        report.solution ? std::string_view("wired")
                                        : to_string(report.status)));
        if (!report.solution)
          continue;
        FlushState child = state;
        child.placed.clear();
        add_layout(child.placed, report.solution->layout, {0, 0, 0});
        if (!p.join) {
          child.footprints.push_back(p.footprint);
          child.next_free_z = std::max(child.next_free_z, p.footprint.z1 + 2);
        }
        for (const auto &[index, cell] : p.new_ports) {
          child.port[index] = cell;
          child.source[index] = cell;
        }
        for (std::size_t o = 0; o < group.nodes.size(); ++o)
          child.source[group.nodes[o]] = p.outputs[o];
        next.push_back(std::move(child));
        ++children;
      }
      if (cancelled())
        return std::nullopt;
    } // each partial circuit of the beam
    if (next.empty()) {
      say(std::format("flush: no place for gate {} ({})", gi + 1, names));
      return std::nullopt;
    }
    std::stable_sort(next.begin(), next.end(),
                     [](const FlushState &a, const FlushState &b) {
                       return area_of(a) < area_of(b);
                     });
    if (static_cast<int>(next.size()) > beam_width)
      next.resize(beam_width);
    beam = std::move(next);
  }

  // The finished circuit: the smallest of the beam.
  const FlushState &best = beam.front();
  const Cells &placed = best.placed;
  const std::vector<std::optional<Vec3>> &source = best.source;
  const std::vector<std::optional<Vec3>> &port = best.port;

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
  Spec spec =
      make_spec(size, inputs, outputs, rows, options.horizon, options.palette);
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

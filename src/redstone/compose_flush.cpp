// Flush composition: gate modules are placed one at a time, in signal order,
// right against the circuit built so far and shifted so one input sits on
// the cell its signal leaves from (or a little off it). The wires are drawn
// by the path-finding router (wire_router) in milliseconds; only where it
// finds none is a wire solved with SAT, in a narrow corridor with
// everything else pinned.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <future>
#include <limits>
#include <list>
#include <numeric>
#include <set>
#include <string>

#include "redstone/compose.hpp"
#include "redstone/compose_parts.hpp"
#include "redstone/simulator.hpp"
#include "redstone/verifier.hpp"
#include "redstone/wire_router.hpp"

namespace rs {

namespace {

using namespace compose_parts;

// Rows kept free north of the first modules (lower z) for wires.
// Generous: lining a module up may move it north; empty rows are cropped.
constexpr int kMarginZ = 16;
constexpr std::chrono::milliseconds kPollInterval{50};
constexpr int kSettleTicks = 400;
// Room the router may use beyond everything placed so far.
constexpr int kRouteSlack = 3;
// Layers above a module's wires that a crossing needs.
constexpr int kCrossLayers = 2;
// Rows a new input port may sit away from the gate input it feeds.
constexpr int kPortShift = 6;
// With the router: spots this far off a signal's line, and how many of
// them are tried per gate (best first).
constexpr int kRoutedGap = 3;
constexpr int kRoutedShift = 2;
// Columns east of a gate's signals tried for spots between them.
constexpr int kBetweenGap = 6;
// Many: the best-ranked spots are the tightest, and with many module
// variants the first few may all leave a wire no room. Routing stops once
// the beam is full, so the rest cost only their ranking.
constexpr int kRoutedCandidates = 192;
// A shuffled ranking moves a spot's score by up to 1 / this of it.
constexpr long kNoiseShare = 3;
// Dead ends the router's search may go back from before SAT wires.
constexpr int kMaxBacktracks = 24;
// Rows a gate of new inputs may start below the free rows.
constexpr int kFreshRowShift = 6;
// A module fed from its north or south face: rows its input may sit off
// the signal's row (either way).
constexpr int kSideShift = 3;
// Cells past its source a signal still to be read keeps for its wire.
constexpr int kExitDepth = 2;

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
// module input (whose repeater faces `into`).
struct Route {
  Vec3 from;
  Vec3 to;
  int node = 0; // the signal it carries
  Dir into = Dir::East;
};

// Where a module's input k is fed from, relative to the module: the cell
// behind its repeater, and the way the repeater faces.
struct Feed {
  Vec3 cell;
  Dir into = Dir::East;
};

std::vector<Feed> feeds_of(const GateModule &module) {
  std::vector<Feed> feeds;
  for (Vec3 in : module.ins) {
    const Dir into = module_input_facing(module.layout.size(), in);
    feeds.push_back({in - offset(into), into});
  }
  return feeds;
}

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
  std::vector<Vec3> outputs;          // where the gate's signals leave
  std::vector<Dir> output_ways;       // and the way they go
  const GateModule *module = nullptr; // nothing for a join
};

// A partial circuit: every gate so far placed and wired.
struct FlushState {
  Cells placed;
  std::vector<Footprint> footprints;
  std::vector<std::optional<Vec3>> source; // where a signal leaves
  std::vector<Dir> leaves;                 // the way it goes from there
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

// The dust of a partial circuit by the signal it carries and the weakest it
// is when on, found by simulating it under every input vector so far. Wires
// may branch off any of it.
using SignalCells = std::map<int, std::vector<std::pair<Vec3, int>>>;
// Cells kept for one node's wire (the node), off limits to all others.
using ExitCells = std::map<std::array<int, 3>, int>;

// The first two cells a signal leaves its source through and the cells
// beside them, on every level: a wire beside the exit would join the
// signal's first dust, and one over it leaves no room to climb.
void add_exit(ExitCells &exits, Vec3 source, Dir way, int node, int height) {
  const Vec3 side = offset(rotate_clockwise(way));
  Vec3 along = source;
  for (int step = 1; step <= kExitDepth; ++step) {
    along = along + offset(way);
    for (Vec3 c : {along, along + side, along - side})
      for (int y = 0; y < height; ++y)
        exits.emplace(key({c.x, y, c.z}), node);
  }
}

SignalCells signal_cells(const FlushState &state, const Netlist &netlist,
                         int height) {
  const int n = netlist.inputs;
  std::vector<int> ports;
  for (int i = 0; i < n; ++i)
    if (state.port[i])
      ports.push_back(i);
  if (ports.empty() || state.placed.empty())
    return {};
  Vec3 size{1, height, 1};
  for (const auto &[cell, block] : state.placed) {
    size.x = std::max(size.x, cell[0] + 1);
    size.y = std::max(size.y, cell[1] + 1);
    size.z = std::max(size.z, cell[2] + 1);
  }
  World world(size);
  for (const auto &[cell, block] : state.placed)
    world.set({cell[0], cell[1], cell[2]}, block);
  for (std::size_t c = 0; c < ports.size(); ++c)
    world.set(
        *state.port[ports[c]],
        Block::input(static_cast<int>(c), InputKind::Repeater, Dir::East));

  std::vector<Vec3> dust;
  for (const auto &[cell, block] : state.placed)
    if (block.type == BlockType::Dust)
      dust.push_back({cell[0], cell[1], cell[2]});
  const int masks = 1 << ports.size();
  std::vector<std::vector<bool>> on(dust.size(), std::vector<bool>(masks));
  std::vector<int> weakest(dust.size(), kMaxSignal);
  Simulator sim(world);
  for (int mask = 0; mask < masks; ++mask) {
    for (std::size_t c = 0; c < ports.size(); ++c)
      sim.set_input(static_cast<int>(c),
                    ((mask >> c) & 1) != 0 ? kMaxSignal : 0);
    if (!sim.run_until_quiet(kSettleTicks))
      return {};
    for (std::size_t d = 0; d < dust.size(); ++d) {
      int power = sim.state_at(dust[d]).power;
      on[d][mask] = power > 0;
      if (power > 0)
        weakest[d] = std::min(weakest[d], power);
    }
  }
  std::map<std::vector<bool>, std::vector<int>> nodes_by_pattern;
  for (int node = 0; node < static_cast<int>(state.source.size()); ++node) {
    if (!state.source[node])
      continue;
    std::vector<bool> pattern(masks);
    for (int mask = 0; mask < masks; ++mask) {
      int vector = 0;
      for (std::size_t c = 0; c < ports.size(); ++c)
        if ((mask >> c) & 1)
          vector |= 1 << ports[c];
      pattern[mask] = netlist.evaluate(vector)[node];
    }
    nodes_by_pattern[pattern].push_back(node);
  }
  SignalCells result;
  for (std::size_t d = 0; d < dust.size(); ++d) {
    auto it = nodes_by_pattern.find(on[d]);
    if (it == nodes_by_pattern.end())
      continue;
    for (int node : it->second)
      result[node].push_back({dust[d], weakest[d]});
  }
  return result;
}

// A quick check of a routed stage: every input row once, one input flipped
// at a time (Gray code order), each settling to its outputs. The finished
// circuit gets the full check (every switch between rows) at the end.
std::optional<std::string> walk_rows(const World &layout,
                                     const std::vector<StageRow> &rows) {
  Simulator sim(layout);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::size_t mask = i ^ (i >> 1);
    const StageRow &row = rows[mask];
    for (std::size_t c = 0; c < row.inputs.size(); ++c)
      sim.set_input(static_cast<int>(c), row.inputs[c] ? kMaxSignal : 0);
    if (!sim.run_until_quiet(kSettleTicks))
      return std::format("row {} does not settle", mask);
    for (std::size_t o = 0; o < row.outputs.size(); ++o)
      if (row.outputs[o] &&
          (sim.output(static_cast<int>(o)) > 0) != *row.outputs[o])
        return std::format("row {}: output {} is wrong", mask, o + 1);
  }
  return std::nullopt;
}

// The cells placed so far as a world from their first row on.
World world_of(const Cells &placed, int height) {
  int min_z = std::numeric_limits<int>::max();
  Vec3 size{1, height, 1};
  for (const auto &[cell, block] : placed) {
    min_z = std::min(min_z, cell[2]);
    size.x = std::max(size.x, cell[0] + 1);
    size.y = std::max(size.y, cell[1] + 1);
  }
  if (placed.empty())
    return World(size);
  for (const auto &[cell, block] : placed)
    size.z = std::max(size.z, cell[2] - min_z + 1);
  World world(size);
  for (const auto &[cell, block] : placed)
    world.set({cell[0], cell[1], cell[2] - min_z}, block);
  return world;
}

constexpr std::uint8_t kOr = 0b1110;
constexpr std::uint8_t kXor = 0b0110;

} // namespace

std::optional<ComposeResult> compose_flush(const Netlist &netlist,
                                           const BoolTable &table,
                                           const NetlistComposeOptions &options,
                                           const ProgressCallback &progress) {
  return compose_parts::compose_flush_groups(
      netlist, table, compose_parts::group_gates(netlist, netlist.levels()),
      options, progress);
}

std::optional<ComposeResult> compose_parts::compose_flush_groups(
    const Netlist &netlist, const BoolTable &table,
    const std::vector<Group> &groups, const NetlistComposeOptions &options,
    const ProgressCallback &progress) {
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  const int n = netlist.inputs;
  int module_height = 2;
  std::optional<std::map<std::vector<std::uint8_t>, GateModule>> modules =
      build_modules(groups, options, module_height, progress);
  if (!modules) {
    say("flush: no module for one of the gate groups");
    return std::nullopt;
  }
  auto allowed = [&](BlockType type) {
    return !options.palette ||
           std::find(options.palette->begin(), options.palette->end(), type) !=
               options.palette->end();
  };
  const bool use_router = options.flush_router && allowed(BlockType::Dust) &&
                          allowed(BlockType::Solid);
  // The router crosses a wire two layers above it, and climbs only where
  // it must: it may use the whole height allowed.
  int extra_layers = use_router ? std::max(options.extra_layers, kCrossLayers)
                                : options.extra_layers;
  if (options.max_height > 0)
    extra_layers = std::max(0, options.max_height - module_height);
  if (!use_router && options.max_height > 0)
    extra_layers = std::min(extra_layers, options.extra_layers);
  const int height = module_height + extra_layers;

  // A signal is kept correct until its last reader is placed.
  const int nodes = n + static_cast<int>(netlist.gates.size());
  const int forever = static_cast<int>(groups.size());
  std::vector<int> last_use(nodes, -1);
  for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi)
    for (int operand : groups[gi].operands)
      last_use[operand] = std::max(last_use[operand], gi);
  for (int out : netlist.outputs)
    last_use[out] = forever;

  // A beam of the best partial circuits: each gate is placed in every one of
  // them, and the smallest few results go on to the next gate.
  std::vector<FlushState> beam(1);
  beam[0].source.resize(nodes);
  beam[0].leaves.resize(nodes, Dir::East);
  beam[0].port.resize(n);
  beam[0].next_free_z = kMarginZ;
  const int beam_width = std::max(1, options.flush_beam);
  // Rows beside a wire's straight path it may also use (to go around).
  const int margin = std::max(1, options.flush_margin);

  // A stage's wires drawn by the router, then checked like a solved stage:
  // the cells of the whole partial circuit, or nothing.
  auto route_stage =
      [&](const Cells &base, const std::vector<Footprint> &footprints,
          const Spot &p, const SignalCells &signals, Vec3 size,
          const std::vector<Vec3> &input_cells,
          const std::vector<Vec3> &output_cells,
          const std::vector<StageRow> &rows, const ExitCells &exits,
          std::string &why) -> std::optional<Cells> {
    Cells cells = base;
    int routing = -1; // the node whose wire is being drawn
    auto blocked = [&](Vec3 c) {
      if (!p.join && inside(p.footprint, c))
        return true;
      if (auto it = exits.find(key(c));
          it != exits.end() && it->second != routing)
        return true;
      return std::any_of(footprints.begin(), footprints.end(),
                         [&](const Footprint &f) { return inside(f, c); });
    };
    // New input ports: a repeater facing east on a block.
    for (const auto &[index, cell] : p.new_ports) {
      cells[key(cell)] = Block::repeater(Dir::East);
      cells[key(cell - Vec3{0, 1, 0})] = Block::solid();
    }
    // Every signal's dust and its strength: wires may cross other wires
    // on the level below by rebuilding them.
    std::map<std::array<int, 3>, int> strengths;
    for (const auto &[node, dust] : signals)
      for (const auto &[cell, strength] : dust)
        strengths[key(cell)] = strength;
    // A joined-wire OR: each signal leaves through a repeater (strength 1
    // at the start allows nothing else), so neither flows back into the
    // other's wire; the first wire ends in dust at the meeting cell, the
    // second joins it.
    bool first_join = true;
    for (const Route &r : p.routes) {
      routing = r.node;
      std::vector<WireStart> starts;
      std::set<std::array<int, 3>> own;
      bool new_port = false;
      for (const auto &[index, cell] : p.new_ports)
        new_port |= cell == r.from;
      if (r.from.x == 0 && !p.join)
        starts.push_back({r.from, kMaxSignal, Dir::East});
      if (!new_port)
        if (auto it = signals.find(r.node); it != signals.end())
          for (const auto &[cell, strength] : it->second) {
            starts.push_back({cell, p.join ? 1 : strength, std::nullopt});
            own.insert(key(cell));
          }
      if (starts.empty()) {
        why = "no dust carries the signal to start from";
        return std::nullopt;
      }
      WireRouterOptions o;
      o.low = {1, 1, 0};
      o.high = {0, height - 1, 0};
      for (const auto &[cell, block] : cells) {
        o.high.x = std::max(o.high.x, cell[0] + kRouteSlack);
        o.high.z = std::max(o.high.z, cell[2] + kRouteSlack);
      }
      o.repeaters = allowed(BlockType::Repeater);
      WireRouter router(cells, blocked, o);
      WireGoal goal{r.to, r.into};
      if (p.join) {
        goal.kind = first_join ? WireGoal::Kind::Dust : WireGoal::Kind::Join;
        first_join = false;
      }
      auto wire = router.route(starts, own, goal, strengths);
      if (!wire) {
        why = std::format("no path from x {} z {} to x {} z {}", r.from.x,
                          r.from.z, r.to.x, r.to.z);
        return std::nullopt;
      }
      for (const auto &[c, block] : *wire)
        cells[key(c)] = block;
    }

    for (const auto &[cell, block] : cells) {
      size.x = std::max(size.x, cell[0] + 1);
      size.y = std::max(size.y, cell[1] + 1);
      size.z = std::max(size.z, cell[2] + 1);
    }
    Spec spec = make_spec(size, input_cells, output_cells, rows,
                          options.horizon, options.palette);
    World layout = spec.base_world();
    for (const auto &[cell, block] : cells) {
      Vec3 c{cell[0], cell[1], cell[2]};
      BlockType here = layout.at(c).type;
      if (here != BlockType::Input && here != BlockType::Output)
        layout.set(c, block);
    }
    if (std::vector<std::string> problems = layout.validate();
        !problems.empty()) {
      why = "the wires break a placement rule: " + problems.front();
      return std::nullopt;
    }
    if (std::optional<std::string> failure = walk_rows(layout, rows)) {
      why = "the wires failed the check: " + *failure;
      return std::nullopt;
    }
    return cells;
  };

  // Partial circuits past the beam's width, per gate: where a dead end
  // goes back to before any wire is left to SAT.
  std::vector<std::vector<FlushState>> spare(groups.size());
  int backtracks = 0;

  for (int gi = 0; gi < static_cast<int>(groups.size());) {
    if (cancelled())
      return std::nullopt;
    const Group &group = groups[gi];
    const int arity = static_cast<int>(group.operands.size());
    // Modules for this gate with its operands in some order: the one built
    // for it and, from the catalog, the same gate in other shapes and port
    // rows (another order of the operands is another truth table). Each is
    // tried at every spot.
    struct Variant {
      const GateModule *module = nullptr;
      std::vector<int> ops; // operand node of each module input
    };
    std::list<std::vector<GateModule>> catalog_modules;
    std::vector<Variant> variants;
    std::vector<int> order(arity);
    std::iota(order.begin(), order.end(), 0);
    do {
      std::vector<std::uint8_t> functions;
      for (std::uint8_t f : group.functions)
        functions.push_back(permute_function(f, arity, order));
      std::vector<int> ops;
      for (int k : order)
        ops.push_back(group.operands[k]);
      if (functions == group.functions && modules->contains(functions) &&
          arity == 2)
        variants.push_back({&modules->at(functions), ops});
      // Modules with ports on the north or south face need the router.
      if (options.catalog)
        for (const GateModule &m :
             catalog_modules.emplace_back(options.catalog->variants(
                 catalog_key(options.palette, options.solver.verification),
                 arity, functions, height)))
          if (use_router ||
              (std::all_of(m.ins.begin(), m.ins.end(),
                           [](Vec3 in) { return in.x == 0; }) &&
               std::all_of(m.outs.begin(), m.outs.end(), [&](Vec3 out) {
                 return out.x == m.layout.size().x - 1;
               })))
            variants.push_back({&m, ops});
    } while (std::next_permutation(order.begin(), order.end()));
    const std::string names = group_name(group);
    if (variants.empty()) {
      say(std::format("flush: no module for gate {} ({})", gi + 1, names));
      return std::nullopt;
    }

    const bool can_backtrack =
        use_router && backtracks < kMaxBacktracks &&
        std::any_of(
            spare.begin(), spare.begin() + gi,
            [](const std::vector<FlushState> &s) { return !s.empty(); });

    std::vector<FlushState> next;
    for (const FlushState &state : beam) {
      const SignalCells signals =
          use_router ? signal_cells(state, netlist, height) : SignalCells{};
      // A signal still to be read keeps its way out: a wire drawn past it
      // walls it in for good (the counting circuit's gate 5).
      ExitCells exits;
      if (use_router)
        for (int node = 0; node < nodes; ++node)
          if (state.source[node] && last_use[node] >= gi)
            add_exit(exits, *state.source[node], state.leaves[node], node,
                     height);
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
        const Variant *variant = nullptr;
      };
      std::vector<Choice> spots;
      for (const Variant &variant : variants) {
        std::size_t first_spot = spots.size();
        const std::vector<Feed> feeds = feeds_of(*variant.module);
        for (int k = 0; k < arity; ++k) {
          const int op = variant.ops[k];
          if (!source[op])
            continue;
          Vec3 s = *source[op];
          if (state.leaves[op] != Dir::East) {
            // The signal leaves north or south: around the cell past it.
            const Vec3 exit = s + offset(state.leaves[op]);
            for (int dx = -kSideShift; dx <= kSideShift; ++dx)
              for (int dz = -kSideShift; dz <= kSideShift; ++dz)
                spots.push_back({exit.x - feeds[k].cell.x + dx,
                                 exit.z - feeds[k].cell.z + dz, &variant});
            continue;
          }
          if (feeds[k].into != Dir::East) {
            // Fed from the side: east of the signal, the input a few rows
            // off its row.
            for (int gap = 0; gap <= kRoutedGap; ++gap)
              for (int dz = -kSideShift; dz <= kSideShift; ++dz)
                spots.push_back(
                    {s.x + 1 + gap, s.z - feeds[k].cell.z + dz, &variant});
            continue;
          }
          const int input_z = feeds[k].cell.z;
          spots.push_back({s.x + 1, s.z - input_z, &variant});
          for (int gap = 1; gap <= 1 + options.flush_spacing; ++gap)
            spots.push_back({s.x + 1 + gap, s.z - input_z, &variant});
          // Routed wires are cheap: also try spots a little off the line.
          if (use_router)
            for (int gap = 0; gap <= kRoutedGap; ++gap)
              for (int dz = -kRoutedShift; dz <= kRoutedShift; ++dz)
                if (dz != 0)
                  spots.push_back(
                      {s.x + 1 + gap, s.z - input_z + dz, &variant});
        }
        // A gate of new inputs only: start a fresh row, or a few further off
        // when the ports already there leave its inputs no room.
        if (spots.size() == first_spot)
          for (int dz = 0; dz <= (use_router ? kFreshRowShift : 0); ++dz)
            for (int x0 = 1; x0 <= 2 + options.flush_spacing; ++x0)
              spots.push_back({x0, next_free_z + dz, &variant});
        // Signals far apart: spots near one leave the other's wire no room
        // to cross what lies between, so also every row between them, a
        // little further east.
        if (use_router && arity >= 2) {
          int min_z = std::numeric_limits<int>::max();
          int max_z = std::numeric_limits<int>::min();
          int max_x = 0;
          int sourced = 0;
          for (int op : variant.ops)
            if (source[op]) {
              ++sourced;
              min_z = std::min(min_z, source[op]->z);
              max_z = std::max(max_z, source[op]->z);
              max_x = std::max(max_x, source[op]->x);
            }
          const int msize_z = variant.module->layout.size().z;
          if (sourced >= 2 && max_z - min_z > kRoutedShift)
            for (int x0 = max_x + 1; x0 <= max_x + 1 + kBetweenGap; ++x0)
              for (int z = min_z - msize_z + 1; z <= max_z; ++z)
                spots.push_back({x0, z, &variant});
        }
      } // each module variant

      std::vector<Spot> candidates;
      for (auto [x0, z, variant] : spots) {
        if (z < 0 || x0 < 1)
          continue;
        const GateModule &m = *variant->module;
        const Vec3 msize = m.layout.size();
        const std::vector<Feed> feeds = feeds_of(m);
        std::vector<Vec3> targets;
        for (const Feed &feed : feeds)
          targets.push_back(Vec3{x0, 0, z} + feed.cell);
        const std::vector<int> &ops = variant->ops;
        Spot p{x0, z, {x0, x0 + msize.x - 1, z, z + msize.z - 1}, {}, {}, 0};
        p.module = &m;
        bool ok = std::none_of(
            footprints.begin(), footprints.end(),
            [&](const Footprint &f) { return clashes(f, p.footprint); });
        for (int x = p.footprint.x0; ok && x <= p.footprint.x1; ++x)
          for (int y = 0; ok && y < height; ++y)
            for (int zz = p.footprint.z0; ok && zz <= p.footprint.z1; ++zz)
              ok = !placed.contains(key({x, y, zz}));
        long route_length = 0;
        for (int k = 0; ok && k < arity; ++k) {
          const Vec3 target = targets[k];
          Vec3 from;
          if (source[ops[k]]) {
            from = *source[ops[k]];
          } else {
            // A new input port on the west side, level with its gate input
            // or (with the router drawing the wire) a few rows off it.
            // Wires leaving neighbouring ports need a row between them, and
            // one more for a wire that other gates still branch off.
            auto free_port = [&](Vec3 cell) {
              if (cell.z < 0 || occupied(cell))
                return false;
              // Its wire starts east of it: not where another one ends.
              const Vec3 first = cell + Vec3{1, 0, 0};
              for (int j = 0; j < arity; ++j)
                if (j != k && first == targets[j] && first != target)
                  return false;
              auto crowds = [&](int input, Vec3 other) {
                int need =
                    last_use[input] > gi || last_use[ops[k]] > gi ? 2 : 1;
                return std::abs(other.z - cell.z) <= need;
              };
              for (int i = 0; i < n; ++i)
                if (port[i] && crowds(i, *port[i]))
                  return false;
              for (const auto &[index, other] : p.new_ports)
                if (crowds(index, other))
                  return false;
              return true;
            };
            std::optional<Vec3> chosen;
            const int reach = use_router ? kPortShift : 0;
            // Away from the module's other inputs first, so the wires do
            // not cross at the module.
            const bool lowest =
                std::all_of(targets.begin(), targets.end(),
                            [&](Vec3 other) { return target.z <= other.z; });
            const int away = lowest ? -1 : 1;
            for (int sign : {away, -away})
              for (int shift = 0; shift <= reach && !chosen; ++shift) {
                Vec3 cell{0, 1, target.z + sign * shift};
                if (free_port(cell))
                  chosen = cell;
              }
            if (!chosen) {
              ok = false;
              continue;
            }
            from = *chosen;
            p.new_ports.push_back({ops[k], from});
          }
          // A port right against its first gate leaves no wire for its
          // other readers to branch off.
          if (from == target && !source[ops[k]] && last_use[ops[k]] > gi) {
            ok = false;
            continue;
          }
          if (from == target)
            continue; // lined up: no wire
          if (occupied(target) || target.x < 1) {
            ok = false;
            continue;
          }
          p.routes.push_back({from, target, ops[k], feeds[k].into});
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
        for (Vec3 out : m.outs) {
          p.outputs.push_back({x0 + out.x, 1, z + out.z});
          p.output_ways.push_back(module_output_facing(msize, out));
        }
        candidates.push_back(std::move(p));
      }
      // Variants of the build rank spots with a little noise.
      if (options.flush_seed != 0)
        for (Spot &p : candidates) {
          std::uint64_t h = static_cast<std::uint64_t>(options.flush_seed);
          for (int v : {gi, p.x0, p.z, p.join ? 1 : 0})
            h = (h ^ static_cast<std::uint64_t>(v + 0x9e37)) * 0x100000001b3ULL;
          p.score += static_cast<long>(
              h % static_cast<std::uint64_t>(p.score / kNoiseShare + 1));
        }
      std::stable_sort(
          candidates.begin(), candidates.end(),
          [](const Spot &a, const Spot &b) { return a.score < b.score; });
      const int kept =
          use_router ? std::max(options.flush_candidates, kRoutedCandidates)
                     : options.flush_candidates;
      if (static_cast<int>(candidates.size()) > kept)
        candidates.resize(kept);

      // An OR of on/off signals is two dust lines meeting (dust carries the
      // stronger signal); so is an XOR whose inputs are never on together.
      // Tried first: it needs no module at all.
      if (options.flush_joins && arity == 2 && group.functions.size() == 1 &&
          source[group.operands[0]] && source[group.operands[1]]) {
        const int ga = group.operands[0];
        const int gb = group.operands[1];
        const std::uint8_t f = group.functions[0];
        bool joinable = f == kOr;
        if (f == kXor) {
          joinable = true;
          for (int t = 0; t < (1 << n) && joinable; ++t) {
            std::vector<bool> v = netlist.evaluate(t);
            joinable = !(v[ga] && v[gb]);
          }
        }
        const Vec3 sa = *source[ga];
        const Vec3 sb = *source[gb];
        std::vector<Spot> joins;
        // Routed joins start with repeaters: they need room to meet.
        const int reach = use_router ? 3 : 1;
        for (int dx = 1; dx <= reach; ++dx)
          for (int z : {sa.z, sb.z}) {
            if (!joinable)
              break;
            const Vec3 meet{std::max(sa.x, sb.x) + dx, 1, z};
            if (occupied(meet))
              continue;
            Spot j;
            j.join = true;
            j.x0 = meet.x;
            j.z = meet.z;
            j.footprint = {meet.x, meet.x, meet.z, meet.z};
            for (auto [from, node] : {std::pair{sa, ga}, std::pair{sb, gb}})
              if (from != meet)
                j.routes.push_back({from, meet, node});
            j.outputs = {meet};
            j.output_ways = {Dir::East};
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
      std::vector<std::optional<Cells>> routed;
      std::vector<std::string> why_not_routed;
      int routed_count = 0;
      for (const Spot &p : candidates) {
        Cells cells = placed;
        if (!p.join)
          add_layout(cells, p.module->layout, {p.x0, 0, p.z});
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
        if (use_router && routed_count >= beam_width) {
          routed.push_back(std::nullopt);
          why_not_routed.emplace_back();
        } else if (use_router) {
          ExitCells stage_exits = exits;
          for (std::size_t o = 0; o < group.nodes.size(); ++o)
            add_exit(stage_exits, p.outputs[o], p.output_ways[o],
                     group.nodes[o], height);
          routed.push_back(route_stage(
              cells, footprints, p, signals, size, input_cells, output_cells,
              rows, stage_exits, why_not_routed.emplace_back()));
          routed_count += routed.back() ? 1 : 0;
        } else {
          routed.push_back(std::nullopt);
          why_not_routed.emplace_back();
        }
        Spec spec = make_spec(size, input_cells, output_cells, rows,
                              options.horizon, options.palette);
        // Input ports are repeaters: the floor under them may hold a support.
        pin(spec, cells, [&](Vec3 c) {
          return free_cells.contains(key(c)) || (c.y == 0 && c.x == 0);
        });
        stages.push_back(std::move(spec));
      }

      int children = 0;
      auto add_child = [&](const Spot &p, Cells cells) {
        FlushState child = state;
        child.placed = std::move(cells);
        if (!p.join) {
          child.footprints.push_back(p.footprint);
          child.next_free_z = std::max(child.next_free_z, p.footprint.z1 + 2);
        }
        for (const auto &[index, cell] : p.new_ports) {
          child.port[index] = cell;
          child.source[index] = cell;
          child.leaves[index] = Dir::East;
        }
        for (std::size_t o = 0; o < group.nodes.size(); ++o) {
          child.source[group.nodes[o]] = p.outputs[o];
          child.leaves[group.nodes[o]] = p.output_ways[o];
        }
        next.push_back(std::move(child));
        ++children;
      };

      // Routed positions first: they cost nothing more to take.
      for (std::size_t i = 0; i < routed.size() && children < beam_width; ++i)
        if (routed[i]) {
          const Spot &p = candidates[i];
          say(std::format("gate {} ({}) at x {} z {}: routed", gi + 1, names,
                          p.x0, p.z));
          add_child(p, std::move(*routed[i]));
        }

      // SAT wiring only when nothing routed. One position at a time,
      // best-ranked first: solving them side by side made every one run out
      // of time (the multiplexer's third gate).
      const bool any_routed = children > 0;
      if (use_router && !any_routed) {
        // How the spots failed: the first of each kind, and how often.
        std::map<std::string, std::pair<int, std::size_t>> kinds;
        int side_spots = 0;
        for (std::size_t i = 0; i < routed.size(); ++i) {
          const Spot &p = candidates[i];
          if (p.module &&
              (std::any_of(p.module->ins.begin(), p.module->ins.end(),
                           [](Vec3 in) { return in.x != 0; }) ||
               std::any_of(p.output_ways.begin(), p.output_ways.end(),
                           [](Dir d) { return d != Dir::East; })))
            ++side_spots;
          if (why_not_routed[i].empty())
            continue;
          const std::string &why = why_not_routed[i];
          auto &[count, first] =
              kinds
                  .try_emplace(why.substr(0, why.find_first_of(":0123456789")),
                               0, i)
                  .first->second;
          ++count;
        }
        std::string summary;
        for (const auto &[kind, seen] : kinds)
          summary +=
              std::format("; {} x{} (first at x {} z {}: {})", kind, seen.first,
                          candidates[seen.second].x0, candidates[seen.second].z,
                          why_not_routed[seen.second]);
        say(std::format("router: gate {}: none of {} spots wired ({} with "
                        "side ports){}",
                        gi + 1, routed.size(), side_spots, summary));
      }
      // The router looks at many spots; SAT takes seconds per spot, so only
      // the best few: all of them kept one gate busy for a whole run.
      int sat_tries = 0;
      for (std::size_t i = 0;
           i < stages.size() && options.flush_sat && !any_routed &&
           !can_backtrack && children < beam_width &&
           sat_tries < options.flush_candidates;
           ++i, ++sat_tries) {
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
        Cells cells;
        add_layout(cells, report.solution->layout, {0, 0, 0});
        add_child(p, std::move(cells));
      }
      if (cancelled())
        return std::nullopt;
    } // each partial circuit of the beam
    if (next.empty() && can_backtrack) {
      int back = gi - 1;
      while (spare[back].empty())
        --back;
      ++backtracks;
      say(std::format("flush: no place for gate {}; back to gate {}", gi + 1,
                      back + 1));
      std::vector<FlushState> &pool = spare[back];
      const auto take = std::min<std::ptrdiff_t>(beam_width, pool.size());
      beam.assign(std::make_move_iterator(pool.begin()),
                  std::make_move_iterator(pool.begin() + take));
      pool.erase(pool.begin(), pool.begin() + take);
      for (std::size_t later = back + 1; later < spare.size(); ++later)
        spare[later].clear();
      gi = back + 1;
      continue;
    }
    if (next.empty()) {
      say(std::format("flush: no place for gate {} ({})", gi + 1, names));
      return std::nullopt;
    }
    std::stable_sort(next.begin(), next.end(),
                     [](const FlushState &a, const FlushState &b) {
                       return area_of(a) < area_of(b);
                     });
    if (static_cast<int>(next.size()) > beam_width) {
      spare[gi].assign(std::make_move_iterator(next.begin() + beam_width),
                       std::make_move_iterator(next.end()));
      next.resize(beam_width);
    }
    beam = std::move(next);
    ++gi;
    if (options.preview)
      options.preview(world_of(beam.front().placed, height));
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

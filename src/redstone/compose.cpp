#include "redstone/compose.hpp"

#include "redstone/compose_parts.hpp"
#include "redstone/simulator.hpp"
#include "redstone/size_search.hpp"
#include "redstone/verifier.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

namespace rs {

namespace compose_parts {

Spec make_spec(Vec3 size, const std::vector<Vec3> &inputs,
               const std::vector<Vec3> &outputs,
               const std::vector<StageRow> &rows, int horizon,
               const std::optional<std::vector<BlockType>> &palette) {
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
  Spec spec = parse_spec(text);
  if (palette)
    spec.palette = *palette;
  return spec;
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
      return x.operands == std::vector<int>{gate.a, gate.b} &&
             level[x.nodes[0]] == level[node];
    });
    if (same == groups.end()) {
      groups.push_back({{gate.a, gate.b}, {}, {}});
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
  gate_options.palette = options.palette;
  gate_options.solver = options.solver;
  gate_options.cancel = options.cancel;
  std::map<std::vector<std::uint8_t>, GateModule> modules;
  module_height = 2;
  for (const Group &group : groups) {
    if (group.operands.size() != 2 || modules.contains(group.functions))
      continue;
    const std::string cache_key = gate_cache_key(group.functions, gate_options);
    std::optional<GateModule> m;
    if (options.gate_cache)
      m = options.gate_cache->find(cache_key);
    if (!m) {
      m = build_gate(group.functions, gate_options, progress);
      if (!m)
        return std::nullopt;
      if (options.gate_cache)
        options.gate_cache->store(cache_key, *m);
    }
    module_height = std::max(module_height, m->layout.size().y);
    modules.emplace(group.functions, std::move(*m));
  }
  return modules;
}

} // namespace compose_parts

namespace {

using namespace compose_parts;

std::vector<BlockType> core_redstone_palette() {
  return {BlockType::Solid, BlockType::Dust, BlockType::Torch,
          BlockType::Repeater};
}

bool is_exact_one_of_three(const BoolTable &table) {
  if (table.inputs != 3 || table.values.size() != 1 ||
      table.values[0].size() != 8)
    return false;
  for (int vector = 0; vector < 8; ++vector) {
    const int set_bits = std::popcount(static_cast<unsigned>(vector));
    if (table.values[0][vector] != std::optional<bool>(set_bits == 1))
      return false;
  }
  return true;
}

long long network_layout_score(const Netlist &netlist) {
  const std::vector<int> levels = netlist.levels();
  int depth = 0;
  for (int node = netlist.inputs; node < static_cast<int>(levels.size());
       ++node)
    depth = std::max(depth, levels[node]);

  const std::vector<Group> groups = group_gates(netlist, levels);
  long long score = static_cast<long long>(groups.size()) * 1000 + depth * 200;
  std::vector<int> last_use(levels.size(), -1);
  for (std::size_t g = 0; g < netlist.gates.size(); ++g) {
    int user_level = levels[netlist.inputs + static_cast<int>(g)];
    const Gate &gate = netlist.gates[g];
    last_use[gate.a] = std::max(last_use[gate.a], user_level);
    last_use[gate.b] = std::max(last_use[gate.b], user_level);
  }
  for (int output : netlist.outputs)
    last_use[output] = depth + 1;

  // A live signal crosses a Compose column. Fewer live signals usually mean
  // fewer pass-through tracks and fewer opportunities for wiring conflicts.
  for (int boundary = 0; boundary < depth; ++boundary) {
    int live = 0;
    for (int node = 0; node < static_cast<int>(levels.size()); ++node)
      live += levels[node] <= boundary && last_use[node] > boundary;
    score += live * 100 + live * live * 25;
  }
  return score;
}

// Smaller floor area first, then fewer layers, then fewer blocks.
bool smaller(const ComposeResult &a, const ComposeResult &b) {
  Vec3 sa = a.layout.size();
  Vec3 sb = b.layout.size();
  if (sa.x * sa.z != sb.x * sb.z)
    return sa.x * sa.z < sb.x * sb.z;
  if (sa.y != sb.y)
    return sa.y < sb.y;
  return a.spec.hints.size() < b.spec.hints.size();
}

// Fused groupings tried (fewest modules first) and builds of each.
constexpr int kMaxFusedGroupings = 6;
constexpr int kFusedSeeds = 4;

// The plain flush build and, when the router draws the wires (each build
// takes moments), more with shuffled spot rankings side by side, and builds
// where catalog modules of three inputs stand in for several gates; the
// smallest is kept. The first build fills the module cache for the rest.
std::optional<ComposeResult>
flush_variants(const Netlist &netlist, const BoolTable &table,
               const NetlistComposeOptions &options,
               const ProgressCallback &progress) {
  GateCache local_cache;
  NetlistComposeOptions first = options;
  if (!first.gate_cache)
    first.gate_cache = &local_cache;
  std::vector<std::vector<Group>> fused;
  if (options.catalog && options.flush_router) {
    const std::string key =
        catalog_key(options.palette, options.solver.verification);
    auto has_module = [&](const std::vector<std::uint8_t> &functions) {
      std::vector<int> order{0, 1, 2};
      do {
        std::vector<std::uint8_t> permuted;
        for (std::uint8_t f : functions)
          permuted.push_back(permute_function(f, 3, order));
        if (options.catalog->has_any(key, 3, permuted))
          return true;
      } while (std::next_permutation(order.begin(), order.end()));
      return false;
    };
    fused = fused_groupings(netlist, has_module, kMaxFusedGroupings);
    if (progress)
      for (const std::vector<Group> &grouping : fused) {
        std::string pieces;
        for (const Group &group : grouping)
          if (group.operands.size() > 2)
            pieces += (pieces.empty() ? "" : ", ") + group_name(group);
        progress(std::format("flush: also building with {} ({} modules)",
                             pieces, grouping.size()));
      }
  }
  std::optional<ComposeResult> best =
      compose_flush(netlist, table, first, progress);
  if (!options.flush_router || (!best && fused.empty()) ||
      (options.flush_variants <= 1 && fused.empty()))
    return best;
  std::vector<std::future<std::optional<ComposeResult>>> variants;
  for (int seed = 1; best && seed < options.flush_variants; ++seed) {
    NetlistComposeOptions o = first;
    o.flush_seed = seed;
    o.flush_sat = false; // variants are quick or nothing
    o.preview = {};      // the first build was shown
    variants.push_back(std::async(std::launch::async, [&netlist, &table, o] {
      return compose_flush(netlist, table, o, {});
    }));
  }
  const std::size_t plain_builds = variants.size();
  // The last word of each grouping's first build: why it found no place.
  std::vector<std::string> fused_last(fused.size());
  for (std::size_t g = 0; g < fused.size(); ++g)
    for (int seed = 0; seed < kFusedSeeds; ++seed) {
      NetlistComposeOptions o = first;
      o.flush_seed = seed;
      o.flush_sat = false;
      o.preview = {};
      ProgressCallback note;
      // With the router's last complaint, which says what blocked it.
      if (seed == 0)
        note = [&last = fused_last[g],
                router = std::string()](std::string_view m) mutable {
          if (m.starts_with("router: "))
            router = m;
          last = router.empty() ? std::string(m)
                                : std::format("{} ({})", m, router);
        };
      variants.push_back(
          std::async(std::launch::async, [&netlist, &table,
                                          &grouping = fused[g], o, note] {
            return compose_flush_groups(netlist, table, grouping, o, note);
          }));
    }
  const bool timed = options.timing.earliest || options.timing.latest;
  auto meets_timing = [&](const ComposeResult &r) {
    if (!timed)
      return true;
    Spec spec = r.spec;
    spec.timing = options.timing;
    return !verify_layout(r.layout, spec, spec.verification_vectors(),
                          options.solver.verification);
  };
  bool best_meets = best && meets_timing(*best);
  bool best_fused = false;
  std::vector<std::optional<Vec3>> fused_best(fused.size());
  for (std::size_t v = 0; v < variants.size(); ++v) {
    std::optional<ComposeResult> result = variants[v].get();
    if (v >= plain_builds && result) {
      std::optional<Vec3> &kept = fused_best[(v - plain_builds) / kFusedSeeds];
      const Vec3 size = result->layout.size();
      if (!kept || size.x * size.z < kept->x * kept->z)
        kept = size;
    }
    if (!result)
      continue;
    bool meets = meets_timing(*result);
    if (!best || (meets && !best_meets) ||
        (meets == best_meets && smaller(*result, *best))) {
      best = std::move(result);
      best_meets = meets;
      best_fused = v >= plain_builds;
    }
  }
  if (progress)
    for (std::size_t g = 0; g < fused.size(); ++g)
      progress(fused_best[g]
                   ? std::format("flush: with 3-input modules ({}): {}x{}x{}",
                                 g + 1, fused_best[g]->x, fused_best[g]->y,
                                 fused_best[g]->z)
                   : std::format("flush: with 3-input modules ({}): {}", g + 1,
                                 fused_last[g]));
  if (progress && best) {
    Vec3 size = best->layout.size();
    progress(std::format(
        "flush: {} builds; the {} is {}x{}x{}{}", variants.size() + 1,
        timed && best_meets ? "smallest in time" : "smallest", size.x, size.y,
        size.z, best_fused ? " (with 3-input modules)" : ""));
  }
  return best;
}

std::optional<ComposeResult>
compose_with_palette(const Netlist &netlist, const BoolTable &table,
                     const NetlistComposeOptions &options,
                     const ProgressCallback &progress) {
  if (options.flush) {
    if (std::optional<ComposeResult> flush =
            flush_variants(netlist, table, options, progress))
      return flush;
    if (options.cancel && options.cancel->load())
      return std::nullopt;
    if (!options.columns) {
      if (progress)
        progress("flush placement failed; columns come in a later pass");
      return std::nullopt;
    }
    if (progress)
      progress("flush placement failed; laying out in columns");
  }
  return compose_netlist(netlist, table, options, progress);
}

ShrinkResult shrink_layout_of(const ComposeResult &start,
                              const ShrinkOptions &options,
                              const ProgressCallback &progress) {
  return shrink(start.spec, start.layout, options, progress);
}

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
        items.push_back(
            {(at[groups[gi].operands[0]] + at[groups[gi].operands[1]]) / 2,
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
    const auto channel_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(options.stage_seconds));
    int widths_left = options.max_channel_width - options.channel_width + 1;
    for (int width = options.channel_width;
         width <= options.max_channel_width && !done; ++width)
      for (bool upper : {extra_layers > 0}) {
        if (done)
          continue;
        if (cancelled())
          return std::nullopt;
        double remaining =
            std::chrono::duration<double>(channel_deadline -
                                          std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0)
          break;
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
            for (std::size_t input = 0; input < m.ins.size(); ++input) {
              probes.push_back({x0 - 1, 1, s.z + m.ins[input].z});
              probe_nodes.push_back(groups[s.group].operands[input]);
            }
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
        Spec spec = make_spec(size, input_ports, probes, rows, options.horizon,
                              options.palette);
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
        const double attempt_cap = width < options.comfortable_width
                                       ? options.narrow_seconds
                                       : options.stage_seconds;
        solver.time_limit_seconds =
            std::min(attempt_cap, remaining / std::max(1, widths_left));
        --widths_left;
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
          Spec result = make_spec(cropped, input_ports, probes, rows,
                                  options.horizon, options.palette);
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

std::optional<ComposeResult>
compose_and_shrink(const Spec &spec, const NetlistComposeOptions &options,
                   const ShrinkOptions &shrink,
                   const ProgressCallback &progress) {
  // Variants run side by side; their messages must not interleave.
  std::mutex progress_mutex;
  ProgressCallback say = [&](std::string_view message) {
    if (!progress)
      return;
    std::lock_guard lock(progress_mutex);
    progress(message);
  };

  // Each variant's first build, all at once; identical ones are shrunk once.
  const std::array<bool, 2> join_choices{true, false};
  std::vector<std::future<std::optional<ComposeResult>>> builds;
  for (bool joins : join_choices) {
    NetlistComposeOptions o = options;
    o.flush_joins = joins;
    if (joins != join_choices.front())
      o.preview = {}; // one build is shown, not two taking turns
    builds.push_back(std::async(std::launch::async, [&spec, o, &say] {
      return compose_spec(spec, o, say);
    }));
  }
  std::vector<std::pair<std::string, ComposeResult>> starts;
  for (std::size_t v = 0; v < builds.size(); ++v) {
    std::optional<ComposeResult> built = builds[v].get();
    if (!built)
      continue;
    bool same = std::any_of(starts.begin(), starts.end(), [&](const auto &s) {
      return s.second.layout.size() == built->layout.size() &&
             s.second.spec.hints == built->spec.hints;
    });
    if (!same)
      starts.emplace_back(join_choices[v] ? "ORs as joined wires"
                                          : "ORs as modules",
                          std::move(*built));
  }
  if (options.cancel && options.cancel->load())
    return std::nullopt;
  if (starts.empty())
    return std::nullopt;

  auto volume = [](Vec3 s) { return s.x * s.y * s.z; };
  double seconds = shrink.time_limit_seconds;
  if (options.deadline)
    seconds =
        std::max(1.0, std::chrono::duration<double>(
                          *options.deadline - std::chrono::steady_clock::now())
                          .count());

  // The shrinks and the size search run at once: a circuit is shown when it
  // is no bigger than the last one shown, and the smallest so far bounds the
  // size search.
  std::mutex preview_mutex;
  int shown_cells = std::numeric_limits<int>::max();
  std::atomic<int> smallest = std::numeric_limits<int>::max();
  for (const auto &[name, start] : starts)
    smallest = std::min(smallest.load(), volume(start.layout.size()));
  if (options.beat)
    smallest = std::min(smallest.load(), volume(*options.beat));
  PreviewCallback preview = [&](const World &layout) {
    const int cells = volume(layout.size());
    std::lock_guard lock(preview_mutex);
    smallest = std::min(smallest.load(), cells);
    if (!shrink.preview || cells > shown_cells)
      return;
    shown_cells = cells;
    shrink.preview(layout);
  };

  const ComposeResult *beat = &starts.front().second;
  for (const auto &[name, start] : starts)
    if (volume(start.layout.size()) < volume(beat->layout.size()))
      beat = &start;
  const int all_workers = shrink_workers(shrink);
  const int size_workers = options.size_search ? all_workers / 2 : 0;
  std::future<std::optional<SizeSearchResult>> sizes;
  if (size_workers > 0) {
    SizeSearchOptions s;
    s.time_limit_seconds = seconds;
    s.parallel = size_workers;
    s.max_height = options.max_height;
    s.solver.verification = shrink.solver.verification;
    s.cancel = options.cancel;
    s.preview = preview;
    s.bound = &smallest;
    Vec3 to_beat = beat->layout.size();
    if (options.beat && volume(*options.beat) < volume(to_beat))
      to_beat = *options.beat;
    sizes = std::async(std::launch::async, [beat, to_beat, s, &say] {
      return search_sizes(beat->spec, to_beat, s, say);
    });
  }

  std::vector<std::future<ComposeResult>> shrinks;
  const int workers = std::max(1, (all_workers - size_workers) /
                                      static_cast<int>(starts.size()));
  for (auto &[name, start] : starts) {
    say(std::format("shrinking the {}x{}x{} build ({})", start.layout.size().x,
                    start.layout.size().y, start.layout.size().z, name));
    ShrinkOptions o = shrink;
    o.parallel = workers;
    o.cancel = options.cancel;
    o.preview = preview;
    o.time_limit_seconds = seconds;
    shrinks.push_back(std::async(std::launch::async, [&start = start,
                                                      &name = name, o, &say] {
      ShrinkResult shrunk = shrink_layout_of(start, o, say);
      Vec3 size = shrunk.layout.size();
      say(std::format("{}: {}x{}x{}", name, size.x, size.y, size.z));
      return ComposeResult{std::move(shrunk.spec), std::move(shrunk.layout)};
    }));
  }
  std::vector<ComposeResult> results;
  for (auto &job : shrinks)
    results.push_back(job.get());
  if (sizes.valid())
    if (std::optional<SizeSearchResult> found = sizes.get()) {
      Vec3 size = found->layout.size();
      say(std::format("size search: {}x{}x{}", size.x, size.y, size.z));
      results.push_back(
          ComposeResult{std::move(found->spec), std::move(found->layout)});
    }
  std::optional<ComposeResult> best;
  for (ComposeResult &result : results) {
    Vec3 size = result.layout.size();
    if (!best || volume(size) < volume(best->layout.size()) ||
        (volume(size) == volume(best->layout.size()) &&
         result.spec.hints.size() < best->spec.hints.size()))
      best = std::move(result);
  }
  return best;
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

namespace {

// The gate networks of one table: found once, reused by every pass.
// Exact synthesis gets its full time up to this many inputs.
constexpr int kNarrowTableInputs = 4;
constexpr double kWideSynthesisSeconds = 10.0;

struct Networks {
  bool searched = false;
  std::optional<Netlist> best;
  std::vector<Netlist> candidates;
};

// One pass of compose_spec with the step limits in `options`.
std::optional<ComposeResult> compose_once(const Spec &spec,
                                          const NetlistComposeOptions &options,
                                          const ProgressCallback &progress,
                                          Networks &networks) {
  BoolTable table = boolean_table(spec);
  SynthesisOptions synthesis;
  synthesis.cancel = options.cancel;
  synthesis.sat_seed = options.solver.sat_seed;
  synthesis.time_limit_seconds = options.synthesis_seconds;
  if (!networks.searched) {
    networks.searched = true;
    // A split network is found at once; exact synthesis only has to beat
    // it, and on wide tables (a 4-to-1 multiplexer has six inputs) it
    // rarely finishes, so it gets less time there.
    std::optional<Netlist> split = split_network(table);
    if (split) {
      synthesis.max_gates = std::min(synthesis.max_gates,
                                     static_cast<int>(split->gates.size()) - 1);
      if (table.inputs > kNarrowTableInputs)
        synthesis.time_limit_seconds =
            std::min(synthesis.time_limit_seconds, kWideSynthesisSeconds);
    }
    if (!split || synthesis.max_gates > 0)
      networks.best = synthesize(table, synthesis);
    if (!networks.best && split &&
        !(options.cancel && options.cancel->load())) {
      networks.best = std::move(split);
      if (progress)
        progress(std::format("no smaller network found in time; using a "
                             "split network of {} gates",
                             networks.best->gates.size()));
    }
    if (networks.best) {
      networks.candidates = {*networks.best};
      if (options.network_candidates > 1 && networks.best->gates.size() >= 3) {
        SynthesisOptions alternatives = synthesis;
        alternatives.time_limit_seconds = options.network_candidate_seconds;
        networks.candidates = synthesize_alternatives(
            table, *networks.best,
            static_cast<std::size_t>(options.network_candidates), alternatives);
        std::stable_sort(networks.candidates.begin(), networks.candidates.end(),
                         [](const Netlist &a, const Netlist &b) {
                           return network_layout_score(a) <
                                  network_layout_score(b);
                         });
      }
    }
  }
  if (!networks.best) {
    if (progress)
      progress("no gate network found (too many gates)");
    return std::nullopt;
  }
  const Netlist *netlist = &*networks.best;
  if (progress)
    progress(std::format("gate network:\n{}", netlist->describe()));

  const std::vector<BlockType> &allowed = spec.palette;
  const std::vector<BlockType> core = core_redstone_palette();
  const bool core_is_allowed =
      std::all_of(core.begin(), core.end(), [&](BlockType type) {
        return std::find(allowed.begin(), allowed.end(), type) != allowed.end();
      });
  const bool exact_one = is_exact_one_of_three(table);

  // A small fixed box can often hold the complete circuit more compactly
  // than a gate-module composition. For exact-one-of-three, try the direct
  // model first when the requested box has enough room for a vertical route.
  // Skip the preliminary basic-palette model: on this target it roughly
  // doubles the time to the first verified result.
  const long long box_volume = 1LL * spec.size.x * spec.size.y * spec.size.z;
  const bool height_allowed =
      options.max_height <= 0 || spec.size.y <= options.max_height;
  if (exact_one && spec.size.y >= 3 && box_volume <= 256 && height_allowed) {
    if (progress)
      progress("trying a direct exact-one circuit in the requested box");
    SolverOptions direct_options = options.solver;
    direct_options.time_limit_seconds = std::min(options.stage_seconds, 260.0);
    direct_options.stop_at_first = true;
    direct_options.quick_palette_search = false;
    direct_options.cancel = options.cancel;
    SolveReport direct = solve(spec, direct_options, progress);
    if (direct.solution) {
      Spec result_spec = spec;
      result_spec.hints.clear();
      const World &layout = direct.solution->layout;
      for (int i = 0; i < layout.volume(); ++i) {
        Vec3 p = layout.position_of(i);
        const Block &block = layout.at(p);
        if (block.type != BlockType::Air && block.type != BlockType::Input &&
            block.type != BlockType::Output)
          result_spec.hints.emplace_back(p, block);
      }
      if (progress)
        progress("direct exact-one circuit fits the requested box");
      return ComposeResult{std::move(result_spec),
                           std::move(direct.solution->layout)};
    }
    if (options.cancel && options.cancel->load())
      return std::nullopt;
    if (progress)
      progress("direct exact-one search found no circuit; trying gate modules");
  }

  const std::vector<Netlist> &candidates = networks.candidates;

  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (options.cancel && options.cancel->load())
      return std::nullopt;
    const Netlist &candidate = candidates[index];
    if (progress)
      progress(
          std::format("network candidate {} of {} (estimated cost {}):\n{}",
                      index + 1, candidates.size(),
                      network_layout_score(candidate), candidate.describe()));

    // The first, lowest-cost network gets the complete route budget. Divide
    // any fallback attempts among the remaining candidates so a portfolio
    // adds bounded recovery time instead of multiplying every stage limit.
    NetlistComposeOptions attempt = options;
    if (index > 0) {
      const double share =
          options.stage_seconds / static_cast<double>(candidates.size() - 1);
      attempt.stage_seconds = std::min(options.stage_seconds, share);
      attempt.gates.time_limit_seconds =
          std::min(options.gates.time_limit_seconds, share);
      attempt.flush_seconds = std::min(options.flush_seconds, share);
    }

    if (exact_one && core_is_allowed && core != allowed) {
      if (progress)
        progress("trying this network with the core redstone palette");
      NetlistComposeOptions core_options = attempt;
      core_options.palette = core;
      core_options.gates.palette = core;
      if (std::optional<ComposeResult> result =
              compose_with_palette(candidate, table, core_options, progress)) {
        result->spec.palette = spec.palette;
        return result;
      }
      if (options.cancel && options.cancel->load())
        return std::nullopt;
      if (progress)
        progress("core palette could not be routed; trying the full palette");
    }

    NetlistComposeOptions full_options = attempt;
    full_options.palette = spec.palette;
    full_options.gates.palette = spec.palette;
    if (std::optional<ComposeResult> result =
            compose_with_palette(candidate, table, full_options, progress)) {
      result->spec.palette = spec.palette;
      return result;
    }
  }
  return std::nullopt;
}

// Stops everything that watches `stop` once the deadline passes or `outer`
// (the caller's cancel flag) is set.
class DeadlineWatch {
public:
  DeadlineWatch(std::chrono::steady_clock::time_point deadline,
                std::atomic<bool> *outer)
      : thread_([this, deadline, outer](std::stop_token token) {
          while (!token.stop_requested()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                (outer && outer->load())) {
              this->stop_ = true;
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }) {}

  std::atomic<bool> *flag() { return &this->stop_; }

private:
  std::atomic<bool> stop_ = false;
  std::jthread thread_; // after stop_: joined before it goes
};

} // namespace

namespace {

// Game ticks the slowest switch between two input vectors takes until every
// output has its final value for good.
int slowest_switch(const World &layout, const Spec &spec) {
  constexpr int kMaxTicks = 400;
  const std::vector<InputVector> vectors = spec.verification_vectors();
  auto apply = [](Simulator &sim, const InputVector &v) {
    for (std::size_t c = 0; c < v.size(); ++c)
      sim.set_input(static_cast<int>(c), v[c]);
  };
  auto outputs = [&](const Simulator &sim) {
    std::vector<int> values;
    for (std::size_t o = 0; o < spec.outputs.size(); ++o)
      values.push_back(sim.output(static_cast<int>(o)));
    return values;
  };
  std::vector<std::vector<int>> settled;
  for (const InputVector &v : vectors) {
    Simulator sim(layout);
    apply(sim, v);
    sim.run_until_quiet(kMaxTicks);
    settled.push_back(outputs(sim));
  }
  int slowest = 0;
  for (std::size_t from = 0; from < vectors.size(); ++from) {
    Simulator sim(layout);
    apply(sim, vectors[from]);
    sim.run_until_quiet(kMaxTicks);
    for (std::size_t to = 0; to < vectors.size(); ++to) {
      if (to == from)
        continue;
      Simulator step = sim;
      apply(step, vectors[to]);
      int last_wrong = outputs(step) == settled[to] ? -1 : 0;
      for (int tick = 1; tick <= kMaxTicks && step.has_pending_ticks();
           ++tick) {
        step.tick();
        if (outputs(step) != settled[to])
          last_wrong = tick;
      }
      slowest = std::max(slowest, last_wrong + 1);
    }
  }
  return slowest;
}

// Build from gates checks only that outputs come to rest; the request's
// timing ("not before", "by") is checked on the finished circuit, which
// keeps it from then on (so a shrink keeps it too).
std::optional<ComposeResult> with_timing(std::optional<ComposeResult> result,
                                         const Spec &spec,
                                         const NetlistComposeOptions &options,
                                         const ProgressCallback &progress) {
  if (!result || (!spec.timing.earliest && !spec.timing.latest))
    return result;
  Spec timed = result->spec;
  timed.timing = spec.timing;
  if (!verify_layout(result->layout, timed, timed.verification_vectors(),
                     options.solver.verification)) {
    result->spec = std::move(timed);
    return result;
  }
  if (progress) {
    int ticks = slowest_switch(result->layout, result->spec);
    progress(std::format(
        "the circuit works but misses the timing: its slowest switch takes "
        "{} ticks{}; Build from gates cannot make it faster, so allow more "
        "time or use Search the box",
        ticks,
        spec.timing.latest ? std::format(" (asked: by {})", *spec.timing.latest)
                           : std::string()));
  }
  return std::nullopt;
}

std::optional<ComposeResult>
compose_untimed(const Spec &spec, const NetlistComposeOptions &options,
                const ProgressCallback &progress);

} // namespace

std::optional<ComposeResult> compose_spec(const Spec &spec,
                                          const NetlistComposeOptions &options,
                                          const ProgressCallback &progress) {
  NetlistComposeOptions o = options;
  o.timing = spec.timing;
  return with_timing(compose_untimed(spec, o, progress), spec, o, progress);
}

namespace {

std::optional<ComposeResult>
compose_untimed(const Spec &spec, const NetlistComposeOptions &options,
                const ProgressCallback &progress) {
  Networks networks;
  if (!options.deadline)
    return compose_once(spec, options, progress, networks);

  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline = *options.deadline;
  auto left = [&] {
    return std::chrono::duration<double>(deadline - Clock::now()).count();
  };
  constexpr double kShortestPass = 5.0; // seconds; less is not worth a pass
  constexpr double kInstantPassSeconds = 1.0;
  constexpr double kSynthesisShare = 0.2;
  DeadlineWatch watch(deadline, options.cancel);
  GateCache local_cache; // modules survive from pass to pass
  for (int pass = 0;; ++pass) {
    const double remaining = left();
    if (remaining < kShortestPass || (options.cancel && options.cancel->load()))
      return std::nullopt;
    const double scale = static_cast<double>(1 << std::min(pass, 6));
    NetlistComposeOptions o = options;
    o.cancel = watch.flag();
    o.gate_cache = options.gate_cache ? options.gate_cache : &local_cache;
    o.synthesis_seconds =
        std::min(options.synthesis_seconds, remaining * kSynthesisShare);
    o.flush_seconds = std::min(options.flush_seconds * scale, remaining);
    o.stage_seconds = std::min(options.stage_seconds * scale, remaining);
    o.narrow_seconds = std::min(options.narrow_seconds * scale, remaining);
    o.gates.time_limit_seconds =
        std::min(options.gates.time_limit_seconds * scale, remaining);
    o.max_channel_width = options.max_channel_width + 2 * pass;
    // Different shapes, not only more time: wires get room to go around
    // modules, gates may sit further from their signals.
    // The first pass is flush placement only: columns are slow and rarely
    // work where flush does not, so they wait for the next pass.
    o.columns = options.columns && pass > 0;
    o.flush_margin = options.flush_margin + pass;
    o.flush_spacing = options.flush_spacing + pass;
    o.solver.sat_seed = options.solver.sat_seed + pass;
    if (pass > 0 && progress)
      progress(std::format("pass {}: step limits x{:.0f}, {:.0f} s left",
                           pass + 1, scale, remaining));
    const Clock::time_point pass_start = Clock::now();
    if (std::optional<ComposeResult> result =
            compose_once(spec, o, progress, networks))
      return result;
    if (!networks.best)
      return std::nullopt; // no network: more time will not make one
    // A later pass (columns included) that fails at once ran out of nothing:
    // more time will not help, and repeating it only fills the log.
    if (pass > 0 &&
        std::chrono::duration<double>(Clock::now() - pass_start).count() <
            kInstantPassSeconds) {
      if (progress)
        progress("this pass failed without running out of time; stopping");
      return std::nullopt;
    }
  }
}

} // namespace

} // namespace rs

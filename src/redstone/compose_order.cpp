// Inputs in their own order: the circuit is built with its ports wherever
// they suit it, then moved east, and a short crossing region west of it
// takes each input from a port in order to where the circuit reads it.

#include <algorithm>
#include <format>
#include <map>
#include <numeric>

#include "redstone/compose_parts.hpp"
#include "redstone/shrink.hpp"
#include "redstone/simulator.hpp"
#include "redstone/verifier.hpp"
#include "redstone/wire_router.hpp"

namespace rs {

namespace {

using namespace compose_parts;

// Columns tried for the crossing region, rows added above and below it,
// and the height tried without a limit (a wire crosses another two layers
// above it).
constexpr int kMinOrderWidth = 3;
constexpr int kMaxOrderWidth = 10;
constexpr int kMaxOrderMargin = 2;
constexpr int kOrderHeight = 4;
// Cells at each end of a wire kept for it.
constexpr int kEndDepth = 2;
// Ticks a circuit gets to come to rest when its outputs are measured.
constexpr int kQuietTicks = 100;

Dir mirrored(Dir d) {
  return d == Dir::North ? Dir::South : d == Dir::South ? Dir::North : d;
}

// The circuit mirrored north to south, if it still works: inputs built in
// nearly the reverse order then come nearly in order.
std::optional<ComposeResult> mirror_z(const ComposeResult &built,
                                      const SolverOptions &solver) {
  const Vec3 size = built.layout.size();
  World layout(size);
  for (int i = 0; i < layout.volume(); ++i) {
    const Vec3 p = built.layout.position_of(i);
    Block b = built.layout.at(p);
    b.facing = mirrored(b.facing);
    layout.set({p.x, p.y, size.z - 1 - p.z}, b);
  }
  Spec spec = built.spec;
  for (InputPort &in : spec.inputs) {
    in.position.z = size.z - 1 - in.position.z;
    in.facing = mirrored(in.facing);
  }
  for (OutputPort &out : spec.outputs) {
    out.position.z = size.z - 1 - out.position.z;
    out.facing = mirrored(out.facing);
  }
  if (!layout.validate().empty())
    return std::nullopt;
  spec = spec_with_layout(spec, layout);
  if (verify_layout(layout, spec, spec.verification_vectors(),
                    solver.verification))
    return std::nullopt;
  return ComposeResult{std::move(spec), std::move(layout)};
}

// Where each input's port is, if all are repeaters facing east on the west
// side.
std::optional<std::vector<Vec3>> west_ports(const ComposeResult &built) {
  std::vector<Vec3> ports;
  for (std::size_t i = 0; i < built.spec.inputs.size(); ++i) {
    std::optional<Vec3> p = built.layout.find_input(static_cast<int>(i));
    if (!p || p->x != 0 || built.spec.inputs[i].facing != Dir::East ||
        built.spec.inputs[i].kind != InputKind::Repeater)
      return std::nullopt;
    ports.push_back(*p);
  }
  return ports;
}

// The circuit moved `width` columns east and `margin` rows south, its
// inputs on the same rows in order and wired across, drawn in `order`. The
// first wire turns off its row by column `first_turn` (none: anywhere): the
// router turns it as late as it can, leaving a wire that crosses it no
// room to come back down.
std::optional<ComposeResult>
cross_into_order(const ComposeResult &built, const std::vector<Vec3> &old_ports,
                 int height, int width, int margin,
                 const std::vector<int> &order, std::optional<int> first_turn,
                 const NetlistComposeOptions &options) {
  const int n = static_cast<int>(old_ports.size());
  std::vector<int> rows;
  for (Vec3 p : old_ports)
    rows.push_back(p.z);
  std::sort(rows.begin(), rows.end());
  const Vec3 size = built.layout.size();
  const Vec3 shift{width, 0, margin};
  Cells cells;
  add_layout(cells, built.layout, shift); // old ports become repeaters
  std::vector<Vec3> ports(n);
  for (int i = 0; i < n; ++i) {
    ports[i] = {0, old_ports[i].y, rows[i] + margin};
    cells[key(ports[i])] = Block::repeater(Dir::East);
    cells[key(ports[i] - Vec3{0, 1, 0})] = Block::solid();
  }
  const Vec3 grown{size.x + width, height, size.z + 2 * margin};
  WireRouterOptions o;
  o.low = {1, 1, 0};
  o.high = {width - 1, height - 1, grown.z - 1};
  o.repeaters = !options.palette ||
                std::find(options.palette->begin(), options.palette->end(),
                          BlockType::Repeater) != options.palette->end();
  // Each wire's first cell past its port and last before the circuit, on
  // every level, kept from the others: a wire drawn through another's end
  // leaves it no way in (the first wire along the top row, past every
  // goal).
  // Two cells deep, and the cells beside them: a wire beside the end would
  // join it.
  std::map<std::pair<int, int>, int> ends; // {x, z}: input
  for (int i = 0; i < n; ++i)
    for (int depth = 0; depth < kEndDepth; ++depth)
      for (int dz = -1; dz <= 1; ++dz) {
        ends.emplace(std::pair{1 + depth, ports[i].z + dz}, i);
        ends.emplace(std::pair{width - 1 - depth, old_ports[i].z + margin + dz},
                     i);
      }
  int routing = 0;
  auto blocked = [&](Vec3 c) {
    if (c.x >= width)
      return true;
    if (first_turn && routing == order.front() && c.x > *first_turn &&
        c.z != old_ports[routing].z + margin)
      return true;
    auto it = ends.find({c.x, c.z});
    return it != ends.end() && it->second != routing;
  };
  for (int i : order) {
    routing = i;
    WireRouter router(cells, blocked, o);
    const Vec3 to = old_ports[i] + shift - Vec3{1, 0, 0};
    std::optional<std::vector<std::pair<Vec3, Block>>> wire =
        router.route({{ports[i], kMaxSignal, Dir::East}}, {}, {to, Dir::East});
    if (!wire)
      return std::nullopt;
    for (const auto &[c, block] : *wire)
      cells[key(c)] = block;
  }

  Spec spec = built.spec;
  spec.size = grown;
  spec.hints.clear();
  for (int i = 0; i < n; ++i)
    spec.inputs[i].position = ports[i];
  for (OutputPort &out : spec.outputs)
    out.position = out.position + shift;
  World result = spec.base_world();
  for (const auto &[cell, block] : cells) {
    const Vec3 p{cell[0], cell[1], cell[2]};
    const BlockType here = result.at(p).type;
    if (here != BlockType::Input && here != BlockType::Output)
      result.set(p, block);
  }
  if (!result.validate().empty())
    return std::nullopt;
  spec = spec_with_layout(spec, result);
  if (verify_layout(result, spec, spec.verification_vectors(),
                    options.solver.verification))
    return std::nullopt;
  return ComposeResult{std::move(spec), std::move(result)};
}

// Where each output's port is, if all are dust (anywhere in the circuit:
// a built circuit's outputs sit at the end of their gates).
std::optional<std::vector<Vec3>> dust_outputs(const ComposeResult &built) {
  std::vector<Vec3> ports;
  for (std::size_t o = 0; o < built.spec.outputs.size(); ++o) {
    std::optional<Vec3> p = built.layout.find_output(static_cast<int>(o));
    if (!p || built.spec.outputs[o].kind != OutputKind::Dust)
      return std::nullopt;
    ports.push_back(*p);
  }
  return ports;
}

// The weakest each output is when on, over every input row (the strength a
// wire leaving it starts with); nothing if a row does not settle.
std::optional<std::vector<int>> weakest_on(const ComposeResult &built) {
  std::vector<int> weakest(built.spec.outputs.size(), kMaxSignal);
  Simulator start(built.layout);
  if (!start.run_until_quiet(kQuietTicks))
    return std::nullopt;
  for (const InputVector &v : built.spec.verification_vectors()) {
    Simulator sim = start;
    for (std::size_t k = 0; k < v.size(); ++k)
      sim.set_input(static_cast<int>(k), v[k]);
    if (!sim.run_until_quiet(kQuietTicks))
      return std::nullopt;
    for (std::size_t o = 0; o < weakest.size(); ++o) {
      const int p = sim.output(static_cast<int>(o));
      if (p > 0)
        weakest[o] = std::min(weakest[o], p);
    }
  }
  return weakest;
}

// The outputs' rows from north to south in output order (OUT1 first), or
// the other way round.
bool outputs_in_order(const std::vector<Vec3> &ports, bool reversed) {
  for (std::size_t i = 1; i < ports.size(); ++i)
    if (reversed ? ports[i].z >= ports[i - 1].z : ports[i].z <= ports[i - 1].z)
      return false;
  return true;
}

// The circuit with `width` columns added east of it and `margin` rows
// above and below, and a wire from each old output (through the circuit's
// free cells if it sits inside) to a new output port on the east side, OUT
// i on row targets[i], drawn in `order`.
std::optional<ComposeResult> cross_out_of_order(
    const ComposeResult &built, const std::vector<Vec3> &old_ports,
    const std::vector<int> &strengths, const std::vector<int> &targets,
    int height, int width, int margin, const std::vector<int> &order,
    const NetlistComposeOptions &options) {
  const int n = static_cast<int>(old_ports.size());
  const Vec3 size = built.layout.size();
  const Vec3 shift{0, 0, margin};
  const int port_x = size.x + width - 1;
  Cells cells;
  add_layout(cells, built.layout, shift); // old output ports become dust
  std::vector<Vec3> starts(n), ports(n);
  for (int i = 0; i < n; ++i) {
    starts[i] = old_ports[i] + shift;
    ports[i] = {port_x, old_ports[i].y, targets[i] + margin};
  }
  const Vec3 grown{size.x + width, height, size.z + 2 * margin};
  WireRouterOptions o;
  o.low = {0, 1, 0};
  o.high = {port_x - 1, height - 1, grown.z - 1};
  o.repeaters = !options.palette ||
                std::find(options.palette->begin(), options.palette->end(),
                          BlockType::Repeater) != options.palette->end();
  // Each wire's last cells before its port (and those beside them) are
  // kept from the other wires.
  std::map<std::pair<int, int>, int> ends; // {x, z}: output
  for (int i = 0; i < n; ++i)
    for (int depth = 0; depth < kEndDepth; ++depth)
      for (int dz = -1; dz <= 1; ++dz)
        ends.emplace(std::pair{port_x - 1 - depth, ports[i].z + dz}, i);
  int routing = 0;
  auto blocked = [&](Vec3 c) {
    if (c.x >= port_x)
      return true;
    auto it = ends.find({c.x, c.z});
    return it != ends.end() && it->second != routing;
  };
  for (int i : order) {
    routing = i;
    WireRouter router(cells, blocked, o);
    const Vec3 to = ports[i] - Vec3{1, 0, 0};
    std::optional<std::vector<std::pair<Vec3, Block>>> wire =
        router.route({{starts[i], strengths[i], std::nullopt}},
                     {key(starts[i])}, {to, Dir::East});
    if (!wire)
      return std::nullopt;
    for (const auto &[c, block] : *wire)
      cells[key(c)] = block;
  }
  for (int i = 0; i < n; ++i)
    cells[key(ports[i] - Vec3{0, 1, 0})] = Block::solid();

  Spec spec = built.spec;
  spec.size = grown;
  spec.hints.clear();
  for (InputPort &in : spec.inputs)
    in.position = in.position + shift;
  for (int i = 0; i < n; ++i)
    spec.outputs[i].position = ports[i];
  World result = spec.base_world();
  for (const auto &[cell, block] : cells) {
    const Vec3 p{cell[0], cell[1], cell[2]};
    const BlockType here = result.at(p).type;
    if (here != BlockType::Input && here != BlockType::Output)
      result.set(p, block);
  }
  if (!result.validate().empty())
    return std::nullopt;
  spec = spec_with_layout(spec, result);
  if (verify_layout(result, spec, spec.verification_vectors(),
                    options.solver.verification))
    return std::nullopt;
  return ComposeResult{std::move(spec), std::move(result)};
}

} // namespace

std::optional<ComposeResult>
compose_parts::with_outputs_in_order(const ComposeResult &built,
                                     const NetlistComposeOptions &options,
                                     const ProgressCallback &progress) {
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  const bool may_reverse = options.allow_reversed_outputs;
  std::optional<std::vector<Vec3>> ports = dust_outputs(built);
  if (!ports) {
    say("outputs in order: the output ports are not all dust");
    return std::nullopt;
  }
  auto done = [&](const ComposeResult &c, const std::vector<Vec3> &p,
                  bool reversed) {
    for (Vec3 q : p)
      if (q.x != c.layout.size().x - 1)
        return false;
    return outputs_in_order(p, reversed);
  };
  if (done(built, *ports, false) || (may_reverse && done(built, *ports, true)))
    return built;

  struct Base {
    ComposeResult circuit;
    std::vector<Vec3> ports;
    std::vector<int> strengths;
    std::string_view name;
  };
  std::vector<Base> bases;
  if (std::optional<std::vector<int>> s = weakest_on(built))
    bases.push_back({built, *ports, std::move(*s), ""});
  // Mirrored, the outputs come in the reverse order; only while the inputs
  // may come in any order, since it reverses theirs too.
  if (!options.keep_input_order)
    if (std::optional<ComposeResult> mirror = mirror_z(built, options.solver))
      if (std::optional<std::vector<Vec3>> p = dust_outputs(*mirror)) {
        if (done(*mirror, *p, false)) {
          say("outputs in order: the circuit mirrored north to south");
          return mirror;
        }
        if (std::optional<std::vector<int>> s = weakest_on(*mirror))
          bases.push_back({std::move(*mirror), std::move(*p), std::move(*s),
                           " (mirrored)"});
      }
  if (bases.empty()) {
    say("outputs in order: the circuit does not come to rest");
    return std::nullopt;
  }

  const int n = static_cast<int>(ports->size());
  const Vec3 size = built.layout.size();
  const int top = options.max_height > 0 ? options.max_height
                                         : std::max(size.y, kOrderHeight);
  // The first that fits for each way round (fewest columns first); the
  // smaller of the two is kept.
  auto search = [&](bool reversed) -> std::optional<ComposeResult> {
    for (int height = size.y; height <= top; ++height)
      for (int width = kMinOrderWidth; width <= kMaxOrderWidth; ++width)
        for (int margin = 0; margin <= kMaxOrderMargin; ++margin)
          for (const Base &base : bases) {
            // New ports on the rows the outputs had, in order.
            std::vector<int> rows;
            for (Vec3 p : base.ports)
              rows.push_back(p.z);
            std::sort(rows.begin(), rows.end());
            std::vector<int> targets(n);
            for (int i = 0; i < n; ++i)
              targets[i] = reversed ? rows[n - 1 - i] : rows[i];
            // Wires drawn first: the eastmost output, the westmost, or in
            // output order; and each of those the other way round.
            std::vector<int> by_index(n);
            std::iota(by_index.begin(), by_index.end(), 0);
            std::vector<int> east_first = by_index;
            std::stable_sort(east_first.begin(), east_first.end(),
                             [&](int a, int b) {
                               return base.ports[a].x > base.ports[b].x;
                             });
            std::vector<int> far_first = by_index;
            std::stable_sort(far_first.begin(), far_first.end(),
                             [&](int a, int b) {
                               return std::abs(base.ports[a].z - targets[a]) >
                                      std::abs(base.ports[b].z - targets[b]);
                             });
            std::vector<std::vector<int>> orders{east_first, far_first,
                                                 by_index};
            for (std::size_t k = 0, count = orders.size(); k < count; ++k)
              orders.emplace_back(orders[k].rbegin(), orders[k].rend());
            for (const std::vector<int> &order : orders) {
              if (options.cancel && options.cancel->load())
                return std::nullopt;
              if (std::optional<ComposeResult> result = cross_out_of_order(
                      base.circuit, base.ports, base.strengths, targets, height,
                      width, margin, order, options)) {
                const Vec3 grown = result->layout.size();
                say(std::format("outputs {}: {} columns behind the {}x{}x{} "
                                "circuit{}: {}x{}x{}",
                                reversed ? "in reverse order" : "in order",
                                width, size.x, size.y, size.z, base.name,
                                grown.x, grown.y, grown.z));
                return result;
              }
            }
          }
    return std::nullopt;
  };
  std::optional<ComposeResult> best = search(false);
  if (may_reverse && !(options.cancel && options.cancel->load()))
    if (std::optional<ComposeResult> back = search(true)) {
      const Vec3 a = back->layout.size();
      if (!best || a.x * a.z < best->layout.size().x * best->layout.size().z ||
          (a.x * a.z == best->layout.size().x * best->layout.size().z &&
           a.y < best->layout.size().y))
        best = std::move(back);
    }
  if (!best)
    say(std::format("outputs in order: no crossings found within {} columns",
                    kMaxOrderWidth));
  return best;
}

std::optional<ComposeResult>
compose_parts::with_inputs_in_order(const ComposeResult &built,
                                    const NetlistComposeOptions &options,
                                    const ProgressCallback &progress) {
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  std::optional<std::vector<Vec3>> ports = west_ports(built);
  if (!ports) {
    say("inputs in order: the ports are not all repeaters on the west side");
    return std::nullopt;
  }
  const int n = static_cast<int>(ports->size());
  auto in_order = [&](const std::vector<Vec3> &p) {
    for (int i = 1; i < n; ++i)
      if (p[i].z <= p[i - 1].z)
        return false;
    return true;
  };
  if (in_order(*ports))
    return built;

  struct Base {
    ComposeResult circuit;
    std::vector<Vec3> ports;
    std::string_view name;
  };
  std::vector<Base> bases{{built, *ports, ""}};
  if (std::optional<ComposeResult> mirror = mirror_z(built, options.solver))
    if (std::optional<std::vector<Vec3>> p = west_ports(*mirror)) {
      if (in_order(*p)) {
        say("inputs in order: the circuit mirrored north to south");
        return mirror;
      }
      bases.push_back({std::move(*mirror), std::move(*p), " (mirrored)"});
    }

  const Vec3 size = built.layout.size();
  const int top = options.max_height > 0 ? options.max_height
                                         : std::max(size.y, kOrderHeight);
  for (int height = size.y; height <= top; ++height)
    for (int width = kMinOrderWidth; width <= kMaxOrderWidth; ++width)
      for (int margin = 0; margin <= kMaxOrderMargin; ++margin)
        for (const Base &base : bases) {
          // Wires drawn first: the longest way across, the shortest, or in
          // input order; each is drawn around those before it.
          std::vector<int> rows;
          for (Vec3 p : base.ports)
            rows.push_back(p.z);
          std::sort(rows.begin(), rows.end());
          std::vector<int> by_index(n);
          std::iota(by_index.begin(), by_index.end(), 0);
          auto by_shift = [&](bool longest_first) {
            std::vector<int> order = by_index;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
              const int da = std::abs(base.ports[a].z - rows[a]);
              const int db = std::abs(base.ports[b].z - rows[b]);
              return longest_first ? da > db : da < db;
            });
            return order;
          };
          std::vector<std::vector<int>> orders{by_shift(true), by_shift(false),
                                               by_index};
          for (std::size_t k = 0, count = orders.size(); k < count; ++k)
            orders.emplace_back(orders[k].rbegin(), orders[k].rend());
          std::vector<std::optional<int>> turns{std::nullopt};
          for (int t = 1; t < width - kEndDepth; ++t)
            turns.push_back(t);
          for (const std::optional<int> &turn : turns)
            for (const std::vector<int> &order : orders) {
              if (options.cancel && options.cancel->load())
                return std::nullopt;
              if (std::optional<ComposeResult> result =
                      cross_into_order(base.circuit, base.ports, height, width,
                                       margin, order, turn, options)) {
                const Vec3 grown = result->layout.size();
                say(std::format("inputs in order: {} columns of crossings in "
                                "front of the {}x{}x{} circuit{}: {}x{}x{}",
                                width, size.x, size.y, size.z, base.name,
                                grown.x, grown.y, grown.z));
                return result;
              }
            }
        }
  say(std::format("inputs in order: no crossings found within {} columns",
                  kMaxOrderWidth));
  return std::nullopt;
}

} // namespace rs

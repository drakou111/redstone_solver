#include "redstone/verifier.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

#include "redstone/simulator.hpp"

namespace rs {

namespace {

constexpr std::array<Vec3, 3> kOrigins{Vec3{0, 0, 0}, Vec3{5, 1, 3},
                                       Vec3{-7, 64, 12}};
constexpr int kCopiesPerTiledAxis = 3;
constexpr int kMaxChannel = 255;

// The layout repeated along every tiled axis. Port channels are renumbered so
// copy k owns channels k * port_count .. k * port_count + port_count - 1.
struct Tiling {
  World world{{1, 1, 1}};
  int copies = 1;
  int center = 0;
};

Tiling make_tiling(const World &layout, const Spec &spec) {
  Vec3 size = layout.size();
  Vec3 counts{spec.tiled[0] ? kCopiesPerTiledAxis : 1,
              spec.tiled[1] ? kCopiesPerTiledAxis : 1,
              spec.tiled[2] ? kCopiesPerTiledAxis : 1};
  Tiling tiling;
  tiling.world =
      World({size.x * counts.x, size.y * counts.y, size.z * counts.z});
  tiling.copies = counts.x * counts.y * counts.z;
  auto copy_index = [&](int cx, int cy, int cz) {
    return (cy * counts.z + cz) * counts.x + cx;
  };
  tiling.center = copy_index(counts.x / 2, counts.y / 2, counts.z / 2);

  int inputs = static_cast<int>(spec.inputs.size());
  int outputs = static_cast<int>(spec.outputs.size());
  if (tiling.copies * std::max(inputs, outputs) > kMaxChannel + 1)
    throw std::invalid_argument("too many ports to verify the tiled layout");

  for (int cx = 0; cx < counts.x; ++cx)
    for (int cy = 0; cy < counts.y; ++cy)
      for (int cz = 0; cz < counts.z; ++cz) {
        int copy = copy_index(cx, cy, cz);
        Vec3 corner{cx * size.x, cy * size.y, cz * size.z};
        for (int i = 0; i < layout.volume(); ++i) {
          Vec3 p = layout.position_of(i);
          Block block = layout.at(p);
          if (block.type == BlockType::Input)
            block.channel =
                static_cast<std::uint8_t>(copy * inputs + block.channel);
          if (block.type == BlockType::Output)
            block.channel =
                static_cast<std::uint8_t>(copy * outputs + block.channel);
          tiling.world.set(corner + p, block);
        }
      }
  return tiling;
}

class Harness {
public:
  Harness(const Spec &spec, const Tiling &tiling,
          const VerificationOptions &options)
      : spec_(spec), tiling_(tiling), options_(options) {}

  void set_inputs(Simulator &sim, int copy, const InputVector &inputs) const {
    int base = copy * static_cast<int>(this->spec_.inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i)
      sim.set_input(base + static_cast<int>(i), inputs[i]);
  }

  std::vector<int> outputs(const Simulator &sim, int copy) const {
    int count = static_cast<int>(this->spec_.outputs.size());
    std::vector<int> values;
    for (int o = 0; o < count; ++o)
      values.push_back(sim.output(copy * count + o));
    return values;
  }

  std::optional<std::string>
  outside(const Simulator &sim, const std::vector<SignalRange> &expected,
          int tick, int copy) const {
    int count = static_cast<int>(this->spec_.outputs.size());
    for (int o = 0; o < count; ++o) {
      int value = sim.output(copy * count + o);
      if (!expected[o].contains(value))
        return std::format("{}OUT{} is {} at tick {}, expected {}",
                           this->copy_name(copy), o + 1, value, tick,
                           to_string(expected[o]));
    }
    return std::nullopt;
  }

  bool outputs_equal(const Simulator &sim, int copy,
                     const std::vector<int> &expected) const {
    int count = static_cast<int>(this->spec_.outputs.size());
    for (int o = 0; o < count; ++o)
      if (sim.output(copy * count + o) != expected[o])
        return false;
    return true;
  }

  // Rules for a copy whose inputs just switched: correct from the deadline
  // on, and unchanged before the earliest tick if requested.
  std::optional<std::string>
  check_switched(const Simulator &sim, const std::vector<int> &before,
                 const std::vector<SignalRange> &expected, int tick,
                 int copy) const {
    const TimingSpec &timing = this->spec_.timing;
    int deadline = timing.latest ? *timing.latest : timing.horizon();
    if (tick >= deadline)
      if (auto problem = this->outside(sim, expected, tick, copy))
        return problem;
    if (timing.hold_previous && timing.earliest && tick < *timing.earliest &&
        !this->outputs_equal(sim, copy, before))
      return std::format("{}outputs changed at tick {}, before the earliest "
                         "allowed tick {}",
                         this->copy_name(copy), tick, *timing.earliest);
    return std::nullopt;
  }

  using TickCheck =
      std::function<std::optional<std::string>(const Simulator &, int)>;

  // Ticks until the circuit is at rest after the horizon, checking each tick.
  std::optional<std::string> run(Simulator &sim, const TickCheck &check) const {
    int horizon = this->spec_.timing.horizon();
    for (int t = 0; t <= horizon + this->options_.max_extra_ticks; ++t) {
      if (t > 0)
        sim.tick();
      if (auto problem = check(sim, t))
        return problem;
      if (t >= horizon && !sim.has_pending_ticks())
        return std::nullopt;
    }
    return std::format("still active {} ticks after the horizon",
                       this->options_.max_extra_ticks);
  }

  std::string copy_name(int copy) const {
    if (this->tiling_.copies == 1)
      return "";
    return copy == this->tiling_.center ? "center copy: "
                                        : std::format("copy {}: ", copy);
  }

private:
  const Spec &spec_;
  const Tiling &tiling_;
  const VerificationOptions &options_;
};

std::optional<VerificationFailure>
check_placement(const Tiling &tiling, const Spec &spec,
                const std::vector<InputVector> &vectors,
                const std::vector<std::vector<SignalRange>> &expected,
                const std::vector<InputVector> &backgrounds,
                const VerificationOptions &options, Placement placement) {
  Harness h(spec, tiling, options);
  auto fail = [&](std::optional<InputVector> from, InputVector to,
                  std::string reason, bool interference) {
    return VerificationFailure{placement, std::move(from), std::move(to),
                               std::move(reason), interference};
  };

  std::optional<Simulator> built;
  try {
    built.emplace(tiling.world, placement);
  } catch (const std::invalid_argument &error) {
    return fail(std::nullopt, vectors.front(),
                std::string("invalid layout: ") + error.what(), false);
  }
  if (!built->run_until_quiet(options.max_settle_ticks))
    return fail(std::nullopt, vectors.front(),
                "does not settle after being built", false);

  // Every copy gets the same inputs (the only case without tiling).
  std::vector<Simulator> settled;
  for (std::size_t u = 0; u < vectors.size(); ++u) {
    Simulator sim = *built;
    for (int copy = 0; copy < tiling.copies; ++copy)
      h.set_inputs(sim, copy, vectors[u]);
    if (!sim.run_until_quiet(options.max_settle_ticks))
      return fail(std::nullopt, vectors[u], "does not settle", false);
    for (int copy = 0; copy < tiling.copies; ++copy)
      if (auto problem = h.outside(sim, expected[u], 0, copy))
        return fail(std::nullopt, vectors[u],
                    "settled state wrong: " + *problem, false);
    settled.push_back(std::move(sim));
  }
  for (std::size_t u = 0; u < vectors.size(); ++u) {
    for (std::size_t v = 0; v < vectors.size(); ++v) {
      if (u == v)
        continue;
      Simulator sim = settled[u];
      std::vector<std::vector<int>> before;
      for (int copy = 0; copy < tiling.copies; ++copy) {
        before.push_back(h.outputs(sim, copy));
        h.set_inputs(sim, copy, vectors[v]);
      }
      auto problem = h.run(sim, [&](const Simulator &s, int t) {
        for (int copy = 0; copy < tiling.copies; ++copy)
          if (auto p = h.check_switched(s, before[copy], expected[v], t, copy))
            return p;
        return std::optional<std::string>{};
      });
      if (problem)
        return fail(vectors[u], vectors[v], *problem, false);
    }
  }

  if (tiling.copies == 1)
    return std::nullopt;

  // The center copy switches while its neighbours hold other inputs; the
  // neighbours' outputs must not move at all.
  for (const InputVector &background : backgrounds) {
    for (std::size_t u = 0; u < vectors.size(); ++u) {
      Simulator base = *built;
      for (int copy = 0; copy < tiling.copies; ++copy)
        h.set_inputs(base, copy,
                     copy == tiling.center ? vectors[u] : background);
      if (!base.run_until_quiet(options.max_settle_ticks))
        return fail(
            std::nullopt, vectors[u],
            "does not settle next to copies at " + to_string(background), true);
      if (auto problem =
              h.outside(base, expected[u], 0, tiling.center))
        return fail(std::nullopt, vectors[u],
                    "settled state wrong next to copies at " +
                        to_string(background) + ": " + *problem,
                    true);

      for (std::size_t v = 0; v < vectors.size(); ++v) {
        if (u == v)
          continue;
        Simulator sim = base;
        std::vector<std::vector<int>> before;
        for (int copy = 0; copy < tiling.copies; ++copy)
          before.push_back(h.outputs(sim, copy));
        h.set_inputs(sim, tiling.center, vectors[v]);
        auto problem = h.run(sim, [&](const Simulator &s, int t) {
          for (int copy = 0; copy < tiling.copies; ++copy) {
            if (copy == tiling.center) {
              if (auto p = h.check_switched(s, before[copy], expected[v], t,
                                            copy))
                return p;
            } else if (!h.outputs_equal(s, copy, before[copy])) {
              return std::optional<std::string>{std::format(
                  "{}output changed at tick {} although its inputs did not",
                  h.copy_name(copy), t)};
            }
          }
          return std::optional<std::string>{};
        });
        if (problem)
          return fail(vectors[u], vectors[v],
                      *problem + " (neighbours at " + to_string(background) +
                          ")",
                      true);
      }
    }
  }
  return std::nullopt;
}

} // namespace

bool VerificationFailure::at_identity_placement() const {
  return this->placement.origin == Vec3{} &&
         this->placement.symmetry.quarter_turns() == 0 &&
         !this->placement.symmetry.mirrored();
}

std::vector<Placement> standard_placements() {
  std::vector<Placement> placements;
  for (Vec3 origin : kOrigins)
    for (Symmetry symmetry : Symmetry::all())
      placements.push_back({origin, symmetry});
  return placements;
}

std::optional<VerificationFailure>
verify_layout(const World &layout, const Spec &spec,
              const std::vector<InputVector> &vectors,
              const VerificationOptions &options) {
  if (vectors.empty())
    throw std::invalid_argument("nothing to verify: no input vectors");
  std::vector<Placement> placements =
      options.placements.empty() ? standard_placements() : options.placements;
  std::vector<std::vector<SignalRange>> expected;
  expected.reserve(vectors.size());
  for (const InputVector &vector : vectors) {
    std::optional<std::vector<SignalRange>> wanted =
        spec.expected_outputs(vector);
    if (!wanted)
      throw std::invalid_argument("verification vector " + to_string(vector) +
                                  " has no row");
    expected.push_back(std::move(*wanted));
  }
  Tiling tiling = make_tiling(layout, spec);
  std::vector<InputVector> backgrounds = spec.representative_vectors();

  // Verification often runs inside parallel solver or shrink jobs. Launching
  // every placement at once multiplies those worker counts and can leave the
  // machine oversubscribed. Process bounded batches in placement order so the
  // reported failure stays deterministic and later placements are skipped
  // once an earlier batch has found one.
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const std::size_t workers = std::min<std::size_t>(
      placements.size(), std::min(8u, std::max(1u, hardware / 2)));
  for (std::size_t begin = 0; begin < placements.size(); begin += workers) {
    const std::size_t end = std::min(placements.size(), begin + workers);
    std::vector<std::future<std::optional<VerificationFailure>>> checks;
    checks.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i)
      checks.push_back(std::async(
          std::launch::async, check_placement, std::cref(tiling),
          std::cref(spec), std::cref(vectors), std::cref(expected),
          std::cref(backgrounds), std::cref(options), placements[i]));
    for (auto &check : checks)
      if (std::optional<VerificationFailure> failure = check.get())
        return failure;
  }
  return std::nullopt;
}

std::string to_string(const InputVector &vector) {
  std::string text = "[";
  for (std::size_t i = 0; i < vector.size(); ++i)
    text += std::format("{}{}", i ? " " : "", vector[i]);
  return text + "]";
}

} // namespace rs

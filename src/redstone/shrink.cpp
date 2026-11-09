#include "redstone/shrink.hpp"

#include "redstone/verifier.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rs {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds kCancelPollInterval{50};
constexpr std::string_view kAxisNames = "xyz";

int &component(Vec3 &p, int axis) {
  return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

// Moves a position back across the removed slice; nothing if it was in it.
std::optional<Vec3> shifted(Vec3 p, int axis, int index) {
  int &v = component(p, axis);
  if (v == index)
    return std::nullopt;
  if (v > index)
    --v;
  return p;
}

template <typename Port> bool shift_port(Port &port, int axis, int index) {
  if (port.is_free()) {
    std::vector<Vec3> region;
    for (Vec3 p : port.region)
      if (auto q = shifted(p, axis, index))
        region.push_back(*q);
    if (region.empty())
      return false;
    port.region = std::move(region);
    return true;
  }
  auto q = shifted(port.position, axis, index);
  if (!q)
    return false;
  port.position = *q;
  return true;
}

void shift_blocks(std::vector<std::pair<Vec3, Block>> &blocks, int axis,
                  int index) {
  std::vector<std::pair<Vec3, Block>> kept;
  for (const auto &[p, block] : blocks)
    if (auto q = shifted(p, axis, index))
      kept.emplace_back(*q, block);
  blocks = std::move(kept);
}

struct Attempt {
  Spec spec; // what is solved: the cut, maybe narrowed to its seam
  Spec base; // the plain cut, the starting point once it works
  std::string name;
  double seconds = 0; // at most; also bounded by the shrink's deadline
  bool whole = false; // the plain cut, repaired anywhere
};

int coordinate(Vec3 p, int axis) {
  return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

// The cut with only the slices near its seam left to the solver: blocks
// further away are kept as they are and their empty cells stay empty. A
// small problem, so a repair that only needs local changes is found (or
// ruled out) quickly. Nothing for specs whose ports the solver places.
std::optional<Spec> near_seam(const Spec &cut, int axis, int index,
                              int half_width) {
  for (const InputPort &port : cut.inputs)
    if (port.is_free())
      return std::nullopt;
  for (const OutputPort &port : cut.outputs)
    if (port.is_free())
      return std::nullopt;
  auto near = [&](Vec3 p) {
    int v = coordinate(p, axis);
    return v >= index - half_width && v < index + half_width;
  };
  Spec local = cut;
  std::vector<std::pair<Vec3, Block>> hints;
  for (const auto &[p, block] : cut.hints) {
    if (near(p))
      hints.emplace_back(p, block);
    else
      local.fixed.emplace_back(p, block);
  }
  local.hints = std::move(hints);
  World taken = local.base_world();
  for (const auto &[p, block] : local.fixed)
    taken.set(p, block);
  for (const auto &[p, block] : local.hints)
    taken.set(p, block);
  for (Vec3 p : cut.forbidden)
    taken.set(p, Block::solid()); // already empty; just not listed twice
  for (int i = 0; i < taken.volume(); ++i) {
    Vec3 p = taken.position_of(i);
    if (!near(p) && taken.at(p).type == BlockType::Air)
      local.forbidden.push_back(p);
  }
  return local;
}

// Repair tries per cut, cheapest first: a narrow window around the seam,
// a wider one, then the whole circuit.
struct Phase {
  int half_width; // 0: the whole circuit
  double seconds;
  std::string_view name;
};
constexpr std::array<Phase, 3> kPhases{{{1, 15.0, "near the cut"},
                                        {2, 40.0, "around the cut"},
                                        {0, 0.0, "whole circuit"}}};

int cells(Vec3 size) { return size.x * size.y * size.z; }

// Tidying: windows this wide along x and z, this far apart, each solved
// for at most this long.
constexpr int kTidyWindow = 3;
constexpr int kTidyStride = 2;
constexpr double kTidyWindowSeconds = 10.0;
// Less time than this left after shrinking is not worth tidying.
constexpr double kTidyMinSeconds = 5.0;

// How often a dead end may send the search back a step.
constexpr int kMaxBacktracks = 3;
// Diversifying: time to build one other circuit, failures in a row before
// walking back to the best circuit, and the share of the whole time kept
// for tidying at the end.
constexpr double kVariantSeconds = 20.0;
constexpr int kVariantTries = 4;
// Changes to the kept share, in turn: a failure is not just repeated.
constexpr std::array<double, 4> kKeepSteps{0.0, -0.1, 0.1, -0.2};
constexpr double kTidyShare = 0.1;

// One verified box on the shrink's path and the cuts tried from it.
struct ShrinkNode {
  Spec spec;
  World layout;
  std::vector<Attempt> attempts;
  std::vector<bool> finished; // solved or failed in full
  bool tried_as_is = false;
};

// The cut layout exactly as it is, when it already works: a slice of
// straight wire along the axis or of air needs no solve at all.
std::optional<World> works_as_is(const Spec &cut, const SolverOptions &solver) {
  World layout = cut.base_world();
  for (const auto &[p, block] : cut.hints) {
    BlockType here = layout.at(p).type;
    if (here != BlockType::Input && here != BlockType::Output)
      layout.set(p, block);
  }
  if (!layout.validate().empty())
    return std::nullopt;
  if (verify_layout(layout, cut, cut.verification_vectors(),
                    solver.verification))
    return std::nullopt;
  return layout;
}

// Cuts with the fewest blocks in the removed slice first, each repaired near
// the seam first and in the whole circuit last.
ShrinkNode make_node(Spec spec, World layout, const ShrinkOptions &options) {
  struct Cut {
    Spec spec;
    int axis;
    int index;
    int blocks;
  };
  std::vector<Cut> cuts;
  for (int axis = 0; axis < 3; ++axis)
    for (int k = 0; k < component(spec.size, axis); ++k)
      if (auto cut = cut_slice(spec, axis, k)) {
        int blocks = 0;
        for (const auto &[p, block] : spec.hints)
          blocks += coordinate(p, axis) == k ? 1 : 0;
        cuts.push_back({std::move(*cut), axis, k, blocks});
      }
  std::stable_sort(cuts.begin(), cuts.end(), [](const Cut &a, const Cut &b) {
    return a.blocks < b.blocks;
  });
  std::vector<Attempt> attempts;
  for (const Phase &phase : kPhases)
    for (const Cut &cut : cuts) {
      std::string name = std::format("{} = {} ({})", kAxisNames[cut.axis],
                                     cut.index, phase.name);
      double seconds = phase.half_width == 0
                           ? options.cut_seconds
                           : std::min(options.cut_seconds, phase.seconds);
      if (phase.half_width == 0) {
        attempts.push_back(
            {cut.spec, cut.spec, std::move(name), seconds, true});
        continue;
      }
      if (auto local =
              near_seam(cut.spec, cut.axis, cut.index, phase.half_width))
        attempts.push_back(
            {std::move(*local), cut.spec, std::move(name), seconds});
    }
  std::vector<bool> finished(attempts.size(), false);
  return {std::move(spec), std::move(layout), std::move(attempts),
          std::move(finished)};
}

} // namespace

std::optional<Spec> cut_slice(const Spec &spec, int axis, int index) {
  if (axis < 0 || axis > 2)
    throw std::invalid_argument("axis must be 0, 1 or 2");
  Spec cut = spec;
  int &extent = component(cut.size, axis);
  if (index < 0 || index >= extent || extent <= 1)
    return std::nullopt;
  --extent;
  for (InputPort &port : cut.inputs)
    if (!shift_port(port, axis, index))
      return std::nullopt;
  for (OutputPort &port : cut.outputs)
    if (!shift_port(port, axis, index))
      return std::nullopt;
  shift_blocks(cut.fixed, axis, index);
  shift_blocks(cut.hints, axis, index);
  std::vector<Vec3> forbidden;
  for (Vec3 p : cut.forbidden)
    if (auto q = shifted(p, axis, index))
      forbidden.push_back(*q);
  cut.forbidden = std::move(forbidden);
  return cut;
}

Spec spec_with_layout(const Spec &spec, const World &layout) {
  Spec result = spec;
  result.hints.clear();
  for (std::size_t i = 0; i < result.inputs.size(); ++i) {
    std::optional<Vec3> p = layout.find_input(static_cast<int>(i));
    if (!p)
      throw std::invalid_argument("the layout has no IN" +
                                  std::to_string(i + 1));
    result.inputs[i].position = *p;
    result.inputs[i].facing = layout.at(*p).facing;
    result.inputs[i].region.clear();
  }
  for (std::size_t i = 0; i < result.outputs.size(); ++i) {
    std::optional<Vec3> p = layout.find_output(static_cast<int>(i));
    if (!p)
      throw std::invalid_argument("the layout has no OUT" +
                                  std::to_string(i + 1));
    result.outputs[i].position = *p;
    result.outputs[i].facing = layout.at(*p).facing;
    result.outputs[i].region.clear();
  }
  std::vector<Vec3> kept;
  for (const auto &[p, block] : spec.fixed)
    kept.push_back(p);
  for (int i = 0; i < layout.volume(); ++i) {
    Vec3 p = layout.position_of(i);
    const Block &block = layout.at(p);
    if (block.type == BlockType::Air || block.type == BlockType::Input ||
        block.type == BlockType::Output ||
        std::find(kept.begin(), kept.end(), p) != kept.end())
      continue;
    result.hints.emplace_back(p, block);
  }
  return result;
}

int layout_blocks(const World &layout) {
  int blocks = 0;
  for (int i = 0; i < layout.volume(); ++i) {
    BlockType type = layout.at(layout.position_of(i)).type;
    blocks += type != BlockType::Air && type != BlockType::Input &&
                      type != BlockType::Output
                  ? 1
                  : 0;
  }
  return blocks;
}

ShrinkResult tidy(const Spec &spec, const World &layout,
                  const ShrinkOptions &options,
                  const ProgressCallback &progress) {
  const Clock::time_point deadline =
      Clock::now() +
      std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto out_of_time = [&] {
    return (options.cancel && options.cancel->load()) ||
           Clock::now() >= deadline;
  };

  ShrinkResult result{spec_with_layout(spec, layout), layout, 0};
  int blocks = layout_blocks(layout);
  const Vec3 size = layout.size();
  while (!out_of_time()) {
    // Windows over the whole height, the busiest first.
    struct Window {
      Vec3 low;
      int blocks;
    };
    std::vector<Window> windows;
    const int wx = std::min(kTidyWindow, size.x);
    const int wz = std::min(kTidyWindow, size.z);
    for (int x = 0; x + wx <= size.x; x += kTidyStride)
      for (int z = 0; z + wz <= size.z; z += kTidyStride) {
        int inside = 0;
        for (int y = 0; y < size.y; ++y)
          for (int dx = 0; dx < wx; ++dx)
            for (int dz = 0; dz < wz; ++dz) {
              BlockType type = result.layout.at({x + dx, y, z + dz}).type;
              inside += type != BlockType::Air && type != BlockType::Input &&
                                type != BlockType::Output
                            ? 1
                            : 0;
            }
        if (inside > 0)
          windows.push_back({{x, 0, z}, inside});
      }
    std::stable_sort(
        windows.begin(), windows.end(),
        [](const Window &a, const Window &b) { return a.blocks > b.blocks; });

    const Spec base = result.spec;
    std::atomic<std::size_t> next = 0;
    std::mutex mutex;
    std::optional<World> better;
    int better_blocks = blocks;
    auto worker = [&] {
      for (std::size_t k = next++; k < windows.size() && !out_of_time();
           k = next++) {
        const Vec3 low = windows[k].low;
        auto inside = [&](Vec3 p) {
          return p.x >= low.x && p.x < low.x + wx && p.z >= low.z &&
                 p.z < low.z + wz;
        };
        Spec window = base;
        window.hints.clear();
        World start = base.base_world();
        for (int i = 0; i < result.layout.volume(); ++i) {
          Vec3 p = result.layout.position_of(i);
          if (start.at(p).type != BlockType::Air)
            continue; // a port or a kept block
          const Block &block = result.layout.at(p);
          if (inside(p)) {
            if (block.type != BlockType::Air)
              window.hints.emplace_back(p, block);
          } else if (block.type != BlockType::Air) {
            window.fixed.emplace_back(p, block);
          } else {
            window.forbidden.push_back(p);
          }
        }
        double left =
            std::chrono::duration<double>(deadline - Clock::now()).count();
        if (left <= 0)
          return;
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = std::min(kTidyWindowSeconds, left);
        solver.stop_at_first = false;
        solver.cancel = options.cancel;
        SolveReport report = solve(window, solver);
        if (!report.solution)
          continue;
        int found = layout_blocks(report.solution->layout);
        std::lock_guard lock(mutex);
        if (found < better_blocks) {
          better_blocks = found;
          better = report.solution->layout;
        }
      }
    };
    std::vector<std::future<void>> workers;
    for (int w = 0; w < shrink_workers(options); ++w)
      workers.push_back(std::async(std::launch::async, worker));
    for (auto &w : workers)
      w.wait();
    if (!better) {
      say(result.steps == 0 ? "tidy: no window gets by with fewer blocks"
                            : "tidy: done");
      break;
    }
    say(std::format("tidy: {} blocks instead of {}", better_blocks, blocks));
    if (options.preview)
      options.preview(*better);
    blocks = better_blocks;
    result.spec = spec_with_layout(spec, *better);
    result.layout = std::move(*better);
    ++result.steps;
  }
  return result;
}

std::optional<World> layout_variant(const Spec &spec, const World &layout,
                                    double keep, unsigned seed, double seconds,
                                    const SolverOptions &solver) {
  Spec variant = spec_with_layout(spec, layout);
  std::vector<std::pair<Vec3, Block>> blocks = std::move(variant.hints);
  variant.hints.clear();
  std::mt19937 rng(seed);
  std::shuffle(blocks.begin(), blocks.end(), rng);
  const auto kept = static_cast<std::size_t>(keep * blocks.size());
  for (std::size_t i = 0; i < kept; ++i)
    variant.fixed.push_back(blocks[i]);
  SolverOptions o = solver;
  o.time_limit_seconds = seconds;
  o.stop_at_first = true;
  o.sat_seed = static_cast<int>(seed % 1000);
  SolveReport report = solve(variant, o);
  if (!report.solution)
    return std::nullopt;
  const World &found = report.solution->layout;
  for (int i = 0; i < found.volume(); ++i) {
    const Vec3 p = found.position_of(i);
    if (!(found.at(p) == layout.at(p)))
      return found;
  }
  return std::nullopt;
}

int shrink_workers(const ShrinkOptions &options) {
  if (options.parallel > 0)
    return options.parallel;
  constexpr int kMinWorkers = 3;
  return std::max(kMinWorkers,
                  static_cast<int>(std::thread::hardware_concurrency() / 2));
}

ShrinkResult shrink(const Spec &spec, const World &layout,
                    const ShrinkOptions &options,
                    const ProgressCallback &progress) {
  const Clock::time_point deadline =
      Clock::now() +
      std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  auto out_of_time = [&] { return cancelled() || Clock::now() >= deadline; };
  // Diversifying stops here, so tidying gets the rest.
  const Clock::time_point diversify_until =
      deadline -
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
          options.time_limit_seconds * kTidyShare));

  // A depth-first search: a dead end goes back to the box before it and
  // tries the cuts there that were not tried or not finished.
  std::vector<ShrinkNode> path;
  path.push_back(make_node(spec_with_layout(spec, layout), layout, options));
  ShrinkResult best{path.back().spec, layout, 0};
  int backtracks = 0;
  int variants = 0;
  unsigned variant_seed = options.seed;
  // Another circuit in the smallest box so far, to cut instead.
  // The circuit the last variant was built from: a random walk among the
  // circuits of the smallest box, back to the best one when it stalls.
  std::optional<std::pair<Spec, World>> walk;
  int stalled = 0;
  auto diversify = [&]() -> bool {
    if (!options.diversify)
      return false;
    while (Clock::now() < diversify_until && !cancelled()) {
      if (walk &&
          (walk->first.size != best.spec.size || stalled >= kVariantTries)) {
        walk.reset();
        stalled = 0;
      }
      const Spec &from_spec = walk ? walk->first : best.spec;
      const World &from_layout = walk ? walk->second : best.layout;
      const double left =
          std::chrono::duration<double>(diversify_until - Clock::now()).count();
      SolverOptions solver = options.solver;
      solver.cancel = options.cancel;
      ++variant_seed;
      const double keep = std::clamp(
          options.diversify_keep + kKeepSteps[variant_seed % kKeepSteps.size()],
          0.0, 1.0);
      std::optional<World> variant =
          layout_variant(from_spec, from_layout, keep, variant_seed,
                         std::min(kVariantSeconds, left), solver);
      if (!variant) {
        ++stalled;
        continue;
      }
      stalled = 0;
      ++variants;
      say(std::format("no cut works; trying another {}x{}x{} circuit ({}, "
                      "{} blocks)",
                      best.spec.size.x, best.spec.size.y, best.spec.size.z,
                      variants, layout_blocks(*variant)));
      path.clear();
      // Built first: the move below must not happen before it reads.
      Spec variant_spec = spec_with_layout(best.spec, *variant);
      // Same box, fewer blocks: the better answer even if no cut works.
      if (layout_blocks(*variant) < layout_blocks(best.layout)) {
        best.spec = variant_spec;
        best.layout = *variant;
        if (options.preview)
          options.preview(best.layout);
      }
      walk.emplace(variant_spec, *variant);
      path.push_back(
          make_node(std::move(variant_spec), std::move(*variant), options));
      backtracks = 0;
      return true;
    }
    return false;
  };
  while (!path.empty() && !out_of_time()) {
    ShrinkNode &node = path.back();
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < node.attempts.size(); ++i)
      if (!node.finished[i])
        pending.push_back(i);
    say(std::format("{}x{}x{}: trying {} cuts", node.spec.size.x,
                    node.spec.size.y, node.spec.size.z, pending.size()));

    // Workers take cuts in turn; the first verified one stops the rest.
    std::atomic<bool> stop = false;
    std::atomic<std::size_t> next = 0;
    std::mutex mutex;
    std::optional<std::pair<Spec, World>> found; // the cut box and its circuit
    auto run_workers = [&](const std::function<void()> &work) {
      std::vector<std::future<void>> workers;
      for (int w = 0; w < shrink_workers(options); ++w)
        workers.push_back(std::async(std::launch::async, work));
      for (auto &w : workers)
        while (w.wait_for(kCancelPollInterval) != std::future_status::ready)
          if (out_of_time())
            stop = true;
    };

    // First every plain cut exactly as it is: a simulation each, no solve.
    // Two neighbouring slices at once come first; each such cut gains two.
    if (!node.tried_as_is) {
      node.tried_as_is = true;
      struct AsIs {
        Spec base;
        std::string name;
        std::optional<std::size_t> attempt;
      };
      std::vector<AsIs> checks;
      for (int axis = 0; axis < 3; ++axis)
        for (int k = 0; k + 1 < component(node.spec.size, axis); ++k)
          if (std::optional<Spec> once = cut_slice(node.spec, axis, k))
            if (std::optional<Spec> twice = cut_slice(*once, axis, k))
              checks.push_back(
                  {std::move(*twice),
                   std::format("{} = {} and {}", kAxisNames[axis], k, k + 1),
                   std::nullopt});
      for (std::size_t i : pending)
        if (node.attempts[i].whole)
          checks.push_back({node.attempts[i].base, node.attempts[i].name, i});
      run_workers([&] {
        for (std::size_t k = next++; k < checks.size() && !stop; k = next++) {
          std::optional<World> layout =
              works_as_is(checks[k].base, options.solver);
          if (!layout)
            continue;
          std::lock_guard lock(mutex);
          if (!found) {
            found.emplace(checks[k].base, std::move(*layout));
            if (checks[k].attempt)
              node.finished[*checks[k].attempt] = true;
            say("removed " + checks[k].name + ", as it was");
            stop = true;
          }
        }
      });
      next = 0;
    }

    auto worker = [&] {
      for (std::size_t k = next++; k < pending.size() && !stop; k = next++) {
        std::size_t i = pending[k];
        double left =
            std::chrono::duration<double>(deadline - Clock::now()).count();
        if (left <= 0)
          return;
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = std::min(node.attempts[i].seconds, left);
        solver.stop_at_first = true;
        solver.cancel = &stop;
        SolveReport report = solve(node.attempts[i].spec, solver);
        std::lock_guard lock(mutex);
        if (!report.solution) {
          // Stopped early by another success: worth trying again later.
          if (!stop)
            node.finished[i] = true;
          continue;
        }
        node.finished[i] = true;
        if (!found) {
          found.emplace(node.attempts[i].base, report.solution->layout);
          say("removed " + node.attempts[i].name);
          stop = true;
        }
      }
    };
    if (!found && !out_of_time())
      run_workers(worker);

    if (found) {
      Spec next_spec = spec_with_layout(found->first, found->second);
      path.push_back(
          make_node(std::move(next_spec), std::move(found->second), options));
      const ShrinkNode &child = path.back();
      if (options.preview)
        options.preview(child.layout);
      if (cells(child.spec.size) < cells(best.spec.size))
        best = {child.spec, child.layout, static_cast<int>(path.size()) - 1};
      continue;
    }
    if (out_of_time())
      break;
    path.pop_back();
    if ((path.empty() || backtracks >= kMaxBacktracks) && diversify())
      continue;
    if (path.empty() || backtracks >= kMaxBacktracks) {
      say("no cut works");
      double left =
          std::chrono::duration<double>(deadline - Clock::now()).count();
      if (left >= kTidyMinSeconds) {
        ShrinkOptions rest = options;
        rest.time_limit_seconds = left;
        ShrinkResult tidied = tidy(best.spec, best.layout, rest, progress);
        best.spec = std::move(tidied.spec);
        best.layout = std::move(tidied.layout);
      }
      break;
    }
    ++backtracks;
    say(std::format("dead end; back to {}x{}x{} for the other cuts",
                    path.back().spec.size.x, path.back().spec.size.y,
                    path.back().spec.size.z));
  }
  if (cancelled())
    say("cancelled");
  else if (Clock::now() >= deadline)
    say("out of time");
  return best;
}

} // namespace rs

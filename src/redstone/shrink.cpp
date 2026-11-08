#include "redstone/shrink.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <future>
#include <mutex>
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

  ShrinkResult result{spec_with_layout(spec, layout), layout, 0};
  while (!cancelled() && Clock::now() < deadline) {
    // Cuts with the fewest blocks in the removed slice first.
    struct Cut {
      Spec spec;
      int axis;
      int index;
      int blocks;
    };
    std::vector<Cut> cuts;
    for (int axis = 0; axis < 3; ++axis)
      for (int k = 0; k < component(result.spec.size, axis); ++k)
        if (auto cut = cut_slice(result.spec, axis, k)) {
          int blocks = 0;
          for (const auto &[p, block] : result.spec.hints)
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
          attempts.push_back({cut.spec, cut.spec, std::move(name), seconds});
          continue;
        }
        if (auto local =
                near_seam(cut.spec, cut.axis, cut.index, phase.half_width))
          attempts.push_back(
              {std::move(*local), cut.spec, std::move(name), seconds});
      }
    say(std::format("{}x{}x{}: trying {} cuts", result.spec.size.x,
                    result.spec.size.y, result.spec.size.z, cuts.size()));

    // Workers take cuts in turn; the first verified one stops the rest.
    std::atomic<bool> stop = false;
    std::atomic<std::size_t> next = 0;
    std::mutex mutex;
    std::optional<std::pair<Spec, World>> found;
    auto worker = [&] {
      for (std::size_t i = next++; i < attempts.size() && !stop; i = next++) {
        double left =
            std::chrono::duration<double>(deadline - Clock::now()).count();
        if (left <= 0)
          return;
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = std::min(attempts[i].seconds, left);
        solver.stop_at_first = true;
        solver.cancel = &stop;
        SolveReport report = solve(attempts[i].spec, solver);
        if (!report.solution)
          continue;
        std::lock_guard lock(mutex);
        if (!found) {
          found.emplace(attempts[i].base, report.solution->layout);
          say("removed " + attempts[i].name);
          stop = true;
        }
      }
    };
    std::vector<std::future<void>> workers;
    for (int w = 0; w < shrink_workers(options); ++w)
      workers.push_back(std::async(std::launch::async, worker));
    for (auto &w : workers)
      while (w.wait_for(kCancelPollInterval) != std::future_status::ready)
        if (cancelled() || Clock::now() >= deadline)
          stop = true;

    if (!found) {
      say(cancelled()                ? "cancelled"
          : Clock::now() >= deadline ? "out of time"
                                     : "no cut works");
      break;
    }
    result.spec = spec_with_layout(found->first, found->second);
    result.layout = std::move(found->second);
    ++result.steps;
  }
  return result;
}

} // namespace rs

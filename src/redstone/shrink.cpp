#include "redstone/shrink.hpp"

#include <algorithm>
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
  Spec spec;
  std::string name;
};

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
    std::vector<Attempt> attempts;
    for (int axis = 0; axis < 3; ++axis)
      for (int k = 0; k < component(result.spec.size, axis); ++k)
        if (auto cut = cut_slice(result.spec, axis, k))
          attempts.push_back(
              {std::move(*cut), std::format("{} = {}", kAxisNames[axis], k)});
    say(std::format("{}x{}x{}: trying {} cuts", result.spec.size.x,
                    result.spec.size.y, result.spec.size.z, attempts.size()));

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
        solver.time_limit_seconds = std::min(options.cut_seconds, left);
        solver.stop_at_first = true;
        solver.cancel = &stop;
        SolveReport report = solve(attempts[i].spec, solver);
        if (!report.solution)
          continue;
        std::lock_guard lock(mutex);
        if (!found) {
          found.emplace(attempts[i].spec, report.solution->layout);
          say("removed " + attempts[i].name);
          stop = true;
        }
      }
    };
    std::vector<std::future<void>> workers;
    for (int w = 0; w < std::max(1, options.parallel); ++w)
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

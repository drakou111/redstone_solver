#include "redstone/size_search.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "redstone/shrink.hpp"

namespace rs {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds kPollInterval{50};
constexpr int kMinHeight = 2; // ports stand on a block
constexpr int kMinLength = 2; // a west and an east side
// Boxes may be this much longer or wider than the circuit to beat.
constexpr int kSlack = 2;
constexpr int kMinWorkers = 2;
constexpr int kThreadsPerSearch = 4; // the search and its quick twin, roughly

int cells(Vec3 size) { return size.x * size.y * size.z; }

enum class SizeState { Open, Found, Impossible };

struct Candidate {
  Vec3 size;
  Spec spec;
  SizeState state = SizeState::Open;
};

} // namespace

std::optional<Spec> spec_for_size(const Spec &spec, Vec3 size) {
  if (size.y < kMinHeight || size.x < kMinLength || size.z < 1)
    return std::nullopt;
  std::vector<Vec3> west;
  std::vector<Vec3> east;
  for (int y = 1; y < size.y; ++y)
    for (int z = 0; z < size.z; ++z) {
      west.push_back({0, y, z});
      east.push_back({size.x - 1, y, z});
    }
  if (west.size() < spec.inputs.size() || east.size() < spec.outputs.size())
    return std::nullopt;

  Spec result = spec;
  result.size = size;
  result.hints.clear();
  result.fixed.clear();
  result.forbidden.clear();
  for (InputPort &port : result.inputs) {
    port.region = west;
    port.position = west.front();
    port.facing = Dir::East;
  }
  for (OutputPort &port : result.outputs) {
    port.region = east;
    port.position = east.front();
    port.facing = Dir::East;
  }
  return result;
}

std::optional<SizeSearchResult> search_sizes(const Spec &spec, Vec3 beat,
                                             const SizeSearchOptions &options,
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

  const int max_height = options.max_height > 0 ? options.max_height : beat.y;
  std::vector<Candidate> candidates;
  for (int y = kMinHeight; y <= max_height; ++y)
    for (int x = kMinLength; x <= beat.x + kSlack; ++x)
      for (int z = 1; z <= beat.z + kSlack; ++z) {
        Vec3 size{x, y, z};
        if (cells(size) >= cells(beat))
          continue;
        if (std::optional<Spec> s = spec_for_size(spec, size))
          candidates.push_back({size, std::move(*s)});
      }
  // Just below the circuit to beat first: those boxes usually hold a
  // circuit and are found quickly, while the smallest ones take longest to
  // rule out. Each find lowers the bar for the rest. Of equal volume, the
  // squarer floor first.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) {
                     if (cells(a.size) != cells(b.size))
                       return cells(a.size) > cells(b.size);
                     return std::abs(a.size.x - a.size.z) <
                            std::abs(b.size.x - b.size.z);
                   });
  if (candidates.empty())
    return std::nullopt;
  say(std::format("sizes: trying {} boxes smaller than {}x{}x{}",
                  candidates.size(), beat.x, beat.y, beat.z));

  const int workers =
      options.parallel > 0
          ? options.parallel
          : std::max(kMinWorkers,
                     static_cast<int>(std::thread::hardware_concurrency()) /
                         kThreadsPerSearch);
  std::mutex mutex;
  std::optional<SizeSearchResult> best;
  std::atomic<int> best_cells = cells(beat);
  // Each worker's running search: its box's volume and its stop flag.
  std::vector<std::atomic<int>> running(workers);
  std::vector<std::unique_ptr<std::atomic<bool>>> stops;
  for (int w = 0; w < workers; ++w) {
    running[w] = std::numeric_limits<int>::max();
    stops.push_back(std::make_unique<std::atomic<bool>>(false));
  }

  // The box to beat: the best found here or elsewhere.
  auto limit = [&] {
    int here = best_cells;
    return options.bound ? std::min(here, options.bound->load()) : here;
  };

  double try_seconds = options.first_try_seconds;
  for (int round = 1; !out_of_time(); ++round) {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < candidates.size(); ++i)
      if (candidates[i].state == SizeState::Open &&
          cells(candidates[i].size) < limit())
        open.push_back(i);
    if (open.empty())
      break;
    if (round > 1)
      say(std::format("sizes: round {}: {} boxes, {:.0f} s each", round,
                      open.size(), try_seconds));

    std::atomic<std::size_t> next = 0;
    auto worker = [&](int w) {
      for (std::size_t k = next++; k < open.size() && !out_of_time();
           k = next++) {
        Candidate &candidate = candidates[open[k]];
        const int volume = cells(candidate.size);
        if (volume >= limit())
          continue;
        double left =
            std::chrono::duration<double>(deadline - Clock::now()).count();
        if (left <= 0)
          return;
        running[w] = volume;
        *stops[w] = false;
        SolverOptions solver = options.solver;
        solver.time_limit_seconds = std::min(try_seconds, left);
        solver.stop_at_first = true;
        solver.cancel = stops[w].get();
        solver.preview = {};
        SolveReport report = solve(candidate.spec, solver);
        running[w] = std::numeric_limits<int>::max();
        std::lock_guard lock(mutex);
        if (report.status == SolveStatus::Infeasible) {
          candidate.state = SizeState::Impossible;
          continue;
        }
        if (!report.solution)
          continue; // out of time or stopped: open for a longer try
        candidate.state = SizeState::Found;
        if (volume >= best_cells)
          continue;
        best_cells = volume;
        const World &layout = report.solution->layout;
        best =
            SizeSearchResult{spec_with_layout(candidate.spec, layout), layout};
        say(std::format("sizes: found a {}x{}x{} circuit ({} blocks)",
                        candidate.size.x, candidate.size.y, candidate.size.z,
                        report.solution->block_count));
        if (options.preview)
          options.preview(layout);
      }
    };
    std::vector<std::future<void>> threads;
    for (int w = 0; w < workers; ++w)
      threads.push_back(std::async(std::launch::async, worker, w));
    // Searches in boxes no smaller than the best found stop at once.
    for (auto &thread : threads)
      while (thread.wait_for(kPollInterval) != std::future_status::ready)
        for (int w = 0; w < workers; ++w) {
          const int volume = running[w];
          if (out_of_time() ||
              (volume != std::numeric_limits<int>::max() && volume >= limit()))
            *stops[w] = true;
        }
    try_seconds *= 2;
  }
  return best;
}

} // namespace rs

// Runs the solver on every spec in a folder and records how long it takes.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "redstone/encoder.hpp"
#include "redstone/compose.hpp"
#include "redstone/solver.hpp"
#include "redstone/spec.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

constexpr std::string_view kUsage =
    "redstone_bench - times the solver on a folder of specs.\n"
    "\n"
    "Usage: redstone_bench [options]\n"
    "  --specs <dir>          spec files to run (default: bench)\n"
    "  --time-limit <s>       limit per solve (default 45)\n"
    "  --mode first|best|both what to time (default both)\n"
    "  --compose              benchmark gate-network composition instead\n"
    "  --max-height <n>       limit Compose to this height (0: unlimited)\n"
    "  --extra-layers <n>     spare routing layers above gate modules\n"
    "  --channel-width <n>    first Compose channel width to try\n"
    "  --max-channel-width <n> last Compose channel width to try\n"
    "  --narrow-seconds <s>   cap for narrow-channel attempts\n"
    "  --progress             print solver/Compose progress while running\n"
    "  --one-placement        verify only at the identity placement\n"
    "  --shrink-time <s>      shrink composed layouts for this long\n"
    "  --out <dir>            where results go (default: output/bench)\n"
    "  --label <text>         name stored with the results\n"
    "  --seeds <n>            runs per spec with SAT seeds 0..n-1 (default 1)\n"
    "  --no-quick-palette     skip the simultaneous basic-block search\n"
    "  --initial-cases <n>    settled states encoded from the start (0: all)\n"
    "  --extra-searches <n>   more complete searches per solve, other seeds\n"
    "  --lazy-loops           forbid signal loops as the search makes them\n"
    "  --jobs <n>             solves run in parallel (default 1); compare\n"
    "                         conflicts, since times then depend on load\n"
    "  --profile <spec>       print the model size per part (settled states\n"
    "                         and one input change) instead of solving\n"
    "  --help                 show this help\n";

struct Options {
  std::filesystem::path specs = "bench";
  std::filesystem::path out = "output/bench";
  double time_limit = 45.0;
  bool first = true;
  bool best = true;
  bool compose = false;
  bool progress = false;
  bool one_placement = false;
  bool quick_palette_search = true;
  int initial_cases = 0;
  int extra_searches = 0;
  bool lazy_loops = false;
  int max_height = 0;
  int extra_layers = 1;
  int channel_width = 1;
  int max_channel_width = 6;
  double narrow_seconds = 20.0;
  double shrink_time = 0.0;
  std::string label = "unnamed";
  rs::SolverEngine engine = rs::SolverEngine::Sat;
  int sat_workers = 1;
  int seeds = 1;
  int jobs = 1;
  std::filesystem::path profile;
};

struct Row {
  std::string spec;
  std::string mode;
  std::string status;
  double seconds = 0;
  int blocks = -1;
  int x = 0;
  int y = 0;
  int z = 0;
  int rounds = 0;
  long long conflicts = 0;
  int seed = 0;
  bool solved = false;
};

int positive_int(std::string_view text, std::string_view name) {
  int value = 0;
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || value < 1)
    throw std::invalid_argument(
        std::format("{} expects a positive number", name));
  return value;
}

Options parse(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto value = [&]() -> std::string_view {
      if (i + 1 >= argc)
        throw std::invalid_argument(std::format("{} needs a value", arg));
      return argv[++i];
    };
    if (arg == "--specs") {
      o.specs = value();
    } else if (arg == "--out") {
      o.out = value();
    } else if (arg == "--label") {
      o.label = value();
    } else if (arg == "--time-limit") {
      std::string_view v = value();
      auto [end, error] =
          std::from_chars(v.data(), v.data() + v.size(), o.time_limit);
      if (error != std::errc() || o.time_limit <= 0)
        throw std::invalid_argument("--time-limit expects a positive number");
    } else if (arg == "--sat-workers") {
      o.sat_workers = positive_int(value(), arg);
    } else if (arg == "--seeds") {
      o.seeds = positive_int(value(), arg);
    } else if (arg == "--jobs") {
      o.jobs = positive_int(value(), arg);
    } else if (arg == "--profile") {
      o.profile = value();
    } else if (arg == "--compose") {
      o.compose = true;
    } else if (arg == "--progress") {
      o.progress = true;
    } else if (arg == "--one-placement") {
      o.one_placement = true;
    } else if (arg == "--no-quick-palette") {
      o.quick_palette_search = false;
    } else if (arg == "--initial-cases") {
      o.initial_cases = positive_int(value(), arg);
    } else if (arg == "--lazy-loops") {
      o.lazy_loops = true;
    } else if (arg == "--extra-searches") {
      o.extra_searches = positive_int(value(), arg);
    } else if (arg == "--max-height") {
      std::string_view v = value();
      auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), o.max_height);
      if (error != std::errc() || end != v.data() + v.size() || o.max_height < 0)
        throw std::invalid_argument("--max-height expects a nonnegative integer");
    } else if (arg == "--extra-layers") {
      std::string_view v = value();
      auto [end, error] =
          std::from_chars(v.data(), v.data() + v.size(), o.extra_layers);
      if (error != std::errc() || end != v.data() + v.size() ||
          o.extra_layers < 0)
        throw std::invalid_argument(
            "--extra-layers expects a nonnegative integer");
    } else if (arg == "--channel-width") {
      o.channel_width = positive_int(value(), arg);
    } else if (arg == "--max-channel-width") {
      o.max_channel_width = positive_int(value(), arg);
    } else if (arg == "--narrow-seconds") {
      std::string_view v = value();
      auto [end, error] =
          std::from_chars(v.data(), v.data() + v.size(), o.narrow_seconds);
      if (error != std::errc() || end != v.data() + v.size() ||
          o.narrow_seconds <= 0)
        throw std::invalid_argument(
            "--narrow-seconds expects a positive number");
    } else if (arg == "--shrink-time") {
      std::string_view v = value();
      auto [end, error] =
          std::from_chars(v.data(), v.data() + v.size(), o.shrink_time);
      if (error != std::errc() || o.shrink_time < 0)
        throw std::invalid_argument("--shrink-time expects a nonnegative number");
    } else if (arg == "--engine") {
      std::string_view v = value();
      if (v != "sat" && v != "cpsat")
        throw std::invalid_argument("--engine is sat or cpsat");
      o.engine = v == "sat" ? rs::SolverEngine::Sat : rs::SolverEngine::CpSat;
    } else if (arg == "--mode") {
      std::string_view v = value();
      if (v != "first" && v != "best" && v != "both")
        throw std::invalid_argument("--mode is first, best or both");
      o.first = v != "best";
      o.best = v != "first";
    } else {
      throw std::invalid_argument(std::format("unknown argument {}", arg));
    }
  }
  if (o.max_channel_width < o.channel_width)
    throw std::invalid_argument(
        "--max-channel-width must be at least --channel-width");
  return o;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open " + path.string());
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

Row run(const std::filesystem::path &path, bool first, int seed,
        const Options &o) {
  rs::Spec spec = rs::parse_spec(read_file(path));
  if (o.compose) {
    rs::NetlistComposeOptions options;
    rs::GateCache cache;
    options.gate_cache = &cache;
    options.stage_seconds = o.time_limit;
    options.narrow_seconds = std::min(o.narrow_seconds, o.time_limit);
    options.channel_width = o.channel_width;
    options.max_channel_width = o.max_channel_width;
    options.gates.time_limit_seconds = o.time_limit;
    options.flush_seconds = o.time_limit;
    options.max_height = o.max_height;
    options.extra_layers = o.extra_layers;
    options.solver.sat_seed = seed;
    if (o.one_placement)
      options.solver.verification.placements = {rs::Placement{}};
    auto say = [&](std::string_view message) {
      if (o.progress)
        std::clog << '[' << path.stem().string() << "] " << message << '\n';
    };
    auto start = Clock::now();
    std::optional<rs::ComposeResult> result;
    if (o.shrink_time > 0) {
      rs::ShrinkOptions shrink;
      shrink.time_limit_seconds = o.shrink_time;
      shrink.cut_seconds = std::min(o.time_limit, o.shrink_time);
      result = rs::compose_and_shrink(spec, options, shrink, say);
    } else {
      result = rs::compose_spec(spec, options, say);
    }
    double seconds = seconds_since(start);
    Row row{.spec = path.stem().string(),
            .mode = o.shrink_time > 0 ? "compose-shrink" : "compose",
            .status = result ? "feasible" : "out of time",
            .seconds = seconds,
            .blocks =
                result ? static_cast<int>(result->spec.hints.size()) : -1,
            .seed = seed,
            .solved = result.has_value()};
    if (result) {
      rs::Vec3 size = result->layout.size();
      row.x = size.x;
      row.y = size.y;
      row.z = size.z;
    }
    return row;
  }
  rs::SolverOptions options;
  options.time_limit_seconds = o.time_limit;
  options.stop_at_first = first;
  options.engine = o.engine;
  options.max_sat_workers = o.sat_workers;
  options.sat_seed = seed;
  options.quick_palette_search = o.quick_palette_search;
  options.initial_cases = o.initial_cases;
  options.extra_searches = o.extra_searches;
  options.lazy_loops = o.lazy_loops;
  if (o.one_placement)
    options.verification.placements = {rs::Placement{}};
  auto say = [&](std::string_view message) {
    if (o.progress)
      std::clog << '[' << path.stem().string() << "] " << message << '\n';
  };
  rs::SolveReport report = rs::solve(spec, options, say);
  Row row{path.stem().string(), first ? "first" : "best",
          std::string(rs::to_string(report.status)), report.seconds};
  if (report.solution)
    row.blocks = report.solution->block_count;
  if (report.solution) {
    rs::Vec3 size = report.solution->layout.size();
    row.x = size.x;
    row.y = size.y;
    row.z = size.z;
  }
  row.rounds = report.refinements;
  row.conflicts = report.sat_conflicts;
  row.seed = seed;
  row.solved = report.solution.has_value();
  return row;
}

// The model the SAT engine starts from, plus one input change, by part.
void print_profile(const std::filesystem::path &path) {
  rs::Spec spec = rs::parse_spec(read_file(path));
  rs::ModelPlan plan;
  for (const rs::InputVector &inputs : spec.representative_vectors())
    plan.steady.push_back({inputs, spec.expected_outputs(inputs)});
  if (plan.steady.size() >= 2)
    plan.transitions.push_back({0, static_cast<int>(plan.steady.size()) - 1});
  rs::CircuitEncoder encoder(
      spec, plan, {.minimize_blocks = false, .build_cp_model = false});

  long long variables = 0;
  long long clauses = 0;
  long long literals = 0;
  std::vector<rs::ModelPart> parts = encoder.size_profile();
  for (const rs::ModelPart &part : parts) {
    variables += part.variables;
    clauses += part.clauses;
    literals += part.literals;
  }
  std::cout << std::format(
      "{}: {} settled states, 1 input change over {} ticks\n",
      path.filename().string(), plan.steady.size(), encoder.horizon());
  std::cout << std::format("{:<30} {:>10} {:>10} {:>11} {:>6}\n", "part",
                           "variables", "clauses", "literals", "share");
  for (const rs::ModelPart &part : parts)
    std::cout << std::format("{:<30} {:>10} {:>10} {:>11} {:>5.1f}%\n",
                             part.name, part.variables, part.clauses,
                             part.literals,
                             100.0 * static_cast<double>(part.literals) /
                                 static_cast<double>(literals));
  std::cout << std::format("{:<30} {:>10} {:>10} {:>11}\n", "total", variables,
                           clauses, literals);
}

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]) == "--help") {
      std::cout << kUsage;
      return 0;
    }
  try {
    Options o = parse(argc, argv);
    if (!o.profile.empty()) {
      print_profile(o.profile);
      return 0;
    }
    std::vector<std::filesystem::path> specs;
    for (const auto &entry : std::filesystem::directory_iterator(o.specs))
      if (entry.path().extension() == ".txt")
        specs.push_back(entry.path());
    std::sort(specs.begin(), specs.end());
    if (specs.empty())
      throw std::runtime_error("no .txt specs in " + o.specs.string());

    std::filesystem::create_directories(o.out);
    auto stamp = std::chrono::system_clock::now().time_since_epoch() /
                 std::chrono::seconds(1);
    std::filesystem::path csv =
        o.out / std::format("{}_{}.csv", stamp, o.label);
    std::ofstream file(csv);
    file << std::format(
        "# label={} engine={} sat_workers={} time_limit={} threads={}\n",
        o.label, o.engine == rs::SolverEngine::Sat ? "sat" : "cpsat",
        o.sat_workers, o.time_limit, std::thread::hardware_concurrency());
    file << "spec,mode,seed,status,seconds,blocks,x,y,z,rounds,conflicts\n";

    struct Task {
      std::filesystem::path path;
      bool first;
      int seed;
    };
    std::vector<Task> tasks;
    for (const auto &path : specs)
      for (bool first : {true, false})
        if (o.compose ? first : (first ? o.first : o.best))
          for (int seed = 0; seed < o.seeds; ++seed)
            tasks.push_back({path, first, seed});

    std::vector<Row> rows(tasks.size());
    std::atomic<std::size_t> next = 0;
    std::mutex output;
    auto work = [&] {
      for (std::size_t t = next++; t < tasks.size(); t = next++) {
        Row row = run(tasks[t].path, tasks[t].first, tasks[t].seed, o);
        std::lock_guard lock(output);
        rows[t] = row;
        file << std::format("{},{},{},{},{:.2f},{},{},{},{},{},{}\n", row.spec,
                            row.mode, row.seed, row.status, row.seconds,
                            row.blocks, row.x, row.y, row.z, row.rounds,
                            row.conflicts)
             << std::flush;
        std::cout << std::format("{:<12} {:<13} seed {:<2} {:>7.2f}s {:>5} "
                                 "blocks {:>2}x{}x{}  {}\n",
                                 row.spec, row.mode, row.seed, row.seconds,
                                 row.blocks, row.x, row.y, row.z, row.status)
                  << std::flush;
      }
    };
    {
      std::vector<std::jthread> threads;
      for (int j = 1; j < o.jobs; ++j)
        threads.emplace_back(work);
      work();
    }

    // Per spec and mode: how many seeds found a circuit, and the median
    // effort (an unsolved run counts as worse than any solved one).
    std::cout << "\nspec         mode   solved  median conflicts  median s\n";
    long long total_median = 0;
    int total_solved = 0;
    for (std::size_t t = 0; t < tasks.size(); t += o.seeds) {
      std::vector<Row> group(rows.begin() + t, rows.begin() + t + o.seeds);
      std::sort(group.begin(), group.end(), [](const Row &a, const Row &b) {
        if (a.solved != b.solved)
          return a.solved;
        return a.conflicts < b.conflicts;
      });
      const Row &median = group[group.size() / 2];
      int solved = 0;
      for (const Row &row : group)
        solved += row.solved ? 1 : 0;
      total_solved += solved;
      total_median += median.conflicts;
      std::cout << std::format("{:<12} {:<6} {:>2}/{:<4} {:>16} {:>9.2f}{}\n",
                               median.spec, median.mode, solved, o.seeds,
                               median.conflicts, median.seconds,
                               median.solved ? "" : "  (median unsolved)");
    }
    std::cout << std::format(
        "solved {}/{} runs, sum of median conflicts {}, results in {}\n",
        total_solved, tasks.size(), total_median, csv.string());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  }
}

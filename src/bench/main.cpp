// Runs the solver on every spec in a folder and records how long it takes.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"

namespace {

constexpr std::string_view kUsage =
    "redstone_bench - times the solver on a folder of specs.\n"
    "\n"
    "Usage: redstone_bench [options]\n"
    "  --specs <dir>          spec files to run (default: bench)\n"
    "  --time-limit <s>       limit per solve (default 45)\n"
    "  --mode first|best|both what to time (default both)\n"
    "  --out <dir>            where results go (default: output/bench)\n"
    "  --label <text>         name stored with the results\n"
    "  --help                 show this help\n";

struct Options {
  std::filesystem::path specs = "bench";
  std::filesystem::path out = "output/bench";
  double time_limit = 45.0;
  bool first = true;
  bool best = true;
  std::string label = "unnamed";
  rs::SolverEngine engine = rs::SolverEngine::Sat;
  int sat_workers = 1;
};

struct Row {
  std::string spec;
  std::string mode;
  std::string status;
  double seconds = 0;
  int blocks = -1;
  int rounds = 0;
};

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
      std::string_view v = value();
      auto [end, error] =
          std::from_chars(v.data(), v.data() + v.size(), o.sat_workers);
      if (error != std::errc() || o.sat_workers < 1)
        throw std::invalid_argument("--sat-workers expects a positive number");
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

Row run(const std::filesystem::path &path, bool first, const Options &o) {
  rs::Spec spec = rs::parse_spec(read_file(path));
  rs::SolverOptions options;
  options.time_limit_seconds = o.time_limit;
  options.stop_at_first = first;
  options.engine = o.engine;
  options.max_sat_workers = o.sat_workers;
  rs::SolveReport report = rs::solve(spec, options);
  Row row{path.stem().string(), first ? "first" : "best",
          std::string(rs::to_string(report.status)), report.seconds};
  if (report.solution)
    row.blocks = report.solution->block_count;
  row.rounds = report.refinements;
  return row;
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
    file << "spec,mode,status,seconds,blocks,rounds\n";

    double total = 0;
    for (const auto &path : specs) {
      for (bool first : {true, false}) {
        if ((first && !o.first) || (!first && !o.best))
          continue;
        Row row = run(path, first, o);
        total += row.seconds;
        std::string line =
            std::format("{},{},{},{:.2f},{},{}", row.spec, row.mode, row.status,
                        row.seconds, row.blocks, row.rounds);
        file << line << '\n';
        file.flush();
        std::cout << std::format("{:<12} {:<6} {:>7.2f}s  {:>3} blocks  {}\n",
                                 row.spec, row.mode, row.seconds, row.blocks,
                                 row.status)
                  << std::flush;
      }
    }
    std::cout << std::format("total {:.1f}s, results in {}\n", total,
                             csv.string());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  }
}

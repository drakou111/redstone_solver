#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "redstone/report.hpp"
#include "redstone/solver.hpp"
#include "redstone/spec.hpp"

namespace {

constexpr std::string_view kUsage =
    "redstone_solver - finds Minecraft redstone circuits that satisfy a truth "
    "table.\n"
    "\n"
    "Usage: redstone_solver <spec-file> [options]\n"
    "\n"
    "Options:\n"
    "  --time-limit <seconds>  total time budget (default 60)\n"
    "  --threads <n>           solver threads (default: all)\n"
    "  --first                 stop at the first verified circuit\n"
    "  --log-search            print CP-SAT's search log\n"
    "  --show-rejected         print layouts the simulator rejected\n"
    "  --help                  show this help\n"
    "\n"
    "Spec file lines (# starts a comment):\n"
    "  size <x> <y> <z>\n"
    "  input IN<n> <kind> <x> <y> <z> [facing]  kind: comparator | repeater "
    "(need a facing),\n"
    "                                           dust | container (barrel)\n"
    "  output OUT<n> <kind> <x> <y> <z> [facing] kind: dust | repeater | "
    "comparator\n"
    "                                           (repeater/comparator need a "
    "facing)\n"
    "  Repeater, comparator and dust ports need a block below, like in game.\n"
    "  timing none | <latest> | <earliest> <latest>  ticks after an input "
    "change; 'rt' suffix\n"
    "                                               for redstone ticks, "
    "default game ticks\n"
    "  hold_previous on|off   outputs keep their old value before <earliest>\n"
    "  tile none|<axes>       copies placed side by side along x/y/z must not "
    "interfere\n"
    "  feedback on|off       allow loops through components (memory, clocks); "
    "default off\n"
    "  settle <ticks>         extra ticks after <latest> to come to rest "
    "(default 4)\n"
    "  horizon <ticks>        time to come to rest when timing is none "
    "(default 20)\n"
    "  palette <blocks...>    solid glass dust torch redstone_block repeater "
    "comparator\n"
    "                         target note_block hopper barrel chest observer\n"
    "  row IN1(ss1..15),IN2(ss0) -> OUT1(ss1..15)\n"
    "\n"
    "Facings: north south east west up down. Coordinates: +x east, +y up, +z "
    "south.\n";

struct CliOptions {
  std::filesystem::path spec_path;
  rs::SolverOptions solver;
};

double parse_number(std::string_view text, std::string_view option) {
  double value = 0;
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || value <= 0)
    throw std::invalid_argument(
        std::format("{} expects a positive number, got '{}'", option, text));
  return value;
}

CliOptions parse_args(int argc, char **argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto value = [&]() -> std::string_view {
      if (i + 1 >= argc)
        throw std::invalid_argument(std::format("{} needs a value", arg));
      return argv[++i];
    };
    if (arg == "--time-limit")
      options.solver.time_limit_seconds = parse_number(value(), arg);
    else if (arg == "--threads")
      options.solver.threads = static_cast<int>(parse_number(value(), arg));
    else if (arg == "--log-search")
      options.solver.log_search = true;
    else if (arg == "--first")
      options.solver.stop_at_first = true;
    else if (arg == "--show-rejected")
      options.solver.show_rejected = true;
    else if (arg.starts_with("--"))
      throw std::invalid_argument(std::format("unknown option {}", arg));
    else if (options.spec_path.empty())
      options.spec_path = arg;
    else
      throw std::invalid_argument("only one spec file can be given");
  }
  if (options.spec_path.empty())
    throw std::invalid_argument("no spec file given");
  return options;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open " + path.string());
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

void print_traces(const rs::Spec &spec, const rs::EncodedSolution &solution) {
  std::cout << "Model output timelines (ticks after the input change):\n";
  for (const rs::TransitionTrace &trace : solution.traces) {
    std::cout << std::format(
        "  {} -> {}:",
        rs::to_string(solution.steady_inputs[trace.transition.from]),
        rs::to_string(solution.steady_inputs[trace.transition.to]));
    for (std::size_t o = 0; o < spec.outputs.size(); ++o) {
      std::cout << std::format(" OUT{}", o + 1);
      for (const std::vector<int> &tick : trace.outputs)
        std::cout << ' ' << tick[o];
    }
    std::cout << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return 0;
    }
  }

  try {
    CliOptions options = parse_args(argc, argv);
    rs::Spec spec = rs::parse_spec(read_file(options.spec_path));
    rs::SolveReport report =
        rs::solve(spec, options.solver, [](std::string_view message) {
          std::clog << "[solver] " << message << '\n';
        });

    std::cout << std::format("Status: {} after {:.1f}s, {} refinements\n",
                             rs::to_string(report.status), report.seconds,
                             report.refinements);
    if (!report.solution)
      return 1;
    std::cout << std::format("Blocks placed: {}\n\n",
                             report.solution->block_count);
    std::cout << rs::render_layers(report.solution->layout) << '\n';
    std::cout << rs::list_blocks(report.solution->layout) << '\n';
    print_traces(spec, *report.solution);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what()
              << "\n\nRun with --help for usage.\n";
    return 2;
  }
}

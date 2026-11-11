// redstone_evolve: the evolve search (redstone/evolve.hpp) from the command
// line, for long runs outside the mod.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "redstone/evolve.hpp"
#include "redstone/spec.hpp"

using namespace rs;

namespace {

struct Options {
  std::string spec_path;
  std::string out_path;
  EvolveOptions evolve;
  int polish_rounds = 0; // > 0: polish the sketch instead of annealing
  bool triples = false;
};

void print_layout(const World &w) {
  const Vec3 size = w.size();
  for (int y = 0; y < size.y; ++y) {
    std::printf("y %d\n", y);
    for (int z = 0; z < size.z; ++z) {
      for (int x = 0; x < size.x; ++x) {
        const char *c = ".";
        switch (w.at({x, y, z}).type) {
        case BlockType::Solid:
          c = "#";
          break;
        case BlockType::Dust:
          c = "+";
          break;
        case BlockType::Repeater:
          c = ">";
          break;
        case BlockType::Torch:
          c = "t";
          break;
        case BlockType::Comparator:
          c = "c";
          break;
        case BlockType::Target:
          c = "x";
          break;
        case BlockType::Barrel:
          c = "b";
          break;
        case BlockType::Input:
          c = "I";
          break;
        case BlockType::Output:
          c = "O";
          break;
        default:
          break;
        }
        std::printf("%s", c);
      }
      std::printf("\n");
    }
  }
}

// The spec's lines but its hints, followed by the layout as hints, for
// redstone_cli --check.
void write_layout(const std::string &spec_text, const World &w,
                  const std::string &path) {
  std::ofstream out(path);
  if (!out)
    throw std::runtime_error("cannot write " + path);
  std::istringstream lines(spec_text);
  std::string line;
  while (std::getline(lines, line))
    if (line.rfind("hint ", 0) != 0)
      out << line << "\n";
  const Vec3 size = w.size();
  for (int y = 0; y < size.y; ++y)
    for (int z = 0; z < size.z; ++z)
      for (int x = 0; x < size.x; ++x) {
        const Block b = w.at({x, y, z});
        if (b.type == BlockType::Air || b.type == BlockType::Input ||
            b.type == BlockType::Output)
          continue;
        out << "hint " << x << " " << y << " " << z << " " << block_words(b)
            << "\n";
      }
}

void usage() {
  std::cout
      << "usage: redstone_evolve SPEC [--seconds S] [--threads N] [--seed K]\n"
         "         [--out FILE] [--start-temp T] [--end-temp T]\n"
         "         [--steps-per-run N] [--from-best P] [--no-sketch]\n"
         "         [--polish ROUNDS] [--triples]\n"
         "Anneals the spec's box directly (2 layers: floor and components) "
         "until a circuit passes the verifier (the layout as drawn). Hint "
         "lines, if any, are the start of every run (--no-sketch: start "
         "empty). --out writes the circuit as a spec with hint lines (or, "
         "with none found, the closest grid to FILE.best.txt). --polish: no "
         "annealing; every change of one cell of the sketch, then of two "
         "(--triples: also three close together) is tried; the best becomes "
         "the sketch for the next round.\n";
}

} // namespace

int main(int argc, char **argv) {
  Options o;
  o.evolve.verification.placements = {Placement{}};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc)
        throw std::invalid_argument(arg + " needs a value");
      return argv[++i];
    };
    if (arg == "--help") {
      usage();
      return 0;
    } else if (arg == "--seconds") {
      o.evolve.time_limit_seconds = std::stod(value());
    } else if (arg == "--threads") {
      o.evolve.threads = std::stoi(value());
    } else if (arg == "--seed") {
      o.evolve.seed = static_cast<unsigned>(std::stoul(value()));
    } else if (arg == "--out") {
      o.out_path = value();
    } else if (arg == "--start-temp") {
      o.evolve.start_temperature = std::stod(value());
    } else if (arg == "--end-temp") {
      o.evolve.end_temperature = std::stod(value());
    } else if (arg == "--steps-per-run") {
      o.evolve.steps_per_run = std::stol(value());
    } else if (arg == "--from-best") {
      o.evolve.from_best = std::stod(value());
    } else if (arg == "--no-sketch") {
      o.evolve.use_sketch = false;
    } else if (arg == "--polish") {
      o.polish_rounds = std::stoi(value());
    } else if (arg == "--triples") {
      o.triples = true;
    } else if (o.spec_path.empty()) {
      o.spec_path = arg;
    } else {
      usage();
      return 2;
    }
  }
  if (o.spec_path.empty()) {
    usage();
    return 2;
  }
  std::ifstream file(o.spec_path);
  if (!file)
    throw std::runtime_error("cannot read " + o.spec_path);
  std::stringstream text;
  text << file.rdbuf();
  const Spec spec = parse_spec(text.str());
  const ProgressCallback say = [](std::string_view m) {
    std::printf("%.*s\n", static_cast<int>(m.size()), m.data());
    std::fflush(stdout);
  };

  std::optional<World> found;
  if (o.polish_rounds > 0) {
    PolishOptions p;
    p.rounds = o.polish_rounds;
    p.triples = o.triples;
    p.time_limit_seconds = o.evolve.time_limit_seconds;
    p.threads = o.evolve.threads;
    p.verification = o.evolve.verification;
    found = polish(spec, p, say);
  } else {
    EvolveResult result = evolve(spec, o.evolve, say);
    found = result.layout;
    if (!found && result.best && !o.out_path.empty()) {
      write_layout(text.str(), *result.best, o.out_path + ".best.txt");
      std::printf("closest grid written to %s.best.txt\n", o.out_path.c_str());
      print_layout(*result.best);
    }
  }
  if (!found)
    return 1;
  print_layout(*found);
  if (!o.out_path.empty()) {
    write_layout(text.str(), *found, o.out_path);
    std::printf("written to %s\n", o.out_path.c_str());
  }
  return 0;
}

// Experiment: search a spec's box directly for a working circuit, with no
// gates, placement or routing. Each thread anneals a grid of blocks: a step
// changes a few cells, the simulator scores the result against every row,
// and better (or, early on, slightly worse) grids are kept. Loops are fine:
// every row is simulated from the built state, as the verifier does. With
// hint lines in the spec, each run starts from that sketch instead of an
// empty grid. Ends at the first circuit the full verifier accepts, or when
// the time is up.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "redstone/simulator.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

using namespace rs;

namespace {

constexpr int kSettleTicks = 60;
constexpr long kRowWeight = 1000;     // per output right in one row
constexpr long kUnsettled = -1000000; // a row that never comes to rest
constexpr long kBlockCost = 1;        // a little pressure toward fewer blocks
constexpr int kMaxCellsPerStep = 3;
constexpr int kMaxSignal = 15;

struct Options {
  std::string spec_path;
  std::string out_path;
  double seconds = 60.0;
  int threads = 0;
  unsigned seed = 1;
  double start_temperature = 300.0;
  double end_temperature = 5.0;
  long steps_per_run = 200000; // then the run starts over (from the sketch)
  bool sketch = true;          // start from the spec's hint lines
  double from_best = 0.5;      // share of runs that start from the best grid
  int polish_rounds = 0;       // > 0: every 1- and 2-cell change instead
  bool triples = false;        // polish: also 3 cells close together
};

// The cells a search may change: the component layer (y = 1), ports left
// out. The floor (y = 0) follows: a block under whatever needs one.
struct Board {
  Spec spec;
  World base;
  std::vector<Vec3> cells;
  std::vector<Block> start; // the sketch (air where it has nothing)
  std::vector<InputVector> rows;
  std::vector<std::vector<SignalRange>> wanted;
};

Board make_board(const Spec &spec, bool sketch) {
  Board board{spec, spec.base_world(), {}, {}, {}, {}};
  const Vec3 size = spec.size;
  for (int z = 0; z < size.z; ++z)
    for (int x = 0; x < size.x; ++x) {
      const Vec3 p{x, 1, z};
      const BlockType t = board.base.at(p).type;
      if (t == BlockType::Input || t == BlockType::Output)
        board.base.set({x, 0, z}, Block::solid());
      else
        board.cells.push_back(p);
    }
  board.start.assign(board.cells.size(), Block::air());
  if (sketch)
    for (const auto &[p, b] : spec.hints)
      for (std::size_t i = 0; i < board.cells.size(); ++i)
        if (board.cells[i] == p)
          board.start[i] = b;
  const int n = static_cast<int>(spec.inputs.size());
  // Gray code order: one input changes from row to row.
  for (int i = 0; i < (1 << n); ++i) {
    const int g = i ^ (i >> 1);
    InputVector v;
    for (int k = 0; k < n; ++k)
      v.push_back(((g >> k) & 1) != 0 ? kMaxSignal : 0);
    std::optional<std::vector<SignalRange>> out = spec.expected_outputs(v);
    if (!out)
      continue;
    board.rows.push_back(v);
    board.wanted.push_back(*out);
  }
  return board;
}

constexpr Dir kSides[] = {Dir::North, Dir::East, Dir::South, Dir::West};

Block random_block(std::mt19937 &rng) {
  std::uniform_int_distribution<int> pick(0, 99);
  std::uniform_int_distribution<int> side(0, 3);
  std::uniform_int_distribution<int> signal(1, kMaxSignal);
  const int r = pick(rng);
  if (r < 25)
    return Block::air();
  if (r < 40)
    return Block::solid();
  if (r < 62)
    return Block::dust();
  if (r < 72)
    return Block::repeater(kSides[side(rng)]);
  if (r < 80)
    return Block::torch(r < 74 ? Dir::Down : kSides[side(rng)]);
  if (r < 94)
    return Block::comparator(kSides[side(rng)], r < 87
                                                    ? ComparatorMode::Compare
                                                    : ComparatorMode::Subtract);
  if (r < 97)
    return Block::target();
  return Block::barrel(signal(rng));
}

// A small change to one cell: turn it, flip a comparator's mode, change a
// barrel's signal, or put something new there.
Block nudge(const Block &b, std::mt19937 &rng) {
  std::uniform_int_distribution<int> pick(0, 2);
  std::uniform_int_distribution<int> side(0, 3);
  std::uniform_int_distribution<int> signal(1, kMaxSignal);
  if (pick(rng) == 0)
    return random_block(rng);
  switch (b.type) {
  case BlockType::Repeater:
    return Block::repeater(kSides[side(rng)]);
  case BlockType::Comparator:
    return pick(rng) == 0
               ? Block::comparator(b.facing, b.comparator_mode() ==
                                                     ComparatorMode::Compare
                                                 ? ComparatorMode::Subtract
                                                 : ComparatorMode::Compare)
               : Block::comparator(kSides[side(rng)], b.comparator_mode());
  case BlockType::Torch:
    return Block::torch(pick(rng) == 0 ? Dir::Down : kSides[side(rng)]);
  case BlockType::Barrel:
    return Block::barrel(signal(rng));
  default:
    return random_block(rng);
  }
}

bool needs_floor(const Block &b) {
  return b.type == BlockType::Dust || b.type == BlockType::Repeater ||
         b.type == BlockType::Comparator ||
         (b.type == BlockType::Torch && b.facing == Dir::Down);
}

World build(const Board &board, const std::vector<Block> &genes) {
  World w = board.base;
  for (std::size_t i = 0; i < genes.size(); ++i) {
    const Vec3 p = board.cells[i];
    w.set(p, genes[i]);
    w.set({p.x, 0, p.z}, needs_floor(genes[i]) ? Block::solid() : Block::air());
  }
  return w;
}

// Rows right (weighted) minus how far off the wrong outputs are, minus a
// little per block.
long score(const Board &board, const World &w, const std::vector<Block> &genes,
           bool &perfect) {
  perfect = false;
  if (!w.validate().empty())
    return std::numeric_limits<long>::min();
  long total = 0;
  for (const Block &b : genes)
    if (b.type != BlockType::Air)
      total -= kBlockCost;
  bool all = true;
  // As the verifier does first: settled once as built (inputs off), then
  // each row from there, so a circuit cannot depend on the rows before.
  Simulator built(w);
  if (!built.run_until_quiet(kSettleTicks))
    return kUnsettled;
  for (std::size_t r = 0; r < board.rows.size(); ++r) {
    Simulator sim = built;
    for (std::size_t k = 0; k < board.rows[r].size(); ++k)
      sim.set_input(static_cast<int>(k), board.rows[r][k]);
    if (!sim.run_until_quiet(kSettleTicks))
      return kUnsettled;
    for (std::size_t o = 0; o < board.wanted[r].size(); ++o) {
      const SignalRange want = board.wanted[r][o];
      const int p = sim.output(static_cast<int>(o));
      if (p >= want.min && p <= want.max) {
        total += kRowWeight;
      } else {
        all = false;
        total -= p < want.min ? want.min - p : p - want.max;
      }
    }
  }
  perfect = all;
  return total;
}

struct Shared {
  std::atomic<bool> done = false;
  std::atomic<long> evaluations = 0;
  std::atomic<long> best = std::numeric_limits<long>::min();
  std::atomic<long> rejected = 0;
  std::atomic<long> runs = 0;
  std::mutex mutex;
  std::optional<World> found;
  std::string first_rejection;
  std::vector<Block> best_genes; // under mutex
  long best_genes_score = std::numeric_limits<long>::min();
};

void search(const Board &board, const Options &o, unsigned seed, Shared &shared,
            std::chrono::steady_clock::time_point deadline) {
  std::mt19937 rng(seed);
  const std::size_t count = board.cells.size();
  std::uniform_int_distribution<std::size_t> cell(0, count - 1);
  std::uniform_int_distribution<int> changes(1, kMaxCellsPerStep);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  while (!shared.done && std::chrono::steady_clock::now() < deadline) {
    ++shared.runs;
    std::vector<Block> genes = board.start;
    {
      // Some runs carry on from the best grid so far.
      std::lock_guard lock(shared.mutex);
      if (!shared.best_genes.empty() && unit(rng) < o.from_best)
        genes = shared.best_genes;
    }
    bool perfect = false;
    long current = score(board, build(board, genes), genes, perfect);
    if (current == std::numeric_limits<long>::min())
      current = kUnsettled * 2; // an invalid sketch: anything valid is better
    long evaluations = 0;
    for (long step = 0; step < o.steps_per_run && !shared.done; ++step) {
      if ((step & 1023) == 0 && std::chrono::steady_clock::now() >= deadline)
        break;
      const double t = o.start_temperature *
                       std::pow(o.end_temperature / o.start_temperature,
                                static_cast<double>(step) / o.steps_per_run);
      std::vector<Block> next = genes;
      for (int c = changes(rng); c > 0; --c) {
        const std::size_t i = cell(rng);
        if (unit(rng) < 0.15) {
          std::swap(next[i], next[cell(rng)]);
        } else {
          next[i] = nudge(next[i], rng);
        }
      }
      const World w = build(board, next);
      const long s = score(board, w, next, perfect);
      if ((++evaluations & 255) == 0) {
        shared.evaluations += evaluations;
        evaluations = 0;
      }
      if (s == std::numeric_limits<long>::min())
        continue;
      if (perfect) {
        // The quick score walks the rows once; the verifier checks every
        // switch between them.
        VerificationOptions one;
        one.placements = {Placement{}};
        const std::optional<VerificationFailure> failure = verify_layout(
            w, board.spec, board.spec.verification_vectors(), one);
        if (failure) {
          ++shared.rejected;
          std::lock_guard lock(shared.mutex);
          if (shared.first_rejection.empty())
            shared.first_rejection = failure->reason;
        } else {
          std::lock_guard lock(shared.mutex);
          if (!shared.found)
            shared.found = w;
          shared.done = true;
          break;
        }
      }
      if (s >= current || unit(rng) < std::exp((s - current) / t)) {
        genes = std::move(next);
        current = s;
        long seen = shared.best;
        while (current > seen &&
               !shared.best.compare_exchange_weak(seen, current))
          ;
        if (current > seen) {
          std::lock_guard lock(shared.mutex);
          if (current > shared.best_genes_score) {
            shared.best_genes_score = current;
            shared.best_genes = genes;
          }
        }
      }
    }
    shared.evaluations += evaluations;
  }
}

// Every block a component cell may hold.
std::vector<Block> alphabet() {
  std::vector<Block> all{Block::air(), Block::solid(), Block::dust(),
                         Block::target(), Block::torch(Dir::Down)};
  for (Dir d : kSides) {
    all.push_back(Block::repeater(d));
    all.push_back(Block::torch(d));
    all.push_back(Block::comparator(d, ComparatorMode::Compare));
    all.push_back(Block::comparator(d, ComparatorMode::Subtract));
  }
  for (int s = 1; s <= kMaxSignal; ++s)
    all.push_back(Block::barrel(s));
  return all;
}

// Every change of one cell of the start grid, then of two, tried in
// parallel; the first circuit the verifier accepts ends it. When none is
// perfect but some scored better, the best becomes the start and it goes
// again (`rounds` times at most).
std::optional<World> polish(const Board &board, int threads, int rounds,
                            bool triples,
                            std::chrono::steady_clock::time_point deadline) {
  const std::vector<Block> letters = alphabet();
  const long a = static_cast<long>(letters.size());
  const long n = static_cast<long>(board.cells.size());
  std::vector<std::pair<long, long>> pairs;
  for (long i = 0; i < n; ++i)
    for (long j = i + 1; j < n; ++j)
      pairs.emplace_back(i, j);
  // With `triples`: changes of three cells close together too (each pair
  // at most two cells apart along x and along z).
  std::vector<std::array<long, 3>> close;
  auto near = [&](long i, long j) {
    const Vec3 p = board.cells[i], q = board.cells[j];
    return std::abs(p.x - q.x) <= 2 && std::abs(p.z - q.z) <= 2;
  };
  if (triples)
    for (long i = 0; i < n; ++i)
      for (long j = i + 1; j < n; ++j)
        for (long k = j + 1; k < n; ++k)
          if (near(i, j) && near(i, k) && near(j, k))
            close.push_back({i, j, k});
  const long singles = n * a;
  const long doubles = static_cast<long>(pairs.size()) * a * a;
  const long total =
      singles + doubles + static_cast<long>(close.size()) * a * a * a;
  std::vector<Block> current = board.start;
  for (int round = 0; round < rounds; ++round) {
    bool perfect = false;
    const long start_score =
        score(board, build(board, current), current, perfect);
    std::printf("polish round %d: start score %ld, %ld changes to try\n",
                round + 1, start_score, total);
    std::fflush(stdout);
    std::atomic<long> next = 0;
    std::atomic<bool> done = false;
    std::mutex mutex;
    std::optional<World> found;
    long best_score = start_score;
    std::vector<Block> best = current;
    auto work = [&] {
      constexpr long kChunk = 256;
      std::vector<Block> genes;
      while (!done) {
        const long from = next.fetch_add(kChunk);
        if (from >= total || std::chrono::steady_clock::now() >= deadline)
          return;
        for (long k = from; k < std::min(total, from + kChunk) && !done; ++k) {
          genes = current;
          if (k < singles) {
            const Block &b = letters[k % a];
            if (genes[k / a] == b)
              continue;
            genes[k / a] = b;
          } else if (k < singles + doubles) {
            const long r = k - singles;
            const auto [i, j] = pairs[r / (a * a)];
            const Block &bi = letters[(r / a) % a];
            const Block &bj = letters[r % a];
            if (genes[i] == bi || genes[j] == bj)
              continue;
            genes[i] = bi;
            genes[j] = bj;
          } else {
            const long r = k - singles - doubles;
            const std::array<long, 3> &t = close[r / (a * a * a)];
            const Block &b0 = letters[(r / (a * a)) % a];
            const Block &b1 = letters[(r / a) % a];
            const Block &b2 = letters[r % a];
            if (genes[t[0]] == b0 || genes[t[1]] == b1 || genes[t[2]] == b2)
              continue;
            genes[t[0]] = b0;
            genes[t[1]] = b1;
            genes[t[2]] = b2;
          }
          const World w = build(board, genes);
          bool ok = false;
          const long s = score(board, w, genes, ok);
          if (ok) {
            VerificationOptions one;
            one.placements = {Placement{}};
            if (!verify_layout(w, board.spec, board.spec.verification_vectors(),
                               one)) {
              std::lock_guard lock(mutex);
              if (!found)
                found = w;
              done = true;
              return;
            }
          }
          if (s > best_score) {
            std::lock_guard lock(mutex);
            if (s > best_score) {
              best_score = s;
              best = genes;
            }
          }
        }
      }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
      pool.emplace_back(work);
    for (std::thread &t : pool)
      t.join();
    if (found)
      return found;
    std::printf("polish round %d: best %ld\n", round + 1, best_score);
    std::fflush(stdout);
    if (best_score <= start_score ||
        std::chrono::steady_clock::now() >= deadline)
      return std::nullopt;
    current = best;
  }
  return std::nullopt;
}

void print_layout(const World &w) {
  const Vec3 size = w.size();
  for (int y = 0; y < size.y; ++y) {
    std::printf("y %d\n", y);
    for (int z = 0; z < size.z; ++z) {
      for (int x = 0; x < size.x; ++x) {
        const Block b = w.at({x, y, z});
        const char *c = ".";
        switch (b.type) {
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

// The spec's header (everything but hint lines) followed by the found
// layout as hints, for redstone_cli --check.
void write_found(const std::string &spec_text, const World &w,
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
         "until a circuit passes the verifier. Hint lines, if any, are the "
         "start of every run (--no-sketch: start empty). --out writes the "
         "circuit as a spec with hint lines. --polish: no annealing; every "
         "change of one cell of the sketch, then of two, is tried; the best "
         "becomes the sketch for the next round.\n";
}

} // namespace

int main(int argc, char **argv) {
  Options o;
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
      o.seconds = std::stod(value());
    } else if (arg == "--threads") {
      o.threads = std::stoi(value());
    } else if (arg == "--seed") {
      o.seed = static_cast<unsigned>(std::stoul(value()));
    } else if (arg == "--out") {
      o.out_path = value();
    } else if (arg == "--start-temp") {
      o.start_temperature = std::stod(value());
    } else if (arg == "--end-temp") {
      o.end_temperature = std::stod(value());
    } else if (arg == "--steps-per-run") {
      o.steps_per_run = std::stol(value());
    } else if (arg == "--from-best") {
      o.from_best = std::stod(value());
    } else if (arg == "--triples") {
      o.triples = true;
    } else if (arg == "--polish") {
      o.polish_rounds = std::stoi(value());
    } else if (arg == "--no-sketch") {
      o.sketch = false;
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
  if (o.start_temperature <= 0 || o.end_temperature <= 0 ||
      o.steps_per_run <= 0)
    throw std::invalid_argument("temperatures and steps must be positive");
  std::ifstream file(o.spec_path);
  if (!file)
    throw std::runtime_error("cannot read " + o.spec_path);
  std::stringstream text;
  text << file.rdbuf();
  const Spec spec = parse_spec(text.str());
  if (spec.size.y != 2)
    throw std::invalid_argument("this experiment searches 2-layer boxes only");
  const Board board = make_board(spec, o.sketch);
  const int threads =
      o.threads > 0 ? o.threads
                    : static_cast<int>(std::thread::hardware_concurrency());
  long sketch_blocks = 0;
  for (const Block &b : board.start)
    sketch_blocks += b.type != BlockType::Air;
  std::printf("%dx%dx%d box, %zu cells to fill, %ld from the sketch, %zu "
              "rows, %d threads\n",
              spec.size.x, spec.size.y, spec.size.z, board.cells.size(),
              sketch_blocks, board.rows.size(), threads);

  Shared shared;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline =
      start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(o.seconds));
  if (o.polish_rounds > 0) {
    std::optional<World> found =
        polish(board, threads, o.polish_rounds, o.triples, deadline);
    if (!found) {
      std::printf("polish: nothing\n");
      return 1;
    }
    std::printf("polish: found\n");
    print_layout(*found);
    if (!o.out_path.empty()) {
      write_found(text.str(), *found, o.out_path);
      std::printf("written to %s\n", o.out_path.c_str());
    }
    return 0;
  }
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t)
    workers.emplace_back(search, std::cref(board), std::cref(o),
                         o.seed * 7919u + t, std::ref(shared), deadline);
  const long perfect_score =
      kRowWeight *
      static_cast<long>(board.rows.size() * board.wanted.front().size());
  int tick = 0;
  while (!shared.done && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (++tick % 30 != 0)
      continue;
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::printf("%6.0f s: best %ld of %ld (less blocks), %.0f evaluations/s, "
                "%ld runs, %ld full checks failed\n",
                s, shared.best.load(), perfect_score, shared.evaluations / s,
                shared.runs.load(), shared.rejected.load());
    std::fflush(stdout);
  }
  for (std::thread &w : workers)
    w.join();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  if (!shared.found) {
    if (!o.out_path.empty() && !shared.best_genes.empty()) {
      const World best = build(board, shared.best_genes);
      write_found(text.str(), best, o.out_path + ".best.txt");
      std::printf("best grid (score %ld) written to %s.best.txt\n",
                  shared.best_genes_score, o.out_path.c_str());
      print_layout(best);
    }
    std::printf("nothing in %.0f s (%ld evaluations, %ld full checks failed; "
                "the first: %s)\n",
                seconds, shared.evaluations.load(), shared.rejected.load(),
                shared.first_rejection.c_str());
    return 1;
  }
  std::printf("found in %.1f s (%ld evaluations)\n", seconds,
              shared.evaluations.load());
  print_layout(*shared.found);
  if (!o.out_path.empty()) {
    write_found(text.str(), *shared.found, o.out_path);
    std::printf("written to %s\n", o.out_path.c_str());
  }
  return 0;
}

#include "redstone/evolve.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "redstone/layout_check.hpp"
#include "redstone/simulator.hpp"
#include "redstone/verifier.hpp"

namespace rs {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSettleTicks = 60;
constexpr long kRowWeight = 1000;     // per output right in one row
constexpr long kUnsettled = -1000000; // a row that never comes to rest
constexpr long kInvalid = std::numeric_limits<long>::min();
constexpr long kBlockCost = 1; // a little pressure toward fewer blocks
constexpr int kMaxCellsPerStep = 3;
constexpr double kSwapShare = 0.15;
constexpr auto kProgressEvery = std::chrono::seconds(30);
constexpr auto kPreviewEvery = std::chrono::seconds(2);
constexpr auto kPoll = std::chrono::milliseconds(200);
constexpr Dir kSides[] = {Dir::North, Dir::East, Dir::South, Dir::West};

// The cells a search may change: the component layer (y = 1), ports and
// kept blocks left out. The floor (y = 0) follows.
struct Board {
  Spec spec;
  World base;
  std::vector<Vec3> cells;
  std::vector<Block> start; // the sketch (air where it has nothing)
  std::vector<InputVector> rows;
  std::vector<std::vector<SignalRange>> wanted;
};

Board make_board(const Spec &spec, bool sketch) {
  if (spec.size.y != 2)
    throw std::invalid_argument(
        "Evolve searches boxes 2 high (a floor and a component layer)");
  for (const InputPort &in : spec.inputs)
    if (in.is_free())
      throw std::invalid_argument("Evolve needs every port placed");
  Board board{spec, spec.base_world(), {}, {}, {}, {}};
  const Vec3 size = spec.size;
  // Kept blocks are in the base world already; empty cells stay empty.
  auto kept = [&](Vec3 p) {
    return std::any_of(spec.fixed.begin(), spec.fixed.end(),
                       [&](const auto &k) { return k.first == p; }) ||
           std::find(spec.forbidden.begin(), spec.forbidden.end(), p) !=
               spec.forbidden.end();
  };
  for (int z = 0; z < size.z; ++z)
    for (int x = 0; x < size.x; ++x) {
      const Vec3 p{x, 1, z};
      const BlockType t = board.base.at(p).type;
      if (t == BlockType::Input || t == BlockType::Output)
        board.base.set({x, 0, z}, Block::solid());
      else if (!kept(p))
        board.cells.push_back(p);
    }
  if (board.cells.empty())
    throw std::invalid_argument("the box has no cells to fill");
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
  if (board.rows.empty())
    throw std::invalid_argument("Evolve needs rows with on/off inputs");
  return board;
}

bool allowed(const Board &board, const Block &b) {
  return b.type == BlockType::Air || board.spec.palette_contains(b.type);
}

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
// little per block; kInvalid for a grid that cannot be built. `right`, if
// given, gets how many (row, output) pairs are right.
long score(const Board &board, const World &w, const std::vector<Block> &genes,
           bool &perfect, int *right = nullptr) {
  perfect = false;
  if (right)
    *right = 0;
  if (!w.validate().empty())
    return kInvalid;
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
        if (right)
          ++*right;
      } else {
        all = false;
        total -= p < want.min ? want.min - p : p - want.max;
      }
    }
  }
  perfect = all;
  return total;
}

// How many (row, output) pairs there are to get right.
int checks_of(const Board &board) {
  return static_cast<int>(board.rows.size() * board.wanted.front().size());
}

long perfect_score_of(const Board &board) {
  return kRowWeight * checks_of(board);
}

// The checks a grid that gets every row right must pass: without feedback,
// no loop through a component (the solver's own rule; its model is built
// once per thread, on first use), then the verifier.
class Acceptance {
public:
  Acceptance(const Board &board, const VerificationOptions &verification)
      : board_(board), verification_(verification) {}

  bool accepts(const World &w) {
    if (!this->board_.spec.allow_feedback) {
      if (!this->express_)
        this->express_ = std::make_unique<ExpressChecker>(this->board_.spec);
      if (!this->express_->can_express(w))
        return false;
    }
    return !verify_layout(w, this->board_.spec,
                          this->board_.spec.verification_vectors(),
                          this->verification_);
  }

private:
  const Board &board_;
  const VerificationOptions &verification_;
  std::unique_ptr<ExpressChecker> express_;
};

struct Shared {
  std::atomic<bool> done = false;
  std::atomic<long> evaluations = 0;
  std::atomic<long> best = std::numeric_limits<long>::min();
  std::atomic<long> rejected = 0;
  std::atomic<long> runs = 0;
  std::mutex mutex;
  std::optional<World> found;
  std::vector<Block> best_genes; // under mutex
  long best_genes_score = std::numeric_limits<long>::min();
  int best_version = 0; // under mutex: counts new bests
};

int thread_count(int asked) {
  if (asked > 0)
    return asked;
  return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
}

bool stopped(const std::atomic<bool> *cancel, Clock::time_point deadline) {
  return (cancel && cancel->load()) || Clock::now() >= deadline;
}

void anneal(const Board &board, const EvolveOptions &o, unsigned seed,
            Shared &shared, Clock::time_point deadline) {
  std::mt19937 rng(seed);
  const std::size_t count = board.cells.size();
  std::uniform_int_distribution<std::size_t> cell(0, count - 1);
  std::uniform_int_distribution<int> changes(1, kMaxCellsPerStep);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  Acceptance acceptance(board, o.verification);
  while (!shared.done && !stopped(o.cancel, deadline)) {
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
    if (current == kInvalid)
      current = kUnsettled * 2; // an invalid sketch: anything valid is better
    long evaluations = 0;
    for (long step = 0; step < o.steps_per_run && !shared.done; ++step) {
      if ((step & 1023) == 0 && stopped(o.cancel, deadline))
        break;
      const double t = o.start_temperature *
                       std::pow(o.end_temperature / o.start_temperature,
                                static_cast<double>(step) / o.steps_per_run);
      std::vector<Block> next = genes;
      for (int c = changes(rng); c > 0; --c) {
        const std::size_t i = cell(rng);
        if (unit(rng) < kSwapShare) {
          std::swap(next[i], next[cell(rng)]);
        } else {
          Block b = nudge(next[i], rng);
          if (allowed(board, b))
            next[i] = b;
        }
      }
      const World w = build(board, next);
      const long s = score(board, w, next, perfect);
      if ((++evaluations & 255) == 0) {
        shared.evaluations += evaluations;
        evaluations = 0;
      }
      if (s == kInvalid)
        continue;
      if (perfect) {
        if (acceptance.accepts(w)) {
          std::lock_guard lock(shared.mutex);
          if (!shared.found)
            shared.found = w;
          shared.done = true;
          break;
        }
        ++shared.rejected;
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
            ++shared.best_version;
          }
        }
      }
    }
    shared.evaluations += evaluations;
  }
}

} // namespace

EvolveResult evolve(const Spec &spec, const EvolveOptions &options,
                    const ProgressCallback &progress) {
  if (options.start_temperature <= 0 || options.end_temperature <= 0 ||
      options.steps_per_run <= 0)
    throw std::invalid_argument("temperatures and steps must be positive");
  const Board board = make_board(spec, options.use_sketch);
  const int threads = thread_count(options.threads);
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  const int checks = checks_of(board);
  say(std::format("evolve: {}x{}x{} box, {} cells to fill, {} threads{}; "
                  "{} output checks to get right ({} input rows x {} "
                  "outputs)",
                  spec.size.x, spec.size.y, spec.size.z, board.cells.size(),
                  threads, spec.allow_feedback ? ", loops allowed" : "", checks,
                  board.rows.size(), board.wanted.front().size()));

  Shared shared;
  const auto start = Clock::now();
  const auto deadline =
      start + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(options.time_limit_seconds));
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t)
    workers.emplace_back(anneal, std::cref(board), std::cref(options),
                         options.seed * 7919u + static_cast<unsigned>(t),
                         std::ref(shared), deadline);
  const long perfect = perfect_score_of(board);
  auto next_progress = start + kProgressEvery;
  auto next_preview = start;
  int seen_version = 0, previewed_version = 0;
  int best_right = -1;
  // How many checks the best grid so far gets right (when it changed).
  auto check_best = [&] {
    std::vector<Block> genes;
    {
      std::lock_guard lock(shared.mutex);
      if (shared.best_version == seen_version)
        return false;
      seen_version = shared.best_version;
      genes = shared.best_genes;
    }
    bool perfect_grid = false;
    int right = 0;
    score(board, build(board, genes), genes, perfect_grid, &right);
    if (right <= best_right)
      return false;
    best_right = right;
    return true;
  };
  while (!shared.done && !stopped(options.cancel, deadline)) {
    std::this_thread::sleep_for(kPoll);
    const auto now = Clock::now();
    const double s = std::chrono::duration<double>(now - start).count();
    if (check_best())
      say(std::format("evolve: closest so far: {} of {} output checks right "
                      "({:.0f} s)",
                      best_right, checks, s));
    if (options.preview && now >= next_preview) {
      std::optional<World> best;
      {
        std::lock_guard lock(shared.mutex);
        if (shared.best_version != previewed_version) {
          best = build(board, shared.best_genes);
          previewed_version = shared.best_version;
        }
      }
      if (best)
        options.preview(*best);
      next_preview = now + kPreviewEvery;
    }
    if (now >= next_progress) {
      say(std::format("evolve: {:.0f} s, closest {} of {} output checks "
                      "right, {:.0f} grids/s, {} rejected",
                      s, std::max(best_right, 0), checks,
                      shared.evaluations.load() / s, shared.rejected.load()));
      next_progress = now + kProgressEvery;
    }
  }
  for (std::thread &w : workers)
    w.join();

  EvolveResult result;
  result.perfect_score = perfect;
  result.evaluations = shared.evaluations;
  result.rejected = shared.rejected;
  result.layout = shared.found;
  result.checks = checks;
  if (!shared.best_genes.empty()) {
    result.best = build(board, shared.best_genes);
    result.best_score = shared.best_genes_score;
    check_best();
    result.best_right = std::max(best_right, 0);
  }
  const double s = std::chrono::duration<double>(Clock::now() - start).count();
  const bool cancelled = options.cancel && options.cancel->load();
  if (result.layout)
    say(std::format("evolve: found a working circuit in {:.1f} s ({} grids "
                    "tried)",
                    s, result.evaluations));
  else
    say(std::format("evolve: {} after {:.0f} s; the closest got {} of {} "
                    "output checks right{}",
                    cancelled ? "stopped" : "no working circuit", s,
                    result.best_right, checks,
                    result.rejected > 0
                        ? std::format(" ({} right everywhere but refused "
                                      "by the checks)",
                                      result.rejected)
                        : std::string()));
  return result;
}

namespace {

// Every block a component cell may hold (within the palette).
std::vector<Block> alphabet(const Board &board) {
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
  std::erase_if(all, [&](const Block &b) { return !allowed(board, b); });
  return all;
}

} // namespace

std::optional<World> polish(const Spec &spec, const PolishOptions &options,
                            const ProgressCallback &progress) {
  const Board board = make_board(spec, true);
  const int threads = thread_count(options.threads);
  const auto deadline =
      Clock::now() +
      std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  const std::vector<Block> letters = alphabet(board);
  const long a = static_cast<long>(letters.size());
  const long n = static_cast<long>(board.cells.size());
  std::vector<std::pair<long, long>> pairs;
  for (long i = 0; i < n; ++i)
    for (long j = i + 1; j < n; ++j)
      pairs.emplace_back(i, j);
  // Three cells close together: each pair at most two apart along x and z.
  std::vector<std::array<long, 3>> close;
  auto near = [&](long i, long j) {
    const Vec3 p = board.cells[i], q = board.cells[j];
    return std::abs(p.x - q.x) <= 2 && std::abs(p.z - q.z) <= 2;
  };
  if (options.triples)
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
  for (int round = 0; round < options.rounds; ++round) {
    bool perfect = false;
    const long start_score =
        score(board, build(board, current), current, perfect);
    if (progress)
      progress(std::format("polish round {}: start score {}, {} changes to "
                           "try",
                           round + 1, start_score, total));
    std::atomic<long> next = 0;
    std::atomic<bool> done = false;
    std::mutex mutex;
    std::optional<World> found;
    long best_score = start_score;
    std::vector<Block> best = current;
    auto work = [&] {
      constexpr long kChunk = 256;
      Acceptance acceptance(board, options.verification);
      std::vector<Block> genes;
      while (!done) {
        const long from = next.fetch_add(kChunk);
        if (from >= total || stopped(options.cancel, deadline))
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
          if (ok && acceptance.accepts(w)) {
            std::lock_guard lock(mutex);
            if (!found)
              found = w;
            done = true;
            return;
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
    if (progress)
      progress(std::format("polish round {}: best {}", round + 1, best_score));
    if (best_score <= start_score || stopped(options.cancel, deadline))
      return std::nullopt;
    current = best;
  }
  return std::nullopt;
}

} // namespace rs

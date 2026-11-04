// redstone_datagen: random circuits and the truth tables they compute, as
// training data for a model that sketches circuits from truth tables.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "redstone/circuit_gen.hpp"
#include "redstone/layout_check.hpp"
#include "redstone/simulator.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

namespace {

using namespace rs::gen;

constexpr std::string_view kUsage =
    "redstone_datagen - random circuits and the truth tables they compute.\n"
    "\n"
    "Usage: redstone_datagen --spec <file> --count <n> --out <file.rsd>\n"
    "  --spec <file>     box size and ports come from this spec (rows unused)\n"
    "  --count <n>       circuits to keep\n"
    "  --out <file>      binary samples; <file>.json gets parameters and "
    "stats\n"
    "  --threads <n>     worker threads (default: all)\n"
    "  --seed <n>        random seed (default 1)\n"
    "  --keep-constant   also keep circuits whose outputs never change\n"
    "  --generator <g>   grow (wires from inputs to outputs, default),\n"
    "                    motif (chained subtract/join logic), mix (half\n"
    "                    each) or random (every cell random)\n"
    "  --evolve <s>      instead of sampling, search for new functions for\n"
    "                    s seconds: one smallest circuit per function, and\n"
    "                    the spec's own table if found (<out>.target.txt)\n"
    "  --search <s>      search s seconds for a circuit computing the spec's\n"
    "                    rows; writes the best as a spec with hints to <out>\n"
    "  --seeds <file>    with --search: start from these layouts (.rsd)\n"
    "  --tables <file>   compute the truth table of every layout in an .rsd\n"
    "                    file (outputs ignored) and write them to <out>\n"
    "  --help            show this help\n";

constexpr std::array<char, 4> kMagic{'R', 'S', 'D', '1'};

// Novelty search: grown circuits it starts from, how many of the newest
// functions count as the frontier, and the checkpoint interval.
constexpr int kEvolveSeeds = 2000;
constexpr std::size_t kFrontier = 200;
constexpr double kCheckpointSeconds = 300;
// Target search: mutants without improvement before a restart.
constexpr long long kRestartAfter = 20000;

struct Options {
  std::filesystem::path spec;
  std::filesystem::path out;
  long long count = 0;
  int threads = 0;
  std::uint64_t seed = 1;
  bool keep_constant = false;
  std::string generator = "grow"; // grow, motif, mix or random
  double evolve_seconds = 0;      // > 0: novelty search instead of sampling
  double search_seconds = 0;      // > 0: search for the spec's own table
  std::filesystem::path tables;   // layouts to compute the truth tables of
  std::filesystem::path seeds;    // circuits the search starts from
};

struct Sample {
  std::vector<std::uint8_t> outputs; // [vector][output]
  std::vector<std::uint8_t> cells;   // class per cell, World index order
  std::uint64_t function = 0;        // on/off bit per (vector, output)
};

// Novelty search: an archive with the smallest circuit found for every
// function; mutants that compute a new function, or a known one with fewer
// blocks, replace or join it.
int run_evolve(const Options &o, rs::Spec spec,
               const std::vector<rs::InputVector> &vectors,
               const CellClasses &classes, int threads) {
  struct Entry {
    rs::World world{{1, 1, 1}};
    std::vector<std::uint8_t> table;
    int blocks = 0;
  };
  const int outputs = static_cast<int>(spec.outputs.size());
  const Target target = target_of(spec, vectors);
  std::mutex mutex;
  std::map<std::uint64_t, Entry> archive;
  std::vector<std::uint64_t> keys;
  std::atomic<long long> tried = 0;
  std::atomic<bool> stop = false;
  std::optional<Entry> best_target;
  const auto started = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         started)
        .count();
  };

  auto offer = [&](rs::World &&w, std::vector<std::uint8_t> &&table) {
    std::uint64_t key = key_of(table);
    int blocks = count_blocks(w);
    std::lock_guard lock(mutex);
    if (target.matches(key) && (!best_target || blocks < best_target->blocks)) {
      best_target = Entry{w, table, blocks};
      std::clog << std::format("{:.0f}s: the spec's table with {} blocks\n",
                               elapsed(), blocks);
    }
    auto it = archive.find(key);
    if (it == archive.end()) {
      archive.emplace(key, Entry{std::move(w), std::move(table), blocks});
      keys.push_back(key);
    } else if (blocks < it->second.blocks) {
      it->second = Entry{std::move(w), std::move(table), blocks};
    }
  };

  auto save = [&] {
    std::lock_guard lock(mutex);
    std::ofstream out(o.out, std::ios::binary);
    rs::Vec3 size = spec.size;
    out.write(kMagic.data(), kMagic.size());
    for (int v : {size.x, size.y, size.z, static_cast<int>(spec.inputs.size()),
                  outputs, static_cast<int>(vectors.size()),
                  static_cast<int>(classes.names().size())})
      out.put(static_cast<char>(v));
    for (const auto &[key, e] : archive) {
      out.write(reinterpret_cast<const char *>(e.table.data()),
                static_cast<std::streamsize>(e.table.size()));
      for (int i = 0; i < e.world.volume(); ++i)
        out.put(
            static_cast<char>(classes.of(e.world.at(e.world.position_of(i)))));
    }
    std::ofstream meta(o.out.string() + ".json");
    meta << std::format(
        "{{\n  \"mode\": \"evolve\",\n  \"spec\": \"{}\",\n  \"seed\": {},\n"
        "  \"seconds\": {:.0f},\n  \"tried\": {},\n  \"functions\": {},\n"
        "  \"target_found\": {},\n  \"target_blocks\": {}\n}}\n",
        o.spec.generic_string(), o.seed, elapsed(), tried.load(),
        archive.size(), best_target ? "true" : "false",
        best_target ? best_target->blocks : -1);
    if (best_target) {
      rs::Spec found = spec;
      found.hints.clear();
      for (int i = 0; i < best_target->world.volume(); ++i) {
        rs::Vec3 p = best_target->world.position_of(i);
        const rs::Block &b = best_target->world.at(p);
        if (b.type != rs::BlockType::Air && b.type != rs::BlockType::Input &&
            b.type != rs::BlockType::Output)
          found.hints.emplace_back(p, b);
      }
      std::ofstream(o.out.string() + ".target.txt") << rs::format_spec(found);
    }
  };

  // Seed the archive with grown circuits.
  {
    std::mt19937_64 rng(o.seed);
    for (int i = 0; i < kEvolveSeeds; ++i) {
      rs::World w = grown_layout(spec, rng);
      if (auto table = truth_table(w, vectors, outputs))
        offer(std::move(w), std::move(*table));
    }
  }

  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < threads; ++t)
      workers.emplace_back([&, t] {
        std::mt19937_64 rng(o.seed * 7919 + t + 1);
        while (!stop) {
          rs::World w{{1, 1, 1}};
          {
            std::lock_guard lock(mutex);
            // Newer functions first half the time: they sit at the frontier.
            std::size_t n = keys.size();
            std::size_t i =
                rng() % 2 ? n - 1 - rng() % std::min<std::size_t>(n, kFrontier)
                          : rng() % n;
            w = archive.at(keys[i]).world;
          }
          // Mostly mutations; sometimes a fresh grown circuit.
          if (rng() % 20 == 0)
            w = grown_layout(spec, rng);
          else
            mutate(w, spec, rng);
          ++tried;
          if (!w.validate().empty())
            continue;
          if (auto table = truth_table(w, vectors, outputs))
            offer(std::move(w), std::move(*table));
        }
      });
    double next_report = 10, next_save = kCheckpointSeconds;
    while (elapsed() < o.evolve_seconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (elapsed() >= next_report) {
        std::lock_guard lock(mutex);
        std::clog << std::format("{:.0f}s: {} functions, {} tried{}\n",
                                 elapsed(), archive.size(), tried.load(),
                                 best_target ? ", spec's table found" : "");
        next_report += 30;
      }
      if (elapsed() >= next_save) {
        save();
        next_save += kCheckpointSeconds;
      }
    }
    stop = true;
  }
  save();
  std::cout << std::format(
      "{} functions from {} circuits in {:.0f}s; spec's "
      "table {}\n",
      archive.size(), tried.load(), elapsed(),
      best_target ? std::format("found ({} blocks)", best_target->blocks)
                  : "not found");
  return 0;
}

// Hill climbing toward the spec's table: every thread keeps a circuit and
// takes any mutant that scores at least as well (drifting across equals),
// restarting from the best or a fresh circuit when stuck.
int run_search(const Options &o, rs::Spec spec,
               const std::vector<rs::InputVector> &vectors,
               const std::vector<rs::World> &seeds, int threads) {
  const int outputs = static_cast<int>(spec.outputs.size());
  const Target target = target_of(spec, vectors);
  const int full = std::popcount(target.mask);
  if (full == 0)
    throw std::invalid_argument("the spec's rows fix no on/off output");
  std::mutex mutex;
  rs::World best_world{{1, 1, 1}};
  Score best;
  std::atomic<long long> tried = 0;
  std::atomic<bool> stop = false;
  const auto started = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         started)
        .count();
  };
  auto save = [&] {
    std::lock_guard lock(mutex);
    rs::Spec found = spec;
    found.hints.clear();
    for (int i = 0; i < best_world.volume(); ++i) {
      rs::Vec3 q = best_world.position_of(i);
      const rs::Block &b = best_world.at(q);
      if (b.type != rs::BlockType::Air && b.type != rs::BlockType::Input &&
          b.type != rs::BlockType::Output)
        found.hints.emplace_back(q, b);
    }
    std::ofstream(o.out) << rs::format_spec(found);
    std::ofstream(o.out.string() + ".json") << std::format(
        "{{\n  \"mode\": \"search\",\n  \"spec\": \"{}\",\n  \"seed\": {},"
        "\n  \"seconds\": {:.0f},\n  \"tried\": {},\n  \"rows_right\": {},"
        "\n  \"rows\": {},\n  \"blocks\": {}\n}}\n",
        o.spec.generic_string(), o.seed, elapsed(), tried.load(), best.right,
        full, best.blocks);
  };

  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < threads; ++t)
      workers.emplace_back([&, t] {
        std::mt19937_64 rng(o.seed * 104729 + t + 1);
        rs::ExpressChecker checker(spec);
        auto fresh = [&]() -> std::pair<rs::World, Score> {
          while (true) {
            rs::World w = seeds.empty() ? grown_layout(spec, rng)
                                        : seeds[rng() % seeds.size()];
            if (!w.validate().empty() || !checker.can_express(w))
              continue;
            if (auto table = truth_table(w, vectors, outputs))
              return {w, score_of(*table, target, count_blocks(w))};
          }
        };
        auto [current, score] = fresh();
        long long since_better = 0;
        while (!stop) {
          rs::World child = current;
          mutate(child, spec, rng);
          ++tried;
          ++since_better;
          // Loops are ruled out like the solver does, so what the search
          // finds can be finished by a sketch repair.
          if (!child.validate().empty() || !checker.can_express(child))
            continue;
          auto table = truth_table(child, vectors, outputs);
          if (!table)
            continue;
          Score s = score_of(*table, target, count_blocks(child));
          // All rows right still has to pass the full verifier (timing,
          // every position and rotation) to count as solved.
          if (s.right == full &&
              rs::verify_layout(child, spec, spec.verification_vectors()))
            s.right = full - 1;
          if (s > score)
            since_better = 0;
          // Block count only breaks ties for the best: extra blocks are the
          // room the search needs to build new structure.
          if (s.right > score.right ||
              (s.right == score.right && s.wrong <= score.wrong)) {
            current = std::move(child);
            score = s;
            std::lock_guard lock(mutex);
            if (score > best) {
              bool first_full = score.right == full && best.right < full;
              best = score;
              best_world = current;
              if (first_full || score.right == full)
                std::clog << std::format(
                    "{:.0f}s: all {} rows right with {} blocks\n", elapsed(),
                    full, score.blocks);
            }
          }
          if (since_better > kRestartAfter) {
            since_better = 0;
            std::lock_guard lock(mutex);
            if (rng() % 2 && best.right >= 0) {
              current = best_world;
              score = best;
            } else {
              auto restart = fresh();
              current = restart.first;
              score = restart.second;
            }
          }
        }
      });
    double next_report = 10, next_save = kCheckpointSeconds;
    while (elapsed() < o.search_seconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (elapsed() >= next_report) {
        std::lock_guard lock(mutex);
        std::clog << std::format("{:.0f}s: best {}/{} rows ({} blocks), {} "
                                 "tried\n",
                                 elapsed(), best.right, full, best.blocks,
                                 tried.load());
        next_report += 30;
      }
      if (elapsed() >= next_save) {
        save();
        next_save += kCheckpointSeconds;
      }
    }
    stop = true;
  }
  save();
  std::cout << std::format("best {}/{} rows with {} blocks after {} circuits "
                           "in {:.0f}s -> {}\n",
                           best.right, full, best.blocks, tried.load(),
                           elapsed(), o.out.generic_string());
  return best.right == full ? 0 : 1;
}

// The layout of one .rsd record's cells on the spec's ports; none if a
// class is unknown or a port class sits off the spec's ports.
std::optional<rs::World> decode_layout(const rs::World &base,
                                       const CellClasses &classes,
                                       const char *cells) {
  rs::World w = base;
  for (int i = 0; i < base.volume(); ++i) {
    int c = static_cast<unsigned char>(cells[i]);
    rs::Vec3 p = w.position_of(i);
    rs::BlockType t = base.at(p).type;
    if (t == rs::BlockType::Input || t == rs::BlockType::Output)
      continue;
    if (c == classes.port() || c >= static_cast<int>(classes.names().size()))
      return std::nullopt;
    w.set(p, classes.block(c));
  }
  return w;
}

// All layouts of an .rsd file for the spec's box (outputs ignored).
std::vector<rs::World> read_layouts(const std::filesystem::path &path,
                                    const rs::Spec &spec,
                                    const CellClasses &classes) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());
  std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  const int header = static_cast<int>(kMagic.size()) + 7;
  if (raw.size() < static_cast<std::size_t>(header) ||
      !std::equal(kMagic.begin(), kMagic.end(), raw.begin()))
    throw std::invalid_argument(path.string() + " is not an .rsd file");
  auto u8 = [&](int i) {
    return static_cast<int>(static_cast<unsigned char>(raw[i]));
  };
  if (u8(4) != spec.size.x || u8(5) != spec.size.y || u8(6) != spec.size.z)
    throw std::invalid_argument(path.string() + " has another box size");
  const rs::World base = spec.base_world();
  const std::size_t record =
      static_cast<std::size_t>(u8(8) * u8(9) + base.volume());
  std::vector<rs::World> layouts;
  for (std::size_t at = header; at + record <= raw.size(); at += record)
    if (auto w = decode_layout(base, classes,
                               raw.data() + at + record - base.volume()))
      layouts.push_back(std::move(*w));
  return layouts;
}

// Reads layouts (records of an .rsd file whose outputs are ignored), and
// writes them back with the truth tables they compute; a layout that breaks
// a placement rule, loops (the solver could not express it) or never comes
// to rest gets 255 for every output.
int run_tables(const Options &o, const rs::Spec &spec,
               const std::vector<rs::InputVector> &vectors,
               const CellClasses &classes, int threads) {
  std::ifstream in(o.tables, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + o.tables.string());
  std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  const rs::Vec3 size = spec.size;
  const int outputs = static_cast<int>(spec.outputs.size());
  const int header = static_cast<int>(kMagic.size()) + 7;
  if (raw.size() < static_cast<std::size_t>(header) ||
      !std::equal(kMagic.begin(), kMagic.end(), raw.begin()))
    throw std::invalid_argument("not an .rsd file");
  auto u8 = [&](int i) {
    return static_cast<int>(static_cast<unsigned char>(raw[i]));
  };
  if (u8(4) != size.x || u8(5) != size.y || u8(6) != size.z ||
      u8(8) != outputs || u8(9) != static_cast<int>(vectors.size()))
    throw std::invalid_argument("layouts do not match the spec's box, "
                                "outputs or on/off vectors");
  const rs::World base = spec.base_world();
  const int volume = base.volume();
  const int table_bytes = static_cast<int>(vectors.size()) * outputs;
  const std::size_t record = static_cast<std::size_t>(table_bytes + volume);
  const std::size_t count = (raw.size() - header) / record;

  std::atomic<std::size_t> next = 0;
  std::atomic<long long> working = 0;
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < threads; ++t)
      workers.emplace_back([&] {
        rs::ExpressChecker checker(spec);
        for (std::size_t r = next++; r < count; r = next++) {
          char *rec = raw.data() + header + r * record;
          char *cells = rec + table_bytes;
          std::optional<rs::World> w = decode_layout(base, classes, cells);
          std::optional<std::vector<std::uint8_t>> table;
          if (w && w->validate().empty() && checker.can_express(*w))
            table = truth_table(*w, vectors, outputs);
          for (int i = 0; i < table_bytes; ++i)
            rec[i] = static_cast<char>(table ? (*table)[i] : 255);
          if (table)
            ++working;
        }
      });
  }
  std::ofstream(o.out, std::ios::binary)
      .write(raw.data(), static_cast<std::streamsize>(raw.size()));
  std::cout << std::format("{} layouts, {} valid and at rest\n", count,
                           working.load());
  return 0;
}

long long parse_count(std::string_view text, std::string_view option) {
  long long value = 0;
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || value < 0)
    throw std::invalid_argument(
        std::format("{} expects a whole number, got '{}'", option, text));
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
    if (arg == "--spec")
      o.spec = value();
    else if (arg == "--out")
      o.out = value();
    else if (arg == "--count")
      o.count = parse_count(value(), arg);
    else if (arg == "--threads")
      o.threads = static_cast<int>(parse_count(value(), arg));
    else if (arg == "--seed")
      o.seed = static_cast<std::uint64_t>(parse_count(value(), arg));
    else if (arg == "--keep-constant")
      o.keep_constant = true;
    else if (arg == "--seeds")
      o.seeds = value();
    else if (arg == "--tables")
      o.tables = value();
    else if (arg == "--search")
      o.search_seconds = static_cast<double>(parse_count(value(), arg));
    else if (arg == "--evolve")
      o.evolve_seconds = static_cast<double>(parse_count(value(), arg));
    else if (arg == "--generator") {
      std::string_view kind = value();
      if (kind != "grow" && kind != "random" && kind != "motif" &&
          kind != "mix")
        throw std::invalid_argument(
            "--generator is grow, motif, mix or random");
      o.generator = kind;
    } else
      throw std::invalid_argument(std::format("unknown option {}", arg));
  }
  if (o.spec.empty() || o.out.empty() ||
      (o.count <= 0 && o.evolve_seconds <= 0 && o.search_seconds <= 0 &&
       o.tables.empty()))
    throw std::invalid_argument(
        "--spec, --out and --count (or --evolve) are required");
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

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]) == "--help") {
      std::cout << kUsage;
      return 0;
    }
  try {
    Options o = parse(argc, argv);
    rs::Spec spec = rs::parse_spec(read_file(o.spec));
    for (const rs::InputPort &port : spec.inputs)
      if (port.is_free())
        throw std::invalid_argument("free ports are not supported");
    for (const rs::OutputPort &port : spec.outputs)
      if (port.is_free())
        throw std::invalid_argument("free ports are not supported");
    const int inputs = static_cast<int>(spec.inputs.size());
    const int outputs = static_cast<int>(spec.outputs.size());
    if (inputs > 6 || (1 << inputs) * outputs > 64)
      throw std::invalid_argument("too many inputs/outputs for a 64-bit key");
    const std::vector<rs::InputVector> vectors = on_off_vectors(inputs);
    const CellClasses classes;
    const int threads =
        o.threads > 0 ? o.threads
                      : static_cast<int>(
                            std::max(1u, std::thread::hardware_concurrency()));

    if (!o.tables.empty())
      return run_tables(o, spec, vectors, classes, threads);
    if (o.evolve_seconds > 0)
      return run_evolve(o, spec, vectors, classes, threads);
    if (o.search_seconds > 0) {
      // The verifier's own vectors (weak and strong inputs), so the search
      // cannot lean on inputs always being full strength.
      std::vector<rs::InputVector> checked = spec.verification_vectors();
      if (checked.size() * spec.outputs.size() > 64)
        throw std::invalid_argument("too many verification vectors to search");
      std::vector<rs::World> seeds;
      if (!o.seeds.empty())
        seeds = read_layouts(o.seeds, spec, classes);
      if (!o.seeds.empty() && seeds.empty())
        throw std::invalid_argument("no usable layouts in " + o.seeds.string());
      return run_search(o, spec, checked, seeds, threads);
    }

    std::ofstream out(o.out, std::ios::binary);
    if (!out)
      throw std::runtime_error("cannot write " + o.out.string());
    rs::Vec3 size = spec.size;
    out.write(kMagic.data(), kMagic.size());
    for (int v : {size.x, size.y, size.z, inputs, outputs,
                  static_cast<int>(vectors.size()),
                  static_cast<int>(classes.names().size())})
      out.put(static_cast<char>(v));

    std::mutex mutex;
    std::atomic<long long> kept = 0, tried = 0, unstable = 0, constant = 0;
    std::map<std::uint64_t, long long> functions;
    auto started = std::chrono::steady_clock::now();
    {
      std::vector<std::jthread> workers;
      for (int t = 0; t < threads; ++t)
        workers.emplace_back([&, t] {
          std::mt19937_64 rng(o.seed * 1000003 + t);
          std::vector<Sample> batch;
          auto flush = [&] {
            std::lock_guard lock(mutex);
            for (Sample &s : batch) {
              if (kept >= o.count)
                break;
              out.write(reinterpret_cast<const char *>(s.outputs.data()),
                        static_cast<std::streamsize>(s.outputs.size()));
              out.write(reinterpret_cast<const char *>(s.cells.data()),
                        static_cast<std::streamsize>(s.cells.size()));
              ++functions[s.function];
              long long now = ++kept;
              if (now % 100000 == 0)
                std::clog << std::format("{} kept of {} tried\n", now,
                                         tried.load());
            }
            batch.clear();
          };
          while (kept < o.count) {
            ++tried;
            rs::World w =
                o.generator == "random" ? random_layout(spec, rng)
                : o.generator == "motif" || (o.generator == "mix" && rng() % 2)
                    ? motif_layout(spec, rng)
                    : grown_layout(spec, rng);
            auto table = truth_table(w, vectors, outputs);
            if (!table) {
              ++unstable;
              continue;
            }
            Sample s;
            s.outputs = std::move(*table);
            for (std::size_t i = 0; i < s.outputs.size(); ++i)
              if (s.outputs[i] > 0)
                s.function |= std::uint64_t{1} << i;
            bool varies = false;
            for (int out_ch = 0; out_ch < outputs; ++out_ch) {
              std::uint64_t first = (s.function >> out_ch) & 1;
              for (std::size_t v = 1; v < vectors.size(); ++v)
                if (((s.function >> (v * outputs + out_ch)) & 1) != first)
                  varies = true;
            }
            if (!varies && !o.keep_constant) {
              ++constant;
              continue;
            }
            s.cells.resize(w.volume());
            for (int i = 0; i < w.volume(); ++i)
              s.cells[i] =
                  static_cast<std::uint8_t>(classes.of(w.at(w.position_of(i))));
            batch.push_back(std::move(s));
            if (batch.size() >= 256)
              flush();
          }
          flush();
        });
    }
    double seconds = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started)
                         .count();

    std::vector<std::pair<long long, std::uint64_t>> common;
    for (const auto &[f, n] : functions)
      common.push_back({n, f});
    std::sort(common.rbegin(), common.rend());
    std::ofstream meta(o.out.string() + ".json");
    meta << "{\n";
    meta << std::format("  \"spec\": \"{}\",\n", o.spec.generic_string());
    meta << std::format("  \"generator\": \"{}\",\n", o.generator);
    meta << std::format("  \"seed\": {},\n  \"keep_constant\": {},\n", o.seed,
                        o.keep_constant ? "true" : "false");
    meta << std::format("  \"size\": [{}, {}, {}],\n", size.x, size.y, size.z);
    meta << std::format("  \"inputs\": {},\n  \"outputs\": {},\n", inputs,
                        outputs);
    meta << std::format("  \"kept\": {},\n  \"tried\": {},\n", kept.load(),
                        tried.load());
    meta << std::format("  \"unstable\": {},\n  \"constant\": {},\n",
                        unstable.load(), constant.load());
    meta << std::format("  \"seconds\": {:.1f},\n", seconds);
    meta << std::format("  \"distinct_functions\": {},\n", functions.size());
    meta << "  \"classes\": [";
    for (std::size_t i = 0; i < classes.names().size(); ++i)
      meta << (i ? ", " : "") << '"' << classes.names()[i] << '"';
    meta << "],\n  \"top_functions\": [";
    for (std::size_t i = 0; i < std::min<std::size_t>(20, common.size()); ++i)
      meta << (i ? ", " : "")
           << std::format("[\"{:#x}\", {}]", common[i].second, common[i].first);
    meta << "]\n}\n";

    std::cout << std::format(
        "kept {} of {} tried ({} unstable, {} constant) in {:.1f}s, "
        "{} distinct functions\n",
        kept.load(), tried.load(), unstable.load(), constant.load(), seconds,
        functions.size());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n\nRun with --help.\n";
    return 2;
  }
}

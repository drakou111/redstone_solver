#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "redstone/solver.hpp"
#include "redstone/world.hpp"

namespace rs {

// Gates of the same inputs built by the solver as one module, ready to be
// placed: inputs are repeaters at ins[k] facing into the module (on the
// west face, or the north or south face, see module_input_facing), output i
// is dust on the east face at outs[i]. Function i is its truth table: bit
// (v0 << (k - 1)) | ... | v(k-1) for input values v (so for two inputs, bit
// (a << 1) | b). The west and east faces, and a north or south face with an
// input, are empty but for the ports, so modules can sit next to each other
// and wires can run along them; sharing inputs inside one module keeps
// wires that feed several gates out of the channels.
struct GateModule {
  std::vector<std::uint8_t> functions;
  World layout{{1, 1, 1}};
  std::vector<Vec3> ins;
  std::vector<Vec3> outs;
};

// The truth table of `function` (of `inputs` inputs) with its inputs in a
// new order: input k of the result is input order[k] of `function`.
std::uint8_t permute_function(std::uint8_t function, int inputs,
                              const std::vector<int> &order);

// The way the input repeater at `in` of a module of `size` faces: east on
// the west face (x 0), else south on the north face (z 0) or north on the
// south face.
Dir module_input_facing(Vec3 size, Vec3 in);

// Whether the function's value changes with each of its inputs.
bool depends_on_all(std::uint8_t function, int inputs);

struct GateLibraryOptions {
  // nullopt uses the spec parser's default palette.
  std::optional<std::vector<BlockType>> palette;
  // Boxes tried in order; the first that holds the module is used.
  std::vector<Vec3> sizes{{4, 2, 3}, {5, 2, 3}, {5, 2, 4},
                          {4, 3, 3}, {5, 3, 3}, {5, 3, 4}};
  double time_limit_seconds = 60.0; // total across sizes and output rows
  int horizon = 30;
  SolverOptions solver; // threads and verification placements
  std::atomic<bool> *cancel = nullptr;
};

// Gate modules already built, kept between jobs so the same gate is not
// solved again. Safe to use from several threads.
class GateCache {
public:
  std::optional<GateModule> find(const std::string &key) const;
  void store(const std::string &key, const GateModule &module);

private:
  mutable std::mutex mutex_;
  std::map<std::string, GateModule> modules_;
};

// What makes two module builds interchangeable: the functions, the shapes
// tried, the horizon and how many placements the module is checked in.
std::string gate_cache_key(const std::vector<std::uint8_t> &functions,
                           const GateLibraryOptions &options);

// Where a module's ports sit, all at height 1: input repeaters at these
// cells (see module_input_facing) and output dust on the east face in these
// rows (z).
struct GateArrangement {
  Vec3 size{};
  std::vector<Vec3> ins;
  std::vector<int> out_rows;
};

// Every arrangement in a box of `size` for `inputs` inputs and `outputs`
// outputs, output rows no two next to each other. Inputs sit on the west
// face and, with `sides` (boxes 3 deep or more), also on the north and
// south faces between the corners. Two inputs take any two places in either
// order; three or more take them in one order only, west rows first (the
// same gate with its inputs in another order is another truth table, see
// permute_function).
std::vector<GateArrangement> gate_arrangements(Vec3 size, int inputs,
                                               int outputs, bool sides = false);

// A module for `functions` in exactly this arrangement: the fewest blocks
// found within options.time_limit_seconds, or nothing.
std::optional<GateModule>
build_gate_in(const std::vector<std::uint8_t> &functions,
              const GateArrangement &arrangement,
              const GateLibraryOptions &options);

// What makes catalog modules usable for a request: the blocks allowed and
// how they were checked.
std::string catalog_key(const std::optional<std::vector<BlockType>> &palette,
                        const VerificationOptions &verification);

// Modules in many arrangements and sizes, built ahead of time (each with the
// fewest blocks for its arrangement), for Build from gates to choose from.
// Safe to use from several threads.
class GateCatalog {
public:
  // Keeps the module unless one with the same arrangement and no more
  // blocks is there already. Returns whether it was kept.
  bool add(const std::string &key, const GateModule &module);
  std::vector<GateModule> variants(const std::string &key, int inputs,
                                   const std::vector<std::uint8_t> &functions,
                                   int max_height) const;
  // Whether any module of `inputs` inputs computes exactly `functions`.
  bool has_any(const std::string &key, int inputs,
               const std::vector<std::uint8_t> &functions) const;
  bool has(const std::string &key, const std::vector<std::uint8_t> &functions,
           const GateArrangement &arrangement) const;
  std::vector<std::pair<std::string, GateModule>> all() const;

private:
  mutable std::mutex mutex_;
  // By catalog key, input count and functions.
  std::map<std::tuple<std::string, int, std::vector<std::uint8_t>>,
           std::vector<GateModule>>
      modules_;
};

// Builds a module with one output per function (bit (a << 1) | b of the
// 2-input truth table). Nothing if no size works in time.
std::optional<GateModule> build_gate(const std::vector<std::uint8_t> &functions,
                                     const GateLibraryOptions &options,
                                     const ProgressCallback &progress = {});

} // namespace rs

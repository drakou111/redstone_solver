#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redstone/solver.hpp"
#include "redstone/world.hpp"

namespace rs {

// Gates of the same two inputs built by the solver as one module, ready to
// be placed: inputs are repeaters facing east on the west face at in1 (top
// row) and in2 (bottom row), output i (function i) is dust on the east
// face at outs[i]. Both faces are empty but for the ports, so modules can
// sit next to each other; sharing inputs inside one module keeps wires
// that feed several gates out of the channels.
struct GateModule {
  std::vector<std::uint8_t> functions;
  World layout{{1, 1, 1}};
  Vec3 in1{}, in2{};
  std::vector<Vec3> outs;
};

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

// Builds a module with one output per function (bit (a << 1) | b of the
// 2-input truth table). Nothing if no size works in time.
std::optional<GateModule> build_gate(const std::vector<std::uint8_t> &functions,
                                     const GateLibraryOptions &options,
                                     const ProgressCallback &progress = {});

} // namespace rs

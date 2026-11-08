#pragma once

#include <atomic>
#include <chrono>
#include <optional>

#include "redstone/gate_library.hpp"
#include "redstone/netlist.hpp"
#include "redstone/shrink.hpp"
#include "redstone/world.hpp"

namespace rs {

struct ComposeResult {
  Spec spec;    // the composed box, ports and rows, the circuit as hints
  World layout; // verified
};

// Lays a gate network out in columns, west to east: the input ports, one
// column per logic level (gate modules stacked along z, plus straight
// tracks for signals that pass through to a later column), the output
// ports. Between two columns is a free channel that one solve wires up:
// the cells feeding the next column are temporary outputs ("probes") whose
// truth tables come from the network, so each channel is checked on its
// own while everything west of it stays pinned. Channels may use a spare
// layer above the modules (room for wires to cross); a channel that fails
// is retried one column wider. An unused top layer is dropped at the end.
struct NetlistComposeOptions {
  int channel_width = 1;
  int max_channel_width = 6;
  int extra_layers = 1; // spare layers above the modules for crossings
  // Most layers the circuit may use (0: no limit). Taller gate shapes and
  // spare layers that do not fit are left out.
  int max_height = 0;
  double stage_seconds = 120.0; // total budget for one wiring channel
  int network_candidates = 3;   // same-size Boolean networks tried
  // Total budget to find alternatives; routed in estimated-cost order.
  double network_candidate_seconds = 2.0;
  // Channels narrower than this get a short try first: when they work they
  // are found quickly, and a long failing try costs a whole stage time.
  int comfortable_width = 3;
  double narrow_seconds = 20.0; // per-attempt cap below comfortable_width
  int horizon = 80;
  // nullopt uses the default palette; compose_spec copies the source spec's
  // palette here so module and wiring solves obey the user's restrictions.
  std::optional<std::vector<BlockType>> palette;
  GateLibraryOptions gates;
  SolverOptions solver; // threads and verification placements
  std::atomic<bool> *cancel = nullptr;
  // Flush placement first (compact, see compose_flush); columns if it fails.
  bool flush = true;
  // Lay out in columns when flush placement fails.
  bool columns = true;
  // Let flush placement make ORs (and XORs of signals never on together)
  // from two joining dust lines instead of a module.
  bool flush_joins = true;
  // Modules built before are taken from here (and new ones stored).
  GateCache *gate_cache = nullptr;
  int flush_candidates = 6; // positions tried per gate, best first
  // Exact synthesis of the smallest gate network gives up after this.
  double synthesis_seconds = 60.0;
  // With a deadline, Compose does not stop after one pass: a pass that
  // fails is run again with doubled step limits, wider channels and a new
  // seed until the deadline, and every running solve stops when it passes.
  // The step limits above are the first pass's.
  std::optional<std::chrono::steady_clock::time_point> deadline;
  double flush_seconds = 30.0; // per position
  // Partial circuits kept per gate; each is finished and the smallest wins.
  int flush_beam = 3;
  // Draw wires with the path-finding router (milliseconds) and solve a wire
  // with SAT only where it finds none or its wires fail the check.
  bool flush_router = true;
  // Routed builds are quick: this many are made (spot rankings shuffled a
  // little, in parallel) and the smallest is kept.
  int flush_variants = 8;
  int flush_seed = 0;    // 0: the plain ranking
  bool flush_sat = true; // false: router only, a gate nothing routes fails
  // The build so far, gate by gate (flush placement).
  PreviewCallback preview;
  // The request's timing (compose_spec sets it): builds that meet it are
  // kept over smaller ones that do not.
  TimingSpec timing;
  // Rows beside a wire's straight path it may use, and how many extra cells
  // a gate may sit away from its signal; later deadline passes raise both.
  int flush_margin = 1;
  int flush_spacing = 0;
};

// The composed circuit for `netlist`; its spec has the table's rows (don't
// cares as any strength) and the circuit as hints.
std::optional<ComposeResult>
compose_netlist(const Netlist &netlist, const BoolTable &table,
                const NetlistComposeOptions &options,
                const ProgressCallback &progress = {});

// Places the gate modules one at a time against the circuit so far, each
// lined up with one of its signals, and wires the rest in small corridors.
// Nothing if a gate finds no place.
std::optional<ComposeResult>
compose_flush(const Netlist &netlist, const BoolTable &table,
              const NetlistComposeOptions &options,
              const ProgressCallback &progress = {});

// Composes the spec in its variants (ORs as joined wires or as modules),
// shrinks them side by side (each with the whole time and a share of the
// workers) and keeps the smallest. The shrink is greedy, so a smaller start
// does not always end smaller; trying both is what finds the compact one.
std::optional<ComposeResult>
compose_and_shrink(const Spec &spec, const NetlistComposeOptions &options,
                   const ShrinkOptions &shrink,
                   const ProgressCallback &progress = {});

// The gate library options with every module shape taller than `max_height`
// removed (0 keeps all). Throws std::invalid_argument when none is left.
GateLibraryOptions gates_within_height(GateLibraryOptions gates,
                                       int max_height);

// Reads the spec as an on/off table, finds the smallest gate network and
// composes it. Throws std::invalid_argument for specs that need exact
// strengths.
std::optional<ComposeResult>
compose_spec(const Spec &spec, const NetlistComposeOptions &options,
             const ProgressCallback &progress = {});

} // namespace rs

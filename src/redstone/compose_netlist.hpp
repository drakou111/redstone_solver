#pragma once

#include <atomic>
#include <optional>

#include "redstone/compose.hpp"
#include "redstone/gate_library.hpp"
#include "redstone/netlist.hpp"

namespace rs {

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
  double stage_seconds = 120.0;
  // Channels narrower than this get a short try first: when they work they
  // are found quickly, and a long failing try costs a whole stage time.
  int comfortable_width = 3;
  double narrow_seconds = 20.0;
  int horizon = 80;
  GateLibraryOptions gates;
  SolverOptions solver; // threads and verification placements
  std::atomic<bool> *cancel = nullptr;
};

// The composed circuit for `netlist`; its spec has the table's rows (don't
// cares as any strength) and the circuit as hints.
std::optional<ComposeResult>
compose_netlist(const Netlist &netlist, const BoolTable &table,
                const NetlistComposeOptions &options,
                const ProgressCallback &progress = {});

// Reads the spec as an on/off table, finds the smallest gate network and
// composes it. Throws std::invalid_argument for specs that need exact
// strengths.
std::optional<ComposeResult>
compose_spec(const Spec &spec, const NetlistComposeOptions &options,
             const ProgressCallback &progress = {});

} // namespace rs

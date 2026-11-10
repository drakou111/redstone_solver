#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "redstone/spec.hpp"

namespace rs {

// A truth table over on/off inputs: values[output][vector], nothing where
// the spec does not care. Vector bit i is input i.
struct BoolTable {
  int inputs = 0;
  std::vector<std::vector<std::optional<bool>>> values;
};

// The spec as an on/off table. Throws std::invalid_argument (with the
// reason) when a row depends on exact signal strengths, since a network of
// on/off gates cannot express that.
BoolTable boolean_table(const Spec &spec);

// A 2-input gate: output = function bit ((a << 1) | b) of its operands.
struct Gate {
  int a = 0; // node indices: inputs are 0..inputs-1, gate g is inputs + g
  int b = 0;
  std::uint8_t function = 0;
};

std::string function_name(std::uint8_t function);

// Gates in order (operands come before the gate), and the node each output
// is read from.
struct Netlist {
  int inputs = 0;
  std::vector<Gate> gates;
  std::vector<int> outputs;

  // Value of every node for one input vector.
  std::vector<bool> evaluate(int vector) const;
  // Logic depth of every node (inputs 0).
  std::vector<int> levels() const;
  std::string describe() const;
};

// Reads the text describe() writes ("g1 = AND(IN1, IN2)", "OUT1 = g1", one
// per line; blank lines and lines starting with '#' are skipped). Throws
// std::invalid_argument naming the line when it cannot be read.
Netlist parse_netlist(std::string_view text, int inputs);

// The 2-input functions that depend on both inputs (AND, OR, XOR, NAND,
// NOR, XNOR and the four "one and not the other" forms).
std::vector<std::uint8_t> two_input_functions();

struct SynthesisOptions {
  int max_gates = 10;
  std::vector<std::uint8_t> functions = two_input_functions();
  // Used instead when they give a network just as small.
  std::vector<std::uint8_t> preferred = {0b1000, 0b1110, 0b0110}; // AND OR XOR
  double time_limit_seconds = 60.0;
  int sat_seed = 0;
  std::atomic<bool> *cancel = nullptr;
};

// The smallest network of gates computing every output (exact synthesis by
// SAT, trying 0, 1, 2... gates). Nothing if none within max_gates or time.
std::optional<Netlist> synthesize(const BoolTable &table,
                                  const SynthesisOptions &options = {});

// Finds alternative networks with the same already-known minimum gate count,
// using varied SAT seeds. The first result is `initial`; the remaining time
// budget is spent looking for structurally different networks.
std::vector<Netlist>
synthesize_alternatives(const BoolTable &table, const Netlist &initial,
                        std::size_t max_candidates,
                        const SynthesisOptions &options = {});

// A network that is quick to find but not the smallest: the table split on
// one input at a time ("if IN then f1 else f0"), splitting on the input that
// leaves the simplest halves, with shared sub-networks and don't-cares used
// to simplify. For when exact synthesis takes too long (many inputs).
// Nothing if an output is constant or a half needs an inverted input alone.
std::optional<Netlist> split_network(const BoolTable &table);

} // namespace rs

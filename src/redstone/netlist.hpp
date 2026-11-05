#pragma once

#include <atomic>
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

// The 2-input functions that depend on both inputs (AND, OR, XOR, NAND,
// NOR, XNOR and the four "one and not the other" forms).
std::vector<std::uint8_t> two_input_functions();

struct SynthesisOptions {
  int max_gates = 10;
  std::vector<std::uint8_t> functions = two_input_functions();
  double time_limit_seconds = 60.0;
  std::atomic<bool> *cancel = nullptr;
};

// The smallest network of gates computing every output (exact synthesis by
// SAT, trying 0, 1, 2... gates). Nothing if none within max_gates or time.
std::optional<Netlist> synthesize(const BoolTable &table,
                                  const SynthesisOptions &options = {});

} // namespace rs

#pragma once

// Pieces shared by the two ways of composing a gate network (columns and
// flush placement). Internal to compose*.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "redstone/compose.hpp"
#include "redstone/gate_library.hpp"
#include "redstone/netlist.hpp"

namespace rs::compose_parts {

using Cells = std::map<std::array<int, 3>, Block>;
inline std::array<int, 3> key(Vec3 p) { return {p.x, p.y, p.z}; }

// One row of a stage: input values and wanted outputs (nothing: any).
struct StageRow {
  std::vector<bool> inputs;
  std::vector<std::optional<bool>> outputs;
};

// A spec with repeater inputs and dust outputs at the given cells, on/off
// rows and no deadline (only coming to rest within `horizon`).
Spec make_spec(Vec3 size, const std::vector<Vec3> &inputs,
               const std::vector<Vec3> &outputs,
               const std::vector<StageRow> &rows, int horizon,
               const std::optional<std::vector<BlockType>> &palette);

// Blocks of a layout moved by `offset`, ports turned into the plain blocks
// they are (repeater inputs, dust outputs).
void add_layout(Cells &cells, const World &layout, Vec3 offset);

// Keeps every cell of `cells` and leaves the rest empty unless `free` says
// the solver may use it. Port cells are left to the spec.
void pin(Spec &spec, const Cells &cells, const std::function<bool(Vec3)> &free);

// Gates of one level that read the same signals: one module. Two operands
// for a gate of the network, three for a fused piece (see fused_groupings).
struct Group {
  std::vector<int> operands;           // in module input order
  std::vector<int> nodes;              // gate nodes, in module output order
  std::vector<std::uint8_t> functions; // truth tables over the operands
};

// 3-input truth tables (first operand the highest bit) worth a name.
constexpr std::uint8_t kXor3Function = 0x96;
constexpr std::uint8_t kMajorityFunction = 0xE8;

// The group's functions by name ("XOR+AND", "XOR3").
std::string group_name(const Group &group);

// Ways to build the network where pieces that read only three signals are
// one module each, and gates that read only a piece's signals may join it
// as more outputs: `has_module` says whether a module exists for a list of
// truth tables over the piece's signals (in increasing node order; the
// piece's own first). Each grouping places fewer modules than the plain
// one; the fewest first, each in signal order and then (when that differs)
// with the pieces placed as late as they can be; at most `max_groupings`.
// Nothing when no piece has a module.
std::vector<std::vector<Group>> fused_groupings(
    const Netlist &netlist,
    const std::function<bool(const std::vector<std::uint8_t> &)> &has_module,
    int max_groupings);

// Flush placement (see compose_flush) of these groups, in this order.
std::optional<ComposeResult>
compose_flush_groups(const Netlist &netlist, const BoolTable &table,
                     const std::vector<Group> &groups,
                     const NetlistComposeOptions &options,
                     const ProgressCallback &progress);

std::vector<Group> group_gates(const Netlist &netlist,
                               const std::vector<int> &level);

// The circuit with its inputs down the west side in order (IN1 first):
// moved east, with the crossing wires west of it drawn by the router. The
// same circuit when they already are; nothing if no crossing fits.
std::optional<ComposeResult>
with_inputs_in_order(const ComposeResult &built,
                     const NetlistComposeOptions &options,
                     const ProgressCallback &progress);

// The circuit with its outputs down the east side in order (OUT1 first, or
// last when options.allow_reversed_outputs): a crossing region added east
// of it. The same circuit when they already are; nothing if no crossing
// fits.
std::optional<ComposeResult>
with_outputs_in_order(const ComposeResult &built,
                      const NetlistComposeOptions &options,
                      const ProgressCallback &progress);

// One module per distinct function list of the 2-operand groups, or
// nothing if one cannot be built (fused groups take catalog modules).
// `module_height` is the tallest module's height (at least 2).
std::optional<std::map<std::vector<std::uint8_t>, GateModule>>
build_modules(const std::vector<Group> &groups,
              const NetlistComposeOptions &options, int &module_height,
              const ProgressCallback &progress);

} // namespace rs::compose_parts

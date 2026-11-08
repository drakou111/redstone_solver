#pragma once

// Pieces shared by the two ways of composing a gate network (columns and
// flush placement). Internal to compose*.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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

// Gates of one level that read the same two signals: one module.
struct Group {
  int a = 0, b = 0;       // operand nodes
  std::vector<int> nodes; // gate nodes, in module output order
  std::vector<std::uint8_t> functions;
};

std::vector<Group> group_gates(const Netlist &netlist,
                               const std::vector<int> &level);

// One module per distinct group function list, or nothing if one cannot be
// built. `module_height` is the tallest module's height (at least 2).
std::optional<std::map<std::vector<std::uint8_t>, GateModule>>
build_modules(const std::vector<Group> &groups,
              const NetlistComposeOptions &options, int &module_height,
              const ProgressCallback &progress);

} // namespace rs::compose_parts

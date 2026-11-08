#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "redstone/block.hpp"
#include "redstone/geometry.hpp"

namespace rs {

// Where a wire may begin: a dust cell already carrying the signal, or a
// repeater (an input port) whose output leaves along `output`.
struct WireStart {
  Vec3 cell;
  int strength = kMaxSignal; // the weakest the signal is there when on
  std::optional<Dir> output; // set for a repeater
};

// Where a wire ends.
//   Feed: dust or a repeater at `cell` feeding the component at
//   cell + offset(into), such as a gate module's input repeater.
//   Dust: dust at `cell`, the start of a joined-wire OR.
//   Join: dust (or a repeater facing it) beside the dust at `cell`, on the
//   same level, joining it.
struct WireGoal {
  enum class Kind { Feed, Dust, Join };
  Vec3 cell;
  Dir into = Dir::East;
  Kind kind = Kind::Feed;
};

struct WireRouterOptions {
  Vec3 low{1, 1, 0};     // wire cells (dust, repeaters) lie in [low, high];
  Vec3 high{1, 1, 1};    // their supports one cell lower
  bool repeaters = true; // may refresh a weak signal
  int max_expansions = 400000;
};

// Draws dust-on-block wires with repeaters where the signal runs low, by
// A* over the grid. A wire keeps one cell of air from every other block in
// all directions but straight up, so it cannot join or power anything else;
// it crosses another wire two levels above it, on a block resting on the
// other wire's dust. Wires are checked by simulation afterwards; the rules
// keep the search from proposing wires that obviously fail.
class WireRouter {
public:
  using Cells = std::map<std::array<int, 3>, Block>;

  // `cells`: everything already placed. `blocked`: cells nothing may use
  // (gate module footprints), even if empty.
  WireRouter(const Cells &cells, std::function<bool(Vec3)> blocked,
             WireRouterOptions options);

  // The blocks of a wire from any start to the goal (wire cells and the
  // blocks they stand on), or nothing. `own`: cells carrying the same
  // signal, which the wire may touch until its first repeater.
  std::optional<std::vector<std::pair<Vec3, Block>>>
  route(const std::vector<WireStart> &starts,
        const std::set<std::array<int, 3>> &own, const WireGoal &goal) const;

private:
  const Cells &cells_;
  std::function<bool(Vec3)> blocked_;
  WireRouterOptions options_;
};

} // namespace rs

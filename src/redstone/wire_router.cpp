#include "redstone/wire_router.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>

namespace rs {

namespace {

constexpr Vec3 kUp{0, 1, 0};
constexpr int kDustKinds = kMaxSignal + 1; // strengths 1..15 (0 unused)
constexpr int kKinds = kDustKinds + 4;     // then a repeater per facing
constexpr int kDustCost = 2;               // dust and its block
constexpr int kSlopeCost = 1;              // extra for changing level
constexpr int kRepeaterCost = 3;
// Extra for rebuilding another wire to cross it on the same level.
constexpr int kRewireCost = 4;
constexpr int kNoParent = -1;

int horizontal_index(Dir d) {
  for (int i = 0; i < 4; ++i)
    if (kHorizontalDirs[i] == d)
      return i;
  throw std::invalid_argument("not a horizontal direction");
}

// A step of a wire: dust or a repeater at `cell` on a block at cell - up.
// Starts are steps that place nothing.
struct Step {
  Vec3 cell;
  int kind = 0; // 1..15 dust strength, kDustKinds + i repeater facing i
  bool isolated = false; // a repeater lies between the start and here
  bool placed = true;
  std::optional<Vec3> keep_air; // must stay empty (a slope's corner)
  // A repeater crossing another wire below it: that wire's cells beside the
  // block become repeaters facing this way (its signal's direction).
  std::optional<Dir> rewire;
  int parent = kNoParent;
  int cost = 0;
};

bool is_repeater_kind(int kind) { return kind >= kDustKinds; }

// The cells near the search area, flattened: what is there and whether it
// carries the routed signal.
class Area {
public:
  Area(const WireRouter::Cells &cells, Vec3 low, Vec3 high)
      : low_{low.x - 1, 0, low.z - 1},
        high_{high.x + 1, high.y + 2, high.z + 1} {
    this->size_ = this->high_ - this->low_ + Vec3{1, 1, 1};
    this->blocks_.assign(static_cast<std::size_t>(this->size_.x) *
                             this->size_.y * this->size_.z,
                         std::nullopt);
    this->own_.assign(this->blocks_.size(), false);
    for (const auto &[cell, block] : cells) {
      Vec3 p{cell[0], cell[1], cell[2]};
      if (this->contains(p) && block.type != BlockType::Air)
        this->blocks_[this->index(p)] = block;
    }
  }

  bool contains(Vec3 p) const {
    return p.x >= this->low_.x && p.y >= this->low_.y && p.z >= this->low_.z &&
           p.x <= this->high_.x && p.y <= this->high_.y && p.z <= this->high_.z;
  }
  std::size_t index(Vec3 p) const {
    Vec3 d = p - this->low_;
    return (static_cast<std::size_t>(d.y) * this->size_.z + d.z) *
               this->size_.x +
           d.x;
  }
  std::size_t volume() const { return this->blocks_.size(); }

  // Nothing outside the area matters to the search: empty.
  std::optional<Block> at(Vec3 p) const {
    return this->contains(p) ? this->blocks_[this->index(p)] : std::nullopt;
  }
  bool own(Vec3 p) const {
    return this->contains(p) && this->own_[this->index(p)];
  }
  void mark_own(Vec3 p) {
    if (this->contains(p))
      this->own_[this->index(p)] = true;
  }

private:
  Vec3 low_, high_, size_;
  std::vector<std::optional<Block>> blocks_;
  std::vector<bool> own_;
};

} // namespace

WireRouter::WireRouter(const Cells &cells, std::function<bool(Vec3)> blocked,
                       WireRouterOptions options)
    : cells_(cells), blocked_(std::move(blocked)), options_(options) {
  if (this->options_.low.y < 1)
    throw std::invalid_argument("wires need room for a block below them");
}

std::optional<std::vector<std::pair<Vec3, Block>>>
WireRouter::route(const std::vector<WireStart> &starts,
                  const std::set<std::array<int, 3>> &own, const WireGoal &goal,
                  const std::map<std::array<int, 3>, int> &strengths) const {
  const Vec3 low = this->options_.low;
  const Vec3 high = this->options_.high;
  Area area(this->cells_, low, high);
  for (const auto &cell : own)
    area.mark_own({cell[0], cell[1], cell[2]});
  using GoalKind = WireGoal::Kind;
  const Vec3 component = goal.cell + offset(goal.into);
  auto at_goal_cell = [&](Vec3 q) {
    if (goal.kind != GoalKind::Join)
      return q == goal.cell;
    return q.y == goal.cell.y &&
           std::abs(q.x - goal.cell.x) + std::abs(q.z - goal.cell.z) == 1;
  };

  auto in_bounds = [&](Vec3 p) {
    return p.x >= low.x && p.y >= low.y && p.z >= low.z && p.x <= high.x &&
           p.y <= high.y && p.z <= high.z;
  };
  const Vec3 span = high - low + Vec3{1, 1, 1};
  auto state_index = [&](Vec3 p, int kind, bool isolated) {
    Vec3 d = p - low;
    std::size_t cell =
        (static_cast<std::size_t>(d.y) * span.z + d.z) * span.x + d.x;
    return (cell * kKinds + kind) * 2 + (isolated ? 1 : 0);
  };
  std::vector<int> best(static_cast<std::size_t>(span.x) * span.y * span.z *
                            kKinds * 2,
                        std::numeric_limits<int>::max());
  auto estimate = [&](Vec3 p) {
    return kDustCost *
               (std::abs(p.x - goal.cell.x) + std::abs(p.z - goal.cell.z)) +
           std::abs(p.y - goal.cell.y);
  };

  std::vector<Step> steps;
  using Entry = std::pair<int, int>; // cost + estimate, step
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
  for (const WireStart &start : starts) {
    Step s;
    s.cell = start.cell;
    s.placed = false;
    if (start.output) {
      s.kind = kDustKinds + horizontal_index(*start.output);
    } else {
      if (start.strength < 1)
        continue;
      s.kind = std::min(start.strength, kMaxSignal);
    }
    steps.push_back(s);
    open.push({estimate(s.cell), static_cast<int>(steps.size()) - 1});
  }

  // Cells of the wire leading to the step being expanded: stamped with the
  // expansion number so membership is one lookup.
  std::vector<int> stamp(area.volume(), -1);
  int expansion = 0;
  auto on_path = [&](Vec3 p) {
    return area.contains(p) && stamp[area.index(p)] == expansion;
  };
  // Cells a crossing is about to rebuild: seen as their new blocks.
  std::vector<std::pair<Vec3, Block>> overlay;
  auto look = [&](Vec3 p) -> std::optional<Block> {
    for (const auto &[cell, block] : overlay)
      if (cell == p)
        return block;
    return area.at(p);
  };
  auto strength_at = [&](Vec3 p) -> std::optional<int> {
    auto it = strengths.find({p.x, p.y, p.z});
    if (it == strengths.end())
      return std::nullopt;
    return it->second;
  };

  // Can a wire cell go at `q` (on a new block at q - up), coming from the
  // step `from`, whose cells it may touch? `repeater`: the cell holds a
  // repeater facing that way, not dust.
  auto fits = [&](Vec3 q, const Step &from, std::optional<Vec3> keep_air,
                  bool at_goal, std::optional<Dir> repeater) {
    if (!in_bounds(q))
      return false;
    const Vec3 support = q - kUp;
    for (Vec3 c : {q, support}) {
      if (area.at(c) || this->blocked_(c) || on_path(c))
        return false;
    }
    if (keep_air && (area.at(*keep_air) || on_path(*keep_air)))
      return false;
    const Vec3 last_support = from.cell - kUp;
    // A neighbour `n` beside the wire cell (dy 0), beside its block (dy -1)
    // or diagonally above it (dy 1), seen in direction `h`.
    auto touches_ok = [&](Vec3 n, Dir h, int dy) {
      if (n == from.cell || (from.placed && n == last_support))
        return true;
      if (!from.placed && n == from.cell - kUp)
        return true; // the start's own block
      if (on_path(n))
        return false;
      std::optional<Block> b = look(n);
      if (!b)
        return true;
      if (at_goal && goal.kind == GoalKind::Feed &&
          (n == component || n == component - kUp))
        return true;
      if (at_goal && goal.kind == GoalKind::Join &&
          (n == goal.cell || n == goal.cell - kUp))
        return true;
      if (!from.isolated && area.own(n))
        return true;
      if (repeater && !(dy == 0 && same_axis(*repeater, h))) {
        // Beside a repeater: only another repeater or comparator facing
        // into its side locks it. Its block carries no power, but must not
        // pass power from one neighbour to another.
        if (dy == 0)
          return !(is_diode(*b) || connects_along_axis(*b)) ||
                 n + offset(b->facing) != q;
        if (dy == -1)
          return is_dust(*b) || b->type == BlockType::Solid ||
                 b->type == BlockType::Glass;
        return true;
      }
      // A repeater ignores dust and blocks at its sides and powers only
      // what is in front of it.
      if (connects_along_axis(*b) && !same_axis(b->facing, h))
        return true;
      if (dy == 1 && connects_along_axis(*b))
        return true;
      // Plain blocks pass nothing to each other, and dust does not reach a
      // block diagonally above it.
      bool plain = b->type == BlockType::Solid || b->type == BlockType::Glass;
      return plain && dy != 0;
    };
    for (Dir h : kHorizontalDirs)
      for (int dy = -1; dy <= 1; ++dy)
        if (!touches_ok(q + offset(h) + Vec3{0, dy, 0}, h, dy))
          return false;
    // Above: only another wire's block, crossing over this one.
    const Vec3 above = q + kUp;
    if (on_path(above))
      return false;
    if (std::optional<Block> b = area.at(above)) {
      std::optional<Block> top = area.at(above + kUp);
      bool crossing = b->type == BlockType::Solid && top && is_dust(*top);
      if (!crossing && !(!from.isolated && area.own(above)))
        return false;
    }
    // Below the block: air, or dust or a block of a wire this one crosses.
    const Vec3 below = support - kUp;
    if (on_path(below))
      return false;
    if (std::optional<Block> b = area.at(below))
      if (!is_dust(*b) && b->type != BlockType::Solid)
        return false;
    return true;
  };

  int found = kNoParent;
  while (!open.empty() && expansion < this->options_.max_expansions) {
    auto [priority, index] = open.top();
    open.pop();
    const Step current = steps[index];
    if (current.placed &&
        best[state_index(current.cell, current.kind, current.isolated)] <
            current.cost)
      continue;
    if (current.placed && at_goal_cell(current.cell)) {
      found = index;
      break;
    }
    ++expansion;
    for (int i = index; i != kNoParent; i = steps[i].parent) {
      const Step &s = steps[i];
      if (!s.placed)
        continue;
      for (Vec3 c : {s.cell, s.cell - kUp})
        if (area.contains(c))
          stamp[area.index(c)] = expansion;
      if (s.keep_air && area.contains(*s.keep_air))
        stamp[area.index(*s.keep_air)] = expansion;
      if (s.rewire)
        for (Vec3 c : {s.cell - kUp - offset(*s.rewire),
                       s.cell - kUp + offset(*s.rewire)})
          if (area.contains(c))
            stamp[area.index(c)] = expansion;
    }

    auto push = [&](Vec3 q, int kind, bool isolated, int cost,
                    std::optional<Vec3> keep_air) {
      bool at_goal = at_goal_cell(q);
      if (at_goal && is_repeater_kind(kind)) {
        Dir facing = kHorizontalDirs[kind - kDustKinds];
        bool feeds = goal.kind == GoalKind::Feed ? facing == goal.into
                     : goal.kind == GoalKind::Join
                         ? q + offset(facing) == goal.cell
                         : false;
        if (!feeds)
          return;
      }
      std::optional<Dir> repeater;
      if (is_repeater_kind(kind))
        repeater = kHorizontalDirs[kind - kDustKinds];
      if (!fits(q, current, keep_air, at_goal, repeater))
        return;
      std::size_t key = state_index(q, kind, isolated);
      if (best[key] <= cost)
        return;
      best[key] = cost;
      Step next;
      next.cell = q;
      next.kind = kind;
      next.isolated = isolated;
      next.keep_air = keep_air;
      next.parent = index;
      next.cost = cost;
      steps.push_back(next);
      open.push({cost + estimate(q), static_cast<int>(steps.size()) - 1});
    };

    // Over a straight dust line one level down, heading `h`: dust at a,
    // a repeater at r on the line's cell c (now a block, the line's
    // neighbours of c repeaters), dust at b.
    auto cross_level = [&](Dir h) {
      const Vec3 a = current.cell + offset(h);
      const Vec3 r = a + offset(h);
      const Vec3 b = r + offset(h);
      const Vec3 c = r - kUp;
      if (c.y < low.y || at_goal_cell(a) || at_goal_cell(r))
        return;
      std::optional<Block> line = area.at(c);
      if (!line || line->type != BlockType::Dust || area.own(c) || on_path(c))
        return;
      const Dir d = rotate_clockwise(h);
      const Vec3 before = c - offset(d);
      const Vec3 after = c + offset(d);
      for (Vec3 p : {before, after, before - offset(d), after + offset(d)}) {
        std::optional<Block> x = area.at(p);
        if (!x || x->type != BlockType::Dust || on_path(p))
          return;
      }
      std::optional<int> s_before = strength_at(before);
      std::optional<int> s_after = strength_at(after);
      if (!s_before || !s_after || *s_before == *s_after)
        return;
      const Dir flow = *s_before > *s_after ? d : opposite(d);
      // The line runs straight there: nothing else joins it beside c.
      for (Vec3 p : {before, after})
        for (Dir side : {h, opposite(h)})
          for (int dy = -1; dy <= 1; ++dy) {
            Vec3 n = p + offset(side) + Vec3{0, dy, 0};
            std::optional<Block> x = area.at(n);
            if (x && is_dust(*x))
              return;
          }
      if (!in_bounds(r) || area.at(r) || this->blocked_(r) || on_path(r))
        return;
      for (Vec3 n : {r - offset(d), r + offset(d), r + kUp})
        if (on_path(n) ||
            (area.at(n) && !(area.at(n)->type == BlockType::Solid ||
                             area.at(n)->type == BlockType::Glass)))
          return;

      overlay = {{c, Block::solid()}};
      bool a_fits = fits(a, current, std::nullopt, false, std::nullopt);
      if (!a_fits) {
        overlay.clear();
        return;
      }
      Step step_a;
      step_a.cell = a;
      step_a.kind = current.kind - 1;
      step_a.isolated = current.isolated;
      step_a.parent = index;
      Step step_r;
      step_r.cell = r;
      step_r.kind = kDustKinds + horizontal_index(h);
      step_r.isolated = true;
      step_r.rewire = flow;
      overlay = {{c, Block::solid()},
                 {before, Block::repeater(flow)},
                 {after, Block::repeater(flow)}};
      bool b_fits =
          fits(b, step_r, std::nullopt, at_goal_cell(b), std::nullopt);
      overlay.clear();
      if (!b_fits)
        return;
      const int cost =
          current.cost + 2 * kDustCost + kRepeaterCost + kRewireCost;
      std::size_t key = state_index(b, kMaxSignal, true);
      if (best[key] <= cost)
        return;
      best[key] = cost;
      steps.push_back(step_a);
      step_r.parent = static_cast<int>(steps.size()) - 1;
      steps.push_back(step_r);
      Step step_b;
      step_b.cell = b;
      step_b.kind = kMaxSignal;
      step_b.isolated = true;
      step_b.parent = static_cast<int>(steps.size()) - 1;
      step_b.cost = cost;
      steps.push_back(step_b);
      open.push({cost + estimate(b), static_cast<int>(steps.size()) - 1});
    };

    if (is_repeater_kind(current.kind)) {
      // A repeater's signal leaves straight ahead only.
      Dir h = kHorizontalDirs[current.kind - kDustKinds];
      Vec3 q = current.cell + offset(h);
      bool isolated = current.isolated || current.placed;
      push(q, kMaxSignal, isolated, current.cost + kDustCost, std::nullopt);
      if (this->options_.repeaters)
        push(q, current.kind, true, current.cost + kRepeaterCost, std::nullopt);
    } else {
      const int strength = current.kind;
      for (Dir h : kHorizontalDirs) {
        Vec3 flat = current.cell + offset(h);
        if (strength > 1) {
          push(flat, strength - 1, current.isolated, current.cost + kDustCost,
               std::nullopt);
          push(flat + kUp, strength - 1, current.isolated,
               current.cost + kDustCost + kSlopeCost, current.cell + kUp);
          push(flat - kUp, strength - 1, current.isolated,
               current.cost + kDustCost + kSlopeCost, flat);
        }
        if (this->options_.repeaters)
          push(flat, kDustKinds + horizontal_index(h), true,
               current.cost + kRepeaterCost, std::nullopt);
        if (strength > 1 && this->options_.repeaters && !strengths.empty())
          cross_level(h);
      }
    }
  }
  if (found == kNoParent)
    return std::nullopt;

  std::vector<std::pair<Vec3, Block>> blocks;
  for (int i = found; i != kNoParent; i = steps[i].parent) {
    const Step &s = steps[i];
    if (!s.placed)
      continue;
    Block wire = is_repeater_kind(s.kind)
                     ? Block::repeater(kHorizontalDirs[s.kind - kDustKinds])
                     : Block::dust();
    blocks.emplace_back(s.cell, wire);
    blocks.emplace_back(s.cell - kUp, Block::solid());
    if (s.rewire) {
      Vec3 c = s.cell - kUp;
      blocks.emplace_back(c - offset(*s.rewire), Block::repeater(*s.rewire));
      blocks.emplace_back(c + offset(*s.rewire), Block::repeater(*s.rewire));
    }
  }
  std::reverse(blocks.begin(), blocks.end());
  return blocks;
}

} // namespace rs

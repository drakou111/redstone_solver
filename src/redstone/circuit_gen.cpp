#include "redstone/circuit_gen.hpp"

#include <algorithm>
#include <cstdlib>

#include "redstone/simulator.hpp"

namespace rs::gen {

namespace {

bool supported_on_top(const rs::World &w, rs::Vec3 p) {
  rs::Vec3 below = p + rs::offset(rs::Dir::Down);
  return w.in_bounds(below) && rs::supports_on_top(w.at(below));
}

} // namespace

// A random block for cell p that the cells below and around allow.
rs::Block random_block(const rs::World &w, rs::Vec3 p, std::mt19937_64 &rng,
                       const Mix &mix) {
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  auto horizontal = [&] { return rs::kHorizontalDirs[rng() % 4]; };
  double r = unit(rng);
  bool on_top = supported_on_top(w, p);
  if ((r -= mix.air) < 0)
    return rs::Block::air();
  if ((r -= mix.solid) < 0)
    return rs::Block::solid();
  if ((r -= mix.dust) < 0)
    return on_top ? rs::Block::dust() : rs::Block::air();
  if ((r -= mix.torch) < 0) {
    std::vector<rs::Dir> ways;
    if (on_top)
      ways.push_back(rs::Dir::Down);
    for (rs::Dir d : rs::kHorizontalDirs) {
      rs::Vec3 q = p + rs::offset(d);
      if (w.in_bounds(q) && rs::supports_on_side(w.at(q)))
        ways.push_back(d);
    }
    if (ways.empty())
      return rs::Block::air();
    return rs::Block::torch(ways[rng() % ways.size()]);
  }
  if ((r -= mix.repeater) < 0)
    return on_top ? rs::Block::repeater(horizontal(), 1 + rng() % 4)
                  : rs::Block::air();
  return on_top ? rs::Block::comparator(horizontal(),
                                        rng() % 2 ? rs::ComparatorMode::Subtract
                                                  : rs::ComparatorMode::Compare)
                : rs::Block::air();
}

// The spec's ports on a random floor, filled layer by layer from the bottom
// so every block has what it stands or hangs on.
rs::World random_layout(const rs::Spec &spec, std::mt19937_64 &rng) {
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  rs::World w = spec.base_world();
  rs::Vec3 size = w.size();
  Mix mix;
  for (int x = 0; x < size.x; ++x)
    for (int z = 0; z < size.z; ++z) {
      rs::Vec3 p{x, 0, z};
      if (w.at(p).type != rs::BlockType::Air)
        continue;
      rs::Vec3 above{x, 1, z};
      bool port_above =
          w.in_bounds(above) && w.at(above).type != rs::BlockType::Air;
      if (port_above || unit(rng) < kFloorSolid)
        w.set(p, rs::Block::solid());
    }
  for (int y = 1; y < size.y; ++y) {
    std::vector<rs::Vec3> cells;
    for (int x = 0; x < size.x; ++x)
      for (int z = 0; z < size.z; ++z)
        if (w.at({x, y, z}).type == rs::BlockType::Air)
          cells.push_back({x, y, z});
    std::shuffle(cells.begin(), cells.end(), rng);
    for (rs::Vec3 p : cells)
      w.set(p, random_block(w, p, rng, mix));
  }
  return w;
}

std::vector<rs::InputVector> on_off_vectors(int inputs) {
  std::vector<rs::InputVector> vectors;
  for (int bits = 0; bits < (1 << inputs); ++bits) {
    rs::InputVector v;
    for (int i = 0; i < inputs; ++i)
      v.push_back((bits >> i) & 1 ? kOn : 0);
    vectors.push_back(v);
  }
  return vectors;
}

namespace {

// Wires grown from the inputs toward the outputs (and into comparator
// sides), with random components on the way, so most circuits react to
// their inputs. Wires that run into each other merge.
class Grower {
public:
  Grower(const rs::Spec &spec, std::mt19937_64 &rng)
      : spec_(spec), rng_(rng), w_(spec.base_world()) {
    rs::Vec3 size = this->w_.size();
    for (int x = 0; x < size.x; ++x)
      for (int z = 0; z < size.z; ++z)
        if (this->w_.at({x, 0, z}).type == rs::BlockType::Air)
          this->w_.set({x, 0, z}, rs::Block::solid());
  }

  // Continues an existing circuit.
  Grower(const rs::Spec &spec, std::mt19937_64 &rng, rs::World w)
      : spec_(spec), rng_(rng), w_(std::move(w)) {
    for (int i = 0; i < this->w_.volume(); ++i) {
      rs::Vec3 p = this->w_.position_of(i);
      if (this->w_.at(p).type == rs::BlockType::Comparator)
        this->comparators_.push_back(p);
    }
  }

  // One more wire: from an input or off an existing wire, to a comparator
  // side or a random cell.
  rs::World with_random_wire() {
    rs::Vec3 size = this->w_.size();
    rs::Vec3 start = this->random_start();
    if (this->rng_() % 2) {
      rs::Vec3 p{static_cast<int>(this->rng_() % size.x), 1,
                 static_cast<int>(this->rng_() % size.z)};
      if (this->w_.at(p).type == rs::BlockType::Dust)
        start = p + rs::offset(rs::kHorizontalDirs[this->rng_() % 4]);
    }
    rs::Vec3 goal{static_cast<int>(this->rng_() % size.x), 1,
                  static_cast<int>(this->rng_() % size.z)};
    if (!this->comparators_.empty() && this->rng_() % 2) {
      rs::Vec3 c = this->comparators_[this->rng_() % this->comparators_.size()];
      rs::Dir facing = this->w_.at(c).facing;
      rs::Dir side = this->rng_() % 2 ? rs::rotate_clockwise(facing)
                                      : rs::rotate_counter_clockwise(facing);
      goal = c + rs::offset(side);
    }
    if (this->w_.in_bounds(goal))
      this->grow(start, goal, true);
    return this->w_;
  }

  // Chained logic on purpose: each output computes a random expression of
  // depth up to two over the inputs, built from subtract comparators (rear
  // minus side) and dust joins, wired with plain climbing wires.
  rs::World build_motifs() {
    for (std::size_t o = 0; o < this->spec_.outputs.size(); ++o) {
      const rs::OutputPort &port = this->spec_.outputs[o];
      bool dust = port.kind == rs::OutputKind::Dust;
      rs::Vec3 goal =
          dust ? port.position : port.position - rs::offset(port.facing);
      std::array<int, 3> in{};
      int inputs = static_cast<int>(this->spec_.inputs.size());
      for (int k = 0; k < 3; ++k)
        in[k] = static_cast<int>(this->rng_() % inputs);
      if (inputs >= 3) {
        // distinct inputs, so the recipes compute what their names say
        std::array<int, 3> order{0, 1, 2};
        std::shuffle(order.begin(), order.end(), this->rng_);
        in = order;
      }
      this->expr_.clear();
      std::optional<rs::Vec3> source = this->embed(this->recipe(in));
      if (!source)
        continue;
      if (auto from = this->branch(*source))
        this->grow(*from, goal, !dust, true);
    }
    return this->w_;
  }

  rs::World build() {
    for (std::size_t o = 0; o < this->spec_.outputs.size(); ++o) {
      const rs::OutputPort &port = this->spec_.outputs[o];
      bool dust = port.kind == rs::OutputKind::Dust;
      rs::Vec3 goal =
          dust ? port.position : port.position - rs::offset(port.facing);
      int wires = 1 + static_cast<int>(this->rng_() % kMaxWiresPerOutput);
      for (int k = 0; k < wires; ++k)
        this->grow(this->random_start(), goal, !dust);
    }
    // Side inputs make the comparators compute something.
    std::vector<rs::Vec3> comparators = this->comparators_;
    for (rs::Vec3 c : comparators) {
      if (this->unit() > kSideWireOdds)
        continue;
      rs::Dir facing = this->w_.at(c).facing;
      rs::Dir side = this->rng_() % 2 ? rs::rotate_clockwise(facing)
                                      : rs::rotate_counter_clockwise(facing);
      rs::Vec3 goal = c + rs::offset(side);
      if (this->free(goal))
        this->grow(this->random_start(), goal, true);
    }
    return this->w_;
  }

private:
  static constexpr int kMaxWiresPerOutput = 3;
  static constexpr int kMaxSteps = 40;
  static constexpr double kSideWireOdds = 0.8;
  static constexpr double kTowardGoal = 0.7;
  static constexpr double kRepeater = 0.12;
  static constexpr double kComparator = 0.14;
  static constexpr double kInverter = 0.12;
  // Wires climb onto blocks and come back down, so they can cross others.
  static constexpr double kClimb = 0.15;
  static constexpr double kDescend = 0.25;

  double unit() { return std::uniform_real_distribution<double>(0, 1)(rng_); }

  bool free(rs::Vec3 p) const {
    if (!this->w_.in_bounds(p) || p.y < 1 ||
        this->w_.at(p).type != rs::BlockType::Air)
      return false;
    return !this->is_input_start(p);
  }

  bool is_input_start(rs::Vec3 p) const {
    for (std::size_t i = 0; i < this->spec_.inputs.size(); ++i)
      if (p == this->input_start(static_cast<int>(i)))
        return true;
    return false;
  }

  // Gives p something to stand on: a solid block below if that cell is
  // empty. False when the cell below holds something else.
  bool prop_up(rs::Vec3 p) {
    rs::Vec3 below = p + rs::offset(rs::Dir::Down);
    if (rs::supports_on_top(this->w_.at(below)))
      return true;
    if (below.y >= 1 && this->w_.at(below).type == rs::BlockType::Air &&
        !this->is_input_start(below)) {
      this->w_.set(below, rs::Block::solid());
      return true;
    }
    return false;
  }

  static constexpr int kMotifDepth = 2;
  static constexpr double kMotifLeaf = 0.25;
  static constexpr double kMotifJoin = 0.3;

  rs::Vec3 random_cell() {
    rs::Vec3 size = this->w_.size();
    return {static_cast<int>(this->rng_() % size.x), 1,
            static_cast<int>(this->rng_() % size.z)};
  }

  // A free cell next to `p` (same level) to start a branch from.
  std::optional<rs::Vec3> branch(rs::Vec3 p) {
    std::array<rs::Dir, 4> dirs = rs::kHorizontalDirs;
    std::shuffle(dirs.begin(), dirs.end(), this->rng_);
    for (rs::Dir d : dirs) {
      rs::Vec3 q = p + rs::offset(d);
      if (this->free(q))
        return q;
    }
    return std::nullopt;
  }

  // An expression over the inputs: an input, a minus b (subtract
  // comparator) or a or b (dust join). Nodes refer to earlier nodes.
  struct Expr {
    char op = 'i';
    int a = -1;
    int b = -1;
    int input = 0;
  };

  int leaf(int input) {
    this->expr_.push_back({'i', -1, -1, input});
    return static_cast<int>(this->expr_.size()) - 1;
  }
  int node(char op, int a, int b) {
    this->expr_.push_back({op, a, b, 0});
    return static_cast<int>(this->expr_.size()) - 1;
  }
  int sub(int a, int b) { return this->node('s', a, b); }
  int join(int a, int b) { return this->node('o', a, b); }
  int exclusive(int a, int b) {
    return this->join(this->sub(a, b), this->sub(b, a));
  }
  int both(int a, int b) { return this->sub(a, this->sub(a, b)); }
  int majority(int a, int b, int c) {
    return this->join(this->both(a, b), this->both(c, this->join(a, b)));
  }

  // A random recipe (the logic adders and comparators are made of), or a
  // random tree; `inputs` are three distinct-if-possible input indices.
  int recipe(const std::array<int, 3> &in) {
    auto x = [&] { return this->leaf(in[0]); };
    auto y = [&] { return this->leaf(in[1]); };
    auto z = [&] { return this->leaf(in[2]); };
    double r = this->unit();
    if ((r -= 0.25) < 0)
      return this->exclusive(x(), y());
    if ((r -= 0.15) < 0)
      return this->exclusive(this->exclusive(x(), y()), z());
    if ((r -= 0.15) < 0)
      return this->majority(x(), y(), z());
    if ((r -= 0.15) < 0)
      return this->both(x(), y());
    if ((r -= 0.1) < 0)
      return this->sub(x(), y());
    if ((r -= 0.1) < 0)
      return this->join(x(), y());
    return this->random_tree(kMotifDepth);
  }

  int random_tree(int depth) {
    int inputs = static_cast<int>(this->spec_.inputs.size());
    double r = this->unit();
    if (depth == 0 || r < kMotifLeaf)
      return this->leaf(static_cast<int>(this->rng_() % inputs));
    int a = this->random_tree(depth - 1);
    int b = this->random_tree(depth - 1);
    return r < kMotifLeaf + kMotifJoin ? this->join(a, b) : this->sub(a, b);
  }

  rs::Vec3 input_start(int i) const {
    const rs::InputPort &port = this->spec_.inputs[i];
    bool diode = port.kind == rs::InputKind::Comparator ||
                 port.kind == rs::InputKind::Repeater;
    return port.position + rs::offset(diode ? port.facing : rs::Dir::East);
  }

  // A cell carrying node n's signal, or none if wiring failed.
  std::optional<rs::Vec3> embed(int n) {
    const Expr e = this->expr_[n];
    if (e.op == 'i') {
      rs::Vec3 s = this->input_start(e.input);
      if (this->w_.at(s).type == rs::BlockType::Air)
        this->w_.set(s, rs::Block::dust());
      if (this->w_.at(s).type != rs::BlockType::Dust)
        return std::nullopt;
      return s;
    }
    std::optional<rs::Vec3> a = this->embed(e.a);
    if (!a)
      return std::nullopt;
    if (e.op == 'o') {
      // a or b: b's wire runs into a's cell.
      std::optional<rs::Vec3> b = this->embed(e.b);
      if (b && *b != *a)
        if (auto from = this->branch(*b))
          this->grow(*from, *a, false, true);
      return a;
    }
    // a minus b: a's wire into a subtract comparator, b's into its side.
    std::optional<rs::Vec3> from = this->branch(*a);
    if (!from)
      return std::nullopt;
    std::optional<rs::Vec3> gate = this->gate_wire(*from, this->random_cell());
    if (!gate)
      return std::nullopt;
    rs::Dir facing = this->w_.at(*gate).facing;
    rs::Vec3 out = *gate + rs::offset(facing);
    std::optional<rs::Vec3> b = this->embed(e.b);
    if (b) {
      // Full strength into the side: a repeater facing the comparator, fed
      // by b's wire, so b cancels a whatever their dust levels.
      std::array<rs::Dir, 2> sides{rs::rotate_clockwise(facing),
                                   rs::rotate_counter_clockwise(facing)};
      if (this->rng_() % 2)
        std::swap(sides[0], sides[1]);
      for (rs::Dir side_dir : sides) {
        rs::Vec3 side = *gate + rs::offset(side_dir);
        rs::Vec3 feed = side + rs::offset(side_dir);
        if (this->free(side) && this->free(feed) && this->prop_up(side)) {
          this->w_.set(side, rs::Block::repeater(rs::opposite(side_dir), 1));
          if (auto from_b = this->branch(*b))
            this->grow(*from_b, feed, true, true);
          break;
        }
      }
    }
    return out;
  }

  // A plain wire from `c` that ends in a subtract comparator (after one to
  // three steps) with dust on its output; the comparator's cell.
  std::optional<rs::Vec3> gate_wire(rs::Vec3 c, rs::Vec3 goal) {
    rs::Dir heading = this->heading_toward(c, goal, rs::Dir::East);
    int before = 1 + static_cast<int>(this->rng_() % 3);
    for (int step = 0; step < kMaxSteps; ++step) {
      if (!this->free(c) || !this->prop_up(c))
        return std::nullopt;
      rs::Dir next = this->heading_toward(c, goal, heading);
      rs::Vec3 ahead = c + rs::offset(heading);
      if (step >= before && next == heading && this->free(ahead) &&
          this->prop_up(ahead)) {
        this->w_.set(
            c, rs::Block::comparator(heading, rs::ComparatorMode::Subtract));
        this->w_.set(ahead, rs::Block::dust());
        this->comparators_.push_back(c);
        return c;
      }
      this->w_.set(c, rs::Block::dust());
      heading = next;
      c = c + rs::offset(heading);
    }
    return std::nullopt;
  }

  rs::Vec3 random_start() {
    const rs::InputPort &port =
        this->spec_.inputs[this->rng_() % this->spec_.inputs.size()];
    bool diode = port.kind == rs::InputKind::Comparator ||
                 port.kind == rs::InputKind::Repeater;
    return port.position + rs::offset(diode ? port.facing : rs::Dir::East);
  }

  rs::Dir heading_toward(rs::Vec3 from, rs::Vec3 goal, rs::Dir current) {
    std::vector<rs::Dir> closer;
    if (goal.x > from.x)
      closer.push_back(rs::Dir::East);
    if (goal.x < from.x)
      closer.push_back(rs::Dir::West);
    if (goal.z > from.z)
      closer.push_back(rs::Dir::South);
    if (goal.z < from.z)
      closer.push_back(rs::Dir::North);
    if (!closer.empty() && this->unit() < kTowardGoal)
      return closer[this->rng_() % closer.size()];
    std::vector<rs::Dir> any;
    for (rs::Dir d : rs::kHorizontalDirs)
      if (d != rs::opposite(current))
        any.push_back(d);
    return any[this->rng_() % any.size()];
  }

  // Lays a wire from `start`; it ends next to `goal` (or on it when
  // `onto`), when it runs into another block, or after kMaxSteps.
  void grow(rs::Vec3 c, rs::Vec3 goal, bool onto, bool plain = false) {
    rs::Dir heading = this->heading_toward(c, goal, rs::Dir::East);
    const rs::Vec3 up = rs::offset(rs::Dir::Up);
    for (int step = 0; step < kMaxSteps; ++step) {
      // A wire may start on an input's own cell; others never touch it.
      bool own_start = step == 0 && this->w_.in_bounds(c) &&
                       this->w_.at(c).type == rs::BlockType::Air &&
                       this->is_input_start(c);
      if (!this->free(c) && !own_start)
        return; // ran into the box edge or another wire: they join
      bool arrived =
          onto ? c == goal
               : std::abs(c.x - goal.x) + std::abs(c.z - goal.z) == 1 &&
                     c.y == goal.y;
      if (!this->prop_up(c))
        return;
      if (arrived) {
        this->w_.set(c, rs::Block::dust());
        return;
      }
      rs::Dir next = this->heading_toward(c, goal, heading);
      rs::Vec3 ahead = c + rs::offset(next);
      // Down when above the goal's level and close or by chance; up by
      // chance. Dust needs the cell above the lower end empty.
      int dy = 0;
      int distance = std::abs(c.x - goal.x) + std::abs(c.z - goal.z);
      if (c.y > goal.y &&
          (distance <= c.y - goal.y + 1 || this->unit() < kDescend)) {
        if (this->w_.in_bounds(ahead) &&
            this->w_.at(ahead).type == rs::BlockType::Air)
          dy = -1;
      } else if (c.y + 1 < this->w_.size().y && this->unit() < kClimb &&
                 this->free(c + up)) {
        dy = 1;
      }
      if (dy != 0) {
        this->w_.set(c, rs::Block::dust());
        heading = next;
        c = ahead + rs::Vec3{0, dy, 0};
        continue;
      }
      double r = plain ? 1.0 : this->unit();
      bool straight = next == heading;
      ahead = c + rs::offset(heading);
      if (straight && (r -= kRepeater) < 0) {
        this->w_.set(c, rs::Block::repeater(
                            heading, 1 + static_cast<int>(this->rng_() % 4)));
      } else if (straight && (r -= kComparator) < 0) {
        this->w_.set(c, rs::Block::comparator(
                            heading, this->rng_() % 2
                                         ? rs::ComparatorMode::Subtract
                                         : rs::ComparatorMode::Compare));
        this->comparators_.push_back(c);
      } else if (straight && (r -= kInverter) < 0 && this->free(ahead) &&
                 ahead != goal) {
        // A block powered by the wire, and a torch on its far side.
        this->w_.set(c, rs::Block::solid());
        this->w_.set(ahead, rs::Block::torch(rs::opposite(heading)));
        c = ahead + rs::offset(heading);
        continue;
      } else {
        this->w_.set(c, rs::Block::dust());
      }
      heading = next;
      c = c + rs::offset(heading);
    }
  }

  const rs::Spec &spec_;
  std::mt19937_64 &rng_;
  rs::World w_;
  std::vector<rs::Vec3> comparators_;
  std::vector<Expr> expr_;
};

} // namespace

World grown_layout(const Spec &spec, std::mt19937_64 &rng) {
  return Grower(spec, rng).build();
}

World motif_layout(const Spec &spec, std::mt19937_64 &rng) {
  return Grower(spec, rng).build_motifs();
}

World with_random_wire(const Spec &spec, std::mt19937_64 &rng, World w) {
  return Grower(spec, rng, std::move(w)).with_random_wire();
}

// Settled outputs for every vector, if the circuit always comes to rest and
// ends up the same whichever vector it came from.
std::optional<std::vector<std::uint8_t>>
truth_table(const rs::World &w, const std::vector<rs::InputVector> &vectors,
            int outputs) {
  std::optional<rs::Simulator> built;
  try {
    built.emplace(w);
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }
  if (!built->run_until_quiet(kSettleTicks))
    return std::nullopt;
  auto apply = [](rs::Simulator &sim, const rs::InputVector &v) {
    for (std::size_t i = 0; i < v.size(); ++i)
      sim.set_input(static_cast<int>(i), v[i]);
  };
  std::vector<rs::Simulator> settled;
  std::vector<std::uint8_t> table;
  for (const rs::InputVector &v : vectors) {
    rs::Simulator sim = *built;
    apply(sim, v);
    if (!sim.run_until_quiet(kSettleTicks))
      return std::nullopt;
    for (int o = 0; o < outputs; ++o)
      table.push_back(static_cast<std::uint8_t>(sim.output(o)));
    settled.push_back(std::move(sim));
  }
  // Coming from the next vector instead must give the same outputs.
  for (std::size_t u = 0; u < vectors.size(); ++u) {
    rs::Simulator sim = settled[(u + 1) % vectors.size()];
    apply(sim, vectors[u]);
    if (!sim.run_until_quiet(kSettleTicks))
      return std::nullopt;
    for (int o = 0; o < outputs; ++o)
      if (sim.output(o) != table[u * outputs + o])
        return std::nullopt;
  }
  return table;
}

// On/off key of a truth table: bit (vector * outputs + output).
std::uint64_t key_of(const std::vector<std::uint8_t> &table) {
  std::uint64_t key = 0;
  for (std::size_t i = 0; i < table.size(); ++i)
    if (table[i] > 0)
      key |= std::uint64_t{1} << i;
  return key;
}

int count_blocks(const rs::World &w) {
  int blocks = 0;
  for (int i = 0; i < w.volume(); ++i) {
    rs::Vec3 p = w.position_of(i);
    rs::BlockType t = w.at(p).type;
    if (p.y > 0 && t != rs::BlockType::Air && t != rs::BlockType::Input &&
        t != rs::BlockType::Output)
      ++blocks;
  }
  return blocks;
}

Target target_of(const rs::Spec &spec,
                 const std::vector<rs::InputVector> &vectors) {
  Target t;
  int outputs = static_cast<int>(spec.outputs.size());
  for (std::size_t v = 0; v < vectors.size(); ++v) {
    auto expected = spec.expected_outputs(vectors[v]);
    if (!expected)
      continue;
    for (int o = 0; o < outputs; ++o) {
      std::uint64_t bit = std::uint64_t{1} << (v * outputs + o);
      if ((*expected)[o].max == 0) {
        t.mask |= bit;
      } else if ((*expected)[o].min >= 1) {
        t.mask |= bit;
        t.bits |= bit;
      }
    }
  }
  return t;
}

// One random change: a cell gets a random block or is cleared, or a short
// wire is grown from a random input.
void mutate(rs::World &w, const rs::Spec &spec, std::mt19937_64 &rng) {
  rs::Vec3 size = w.size();
  int changes = 1 + static_cast<int>(rng() % 3);
  for (int k = 0; k < changes; ++k) {
    rs::Vec3 p{static_cast<int>(rng() % size.x),
               1 + static_cast<int>(rng() % (size.y - 1)),
               static_cast<int>(rng() % size.z)};
    rs::BlockType t = w.at(p).type;
    if (t == rs::BlockType::Input || t == rs::BlockType::Output)
      continue;
    switch (rng() % 6) {
    case 0:
      w.set(p, rs::Block::air());
      break;
    case 1:
    case 2:
      w.set(p, random_block(w, p, rng, Mix{0.2, 0.25, 0.25, 0.1, 0.1, 0.1}));
      break;
    case 3:
      w = Grower(spec, rng, std::move(w)).with_random_wire();
      break;
    default: {
      // Retune or turn a component, or put one on a wire.
      rs::Dir d = rs::kHorizontalDirs[rng() % 4];
      if (t == rs::BlockType::Comparator)
        w.set(p, rs::Block::comparator(d, w.at(p).comparator_mode() ==
                                                  rs::ComparatorMode::Subtract
                                              ? rs::ComparatorMode::Compare
                                              : rs::ComparatorMode::Subtract));
      else if (t == rs::BlockType::Repeater)
        w.set(p, rs::Block::repeater(d, 1 + static_cast<int>(rng() % 4)));
      else if (t == rs::BlockType::Dust)
        w.set(p, rng() % 2
                     ? rs::Block::repeater(d, 1)
                     : rs::Block::comparator(d, rs::ComparatorMode::Subtract));
      break;
    }
    }
  }
}

Score score_of(const std::vector<std::uint8_t> &table, const Target &target,
               int blocks) {
  Score s{0, 0, blocks};
  for (std::size_t i = 0; i < table.size(); ++i) {
    std::uint64_t bit = std::uint64_t{1} << i;
    if (!(target.mask & bit))
      continue;
    bool want = target.bits & bit;
    bool on = table[i] > 0;
    if (want == on)
      ++s.right;
    else
      s.wrong += want ? kOn : table[i];
  }
  return s;
}

} // namespace rs::gen

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

  double unit() { return std::uniform_real_distribution<double>(0, 1)(rng_); }

  bool free(rs::Vec3 p) const {
    return this->w_.in_bounds(p) && p.y == 1 &&
           this->w_.at(p).type == rs::BlockType::Air;
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
  void grow(rs::Vec3 c, rs::Vec3 goal, bool onto) {
    rs::Dir heading = this->heading_toward(c, goal, rs::Dir::East);
    for (int step = 0; step < kMaxSteps; ++step) {
      if (!this->free(c))
        return; // ran into the box edge or another wire: they join
      bool arrived =
          onto ? c == goal
               : std::abs(c.x - goal.x) + std::abs(c.z - goal.z) == 1 &&
                     c.y == goal.y;
      if (arrived) {
        this->w_.set(c, rs::Block::dust());
        return;
      }
      rs::Dir next = this->heading_toward(c, goal, heading);
      double r = this->unit();
      bool straight = next == heading;
      rs::Vec3 ahead = c + rs::offset(heading);
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
};

} // namespace

World grown_layout(const Spec &spec, std::mt19937_64 &rng) {
  return Grower(spec, rng).build();
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

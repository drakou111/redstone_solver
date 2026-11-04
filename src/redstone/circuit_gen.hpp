#pragma once

// Random circuits for training data and search: cell classes, generators,
// mutations and truth tables over on/off input vectors.

#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "redstone/spec.hpp"
#include "redstone/world.hpp"

namespace rs::gen {

constexpr int kSettleTicks = 400;
constexpr int kOn = 15;

// Odds of each block per free cell above the floor.
struct Mix {
  double air = 0.50;
  double solid = 0.16;
  double dust = 0.16;
  double torch = 0.06;
  double repeater = 0.06;
  double comparator = 0.06;
};
constexpr double kFloorSolid = 0.7;

// One class per cell content the model predicts. Ports are fixed by the
// spec and get their own class.
class CellClasses {
public:
  CellClasses() {
    this->add("air", rs::Block::air());
    this->add("solid", rs::Block::solid());
    this->add("dust", rs::Block::dust());
    for (rs::Dir d : {rs::Dir::Down, rs::Dir::North, rs::Dir::South,
                      rs::Dir::West, rs::Dir::East})
      this->add(std::format("torch {}", rs::to_string(d)), rs::Block::torch(d));
    for (rs::Dir d : rs::kHorizontalDirs)
      for (int delay = 1; delay <= 4; ++delay)
        this->add(std::format("repeater {} {}", rs::to_string(d), delay),
                  rs::Block::repeater(d, delay));
    for (rs::Dir d : rs::kHorizontalDirs) {
      this->add(std::format("comparator {} compare", rs::to_string(d)),
                rs::Block::comparator(d, rs::ComparatorMode::Compare));
      this->add(std::format("comparator {} subtract", rs::to_string(d)),
                rs::Block::comparator(d, rs::ComparatorMode::Subtract));
    }
    this->port_ = static_cast<int>(this->names_.size());
    this->names_.push_back("port");
  }

  int of(const rs::Block &b) const {
    if (b.type == rs::BlockType::Input || b.type == rs::BlockType::Output)
      return this->port_;
    for (std::size_t i = 0; i < this->blocks_.size(); ++i)
      if (this->blocks_[i] == b)
        return static_cast<int>(i);
    throw std::logic_error("block outside the class list: " + rs::to_string(b));
  }
  const std::vector<std::string> &names() const { return this->names_; }
  int port() const { return this->port_; }
  // The block of a class other than `port`.
  const rs::Block &block(int index) const { return this->blocks_.at(index); }

private:
  void add(std::string name, rs::Block b) {
    this->names_.push_back(std::move(name));
    this->blocks_.push_back(b);
  }
  std::vector<std::string> names_;
  std::vector<rs::Block> blocks_;
  int port_ = 0;
};

// The spec's own table as bits that must match (mask) and their values.
struct Target {
  std::uint64_t mask = 0;
  std::uint64_t bits = 0;
  bool matches(std::uint64_t key) const {
    return this->mask != 0 && (key & this->mask) == this->bits;
  }
};

// How close a truth table is to the spec's: rows right first, then how
// weakly the wrong outputs are on, then fewer blocks.
struct Score {
  int right = -1;
  int wrong = 0;
  int blocks = 0;
  auto operator<=>(const Score &other) const {
    if (this->right != other.right)
      return this->right <=> other.right;
    if (this->wrong != other.wrong)
      return other.wrong <=> this->wrong;
    return other.blocks <=> this->blocks;
  }
  bool operator==(const Score &) const = default;
};

// A random block for cell p that the cells below and around allow.
Block random_block(const World &w, Vec3 p, std::mt19937_64 &rng,
                   const Mix &mix);
// The spec's ports on a random floor, every other cell random.
World random_layout(const Spec &spec, std::mt19937_64 &rng);
// Wires grown from the inputs toward the outputs and into comparator sides.
World grown_layout(const Spec &spec, std::mt19937_64 &rng);
// The circuit with one more random wire.
World with_random_wire(const Spec &spec, std::mt19937_64 &rng, World w);
// Every combination of inputs off (0) and fully on.
std::vector<InputVector> on_off_vectors(int inputs);
// Settled outputs per vector ([vector][output]), if the circuit always comes
// to rest and ends up the same whichever vector it came from.
std::optional<std::vector<std::uint8_t>>
truth_table(const World &w, const std::vector<InputVector> &vectors,
            int outputs);
// On/off key of a truth table: bit (vector * outputs + output).
std::uint64_t key_of(const std::vector<std::uint8_t> &table);
// Placed blocks above the floor, ports excluded.
int count_blocks(const World &w);
Target target_of(const Spec &spec, const std::vector<InputVector> &vectors);
// One to three random changes.
void mutate(World &w, const Spec &spec, std::mt19937_64 &rng);
Score score_of(const std::vector<std::uint8_t> &table, const Target &target,
               int blocks);

} // namespace rs::gen

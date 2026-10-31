#pragma once

#include <optional>
#include <string>
#include <vector>

#include "redstone/block.hpp"
#include "redstone/geometry.hpp"

namespace rs {

// A fixed-size box of blocks. Everything outside the box is air.
class World {
public:
  explicit World(Vec3 size);

  Vec3 size() const { return this->size_; }
  int volume() const { return static_cast<int>(this->blocks_.size()); }
  bool in_bounds(Vec3 p) const;

  // Flat index of an in-bounds position.
  int index_of(Vec3 p) const {
    return (p.y * this->size_.z + p.z) * this->size_.x + p.x;
  }
  Vec3 position_of(int index) const;

  const Block &at(Vec3 p) const;
  void set(Vec3 p, Block block);

  std::optional<Vec3> find_input(int channel) const;
  std::optional<Vec3> find_output(int channel) const;

  // Placement rules the game enforces plus the restrictions this project adds
  // (no adjacent chests, no hopper item transfer). Empty means valid.
  std::vector<std::string> validate() const;

private:
  Vec3 size_;
  std::vector<Block> blocks_;
};

} // namespace rs

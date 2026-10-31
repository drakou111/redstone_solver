#pragma once

#include <array>

#include "redstone/geometry.hpp"

namespace rs {

// Where a build sits in a real Minecraft world. Update order depends on
// absolute position (through BlockPos hash codes) and on orientation, so the
// same build can behave differently at different placements. Checking several
// placements exposes such dependence.
struct Placement {
  Vec3 origin{};       // world position of the local origin
  Symmetry symmetry{}; // applied to local coordinates before adding the origin
};

// Java's fixed update orders, expressed in the build's local directions.
class UpdateOrder {
public:
  static constexpr int kDustUpdateSetSize = 7;

  explicit UpdateOrder(Placement placement = {});

  const Placement &placement() const { return this->placement_; }
  Vec3 to_world(Vec3 local) const {
    return this->placement_.symmetry.apply(local) + this->placement_.origin;
  }

  // NeighborUpdater.UPDATE_ORDER: west, east, down, up, north, south.
  const std::array<Dir, kDirCount> &neighbor_order() const {
    return this->neighbor_order_;
  }
  // Block.UPDATE_SHAPE_ORDER: west, east, north, south, down, up.
  const std::array<Dir, kDirCount> &shape_order() const {
    return this->shape_order_;
  }
  // Direction.values(): down, up, north, south, west, east.
  const std::array<Dir, kDirCount> &values_order() const {
    return this->values_order_;
  }
  // Direction.Plane.HORIZONTAL: north, east, south, west.
  const std::array<Dir, 4> &horizontal_order() const {
    return this->horizontal_order_;
  }

  // Iteration order of the HashSet<BlockPos> that classic dust fills with its
  // own position and its six neighbours before updating around each of them.
  std::array<Vec3, kDustUpdateSetSize> dust_update_positions(Vec3 local) const;

  // HashMap bucket of a world position in a default-capacity (16) table.
  static int java_bucket(Vec3 world);

private:
  Dir to_local(Dir world) const {
    return this->placement_.symmetry.apply_inverse(world);
  }

  Placement placement_;
  std::array<Dir, kDirCount> neighbor_order_{};
  std::array<Dir, kDirCount> shape_order_{};
  std::array<Dir, kDirCount> values_order_{};
  std::array<Dir, 4> horizontal_order_{};
};

} // namespace rs

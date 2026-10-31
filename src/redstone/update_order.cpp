#include "redstone/update_order.hpp"

#include <algorithm>
#include <cstdint>

namespace rs {

namespace {

constexpr std::array<Dir, kDirCount> kJavaNeighborOrder{
    Dir::West, Dir::East, Dir::Down, Dir::Up, Dir::North, Dir::South};
constexpr std::array<Dir, kDirCount> kJavaShapeOrder{
    Dir::West, Dir::East, Dir::North, Dir::South, Dir::Down, Dir::Up};
constexpr int kJavaHashMultiplier = 31;
constexpr std::uint32_t kDefaultHashTableMask = 15;
constexpr int kJavaHashSpreadShift = 16;

} // namespace

UpdateOrder::UpdateOrder(Placement placement) : placement_(placement) {
  for (int i = 0; i < kDirCount; ++i) {
    this->neighbor_order_[i] = this->to_local(kJavaNeighborOrder[i]);
    this->shape_order_[i] = this->to_local(kJavaShapeOrder[i]);
    this->values_order_[i] = this->to_local(kAllDirs[i]);
  }
  for (int i = 0; i < 4; ++i)
    this->horizontal_order_[i] = this->to_local(kHorizontalDirs[i]);
}

int UpdateOrder::java_bucket(Vec3 world) {
  // Vec3i.hashCode with Java's wrapping int arithmetic, then HashMap.hash
  // spreading.
  auto y = static_cast<std::uint32_t>(world.y);
  auto z = static_cast<std::uint32_t>(world.z);
  auto x = static_cast<std::uint32_t>(world.x);
  std::uint32_t h = (y + z * kJavaHashMultiplier) * kJavaHashMultiplier + x;
  h ^= h >> kJavaHashSpreadShift;
  return static_cast<int>(h & kDefaultHashTableMask);
}

std::array<Vec3, UpdateOrder::kDustUpdateSetSize>
UpdateOrder::dust_update_positions(Vec3 local) const {
  struct Entry {
    Vec3 local;
    int bucket;
  };
  std::array<Entry, kDustUpdateSetSize> entries{};
  entries[0] = {local, java_bucket(this->to_world(local))};
  for (int i = 0; i < kDirCount; ++i) {
    Vec3 neighbor = local + offset(this->values_order_[i]);
    entries[i + 1] = {neighbor, java_bucket(this->to_world(neighbor))};
  }

  // Buckets iterate in index order; colliding keys keep insertion order.
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const Entry &a, const Entry &b) { return a.bucket < b.bucket; });

  std::array<Vec3, kDustUpdateSetSize> result{};
  for (int i = 0; i < kDustUpdateSetSize; ++i)
    result[i] = entries[i].local;
  return result;
}

} // namespace rs

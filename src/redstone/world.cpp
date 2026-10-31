#include "redstone/world.hpp"

#include <format>
#include <stdexcept>

namespace rs {

namespace {

const Block kAir = Block::air();

std::string describe(Vec3 p) {
  return std::format("({}, {}, {})", p.x, p.y, p.z);
}

} // namespace

World::World(Vec3 size) : size_(size) {
  if (size.x <= 0 || size.y <= 0 || size.z <= 0)
    throw std::invalid_argument(
        std::format("world size {} must be positive", describe(size)));
  this->blocks_.resize(static_cast<std::size_t>(size.x) * size.y * size.z);
}

bool World::in_bounds(Vec3 p) const {
  return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < this->size_.x &&
         p.y < this->size_.y && p.z < this->size_.z;
}

Vec3 World::position_of(int index) const {
  int x = index % this->size_.x;
  int rest = index / this->size_.x;
  return {x, rest / this->size_.z, rest % this->size_.z};
}

const Block &World::at(Vec3 p) const {
  if (!this->in_bounds(p))
    return kAir;
  return this->blocks_[this->index_of(p)];
}

void World::set(Vec3 p, Block block) {
  if (!this->in_bounds(p))
    throw std::out_of_range("position " + describe(p) +
                            " is outside the world");
  this->blocks_[this->index_of(p)] = block;
}

std::optional<Vec3> World::find_input(int channel) const {
  for (int i = 0; i < this->volume(); ++i) {
    const Block &b = this->blocks_[i];
    if (b.type == BlockType::Input && b.channel == channel)
      return this->position_of(i);
  }
  return std::nullopt;
}

std::optional<Vec3> World::find_output(int channel) const {
  for (int i = 0; i < this->volume(); ++i) {
    const Block &b = this->blocks_[i];
    if (b.type == BlockType::Output && b.channel == channel)
      return this->position_of(i);
  }
  return std::nullopt;
}

std::vector<std::string> World::validate() const {
  std::vector<std::string> errors;
  std::vector<bool> input_seen(256, false);
  std::vector<bool> output_seen(256, false);

  for (int i = 0; i < this->volume(); ++i) {
    Vec3 p = this->position_of(i);
    const Block &b = this->blocks_[i];
    const Block &below = this->at(p + offset(Dir::Down));
    std::string where = to_string(b) + " at " + describe(p);

    if (is_dust(b) && !supports_on_top(below))
      errors.push_back(where + " has nothing to stand on");

    switch (b.type) {
    case BlockType::Repeater:
    case BlockType::Comparator:
      if (!is_horizontal(b.facing))
        errors.push_back(where + " must face horizontally");
      if (!supports_on_top(below))
        errors.push_back(where + " has nothing to stand on");
      break;
    case BlockType::Torch: {
      const Block &support = this->at(p + offset(b.facing));
      bool ok = b.facing == Dir::Down ? supports_on_top(support)
                                      : supports_on_side(support);
      if (b.facing == Dir::Up)
        ok = false;
      if (!ok)
        errors.push_back(where + " has no valid block to attach to");
      break;
    }
    case BlockType::Chest:
      for (Dir d : kHorizontalDirs)
        if (this->at(p + offset(d)).type == BlockType::Chest)
          errors.push_back(
              where +
              " touches another chest (would merge into a double chest)");
      break;
    case BlockType::Hopper:
      // Hoppers point down; a container above or below would move items and
      // change signals.
      if (is_container(this->at(p + offset(Dir::Up))) || is_container(below))
        errors.push_back(where + " has a container above or below it");
      break;
    case BlockType::Input:
      if (input_seen[b.channel])
        errors.push_back(where + " reuses an input channel");
      input_seen[b.channel] = true;
      break;
    case BlockType::Output:
      if (output_seen[b.channel])
        errors.push_back(where + " reuses an output channel");
      output_seen[b.channel] = true;
      break;
    default:
      break;
    }
  }
  return errors;
}

} // namespace rs

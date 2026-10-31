#pragma once

#include <string>

#include "redstone/world.hpp"

namespace rs {

// Top-down view of every layer (highest first) with short block codes,
// followed by a legend.
std::string render_layers(const World &world);

// One line per non-air block.
std::string list_blocks(const World &world);

} // namespace rs

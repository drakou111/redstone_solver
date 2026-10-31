#pragma once

#include <string>

#include "redstone/world.hpp"

namespace rs {

// Top-down view of every layer (highest first) with short block codes,
// followed by a legend.
std::string render_layers(const World &world);

// One line per non-air block.
std::string list_blocks(const World &world);

// Short code used in layer views, e.g. "#", "-", "re2" (repeater east, 2).
std::string block_code(const Block &block);

// Java /setblock commands relative to the executing position (~ ~ ~ is the
// layout's origin corner). Supports come first so nothing pops off. Ports
// and container contents are listed as comments.
std::string setblock_commands(const World &world);

} // namespace rs

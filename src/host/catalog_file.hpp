#pragma once

#include <filesystem>

#include "redstone/gate_library.hpp"

namespace rs::host {

// Reads a catalog written by save_catalog into `catalog`; a missing file
// leaves it as it is. Throws std::runtime_error for a damaged file.
void load_catalog(const std::filesystem::path &file, GateCatalog &catalog);

// Writes every module of `catalog` to `file` (through a temporary file, so a
// crash leaves the old one).
void save_catalog(const std::filesystem::path &file,
                  const GateCatalog &catalog);

} // namespace rs::host

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace rs::host {

// Newest client jar of a Prism or vanilla launcher install, if any.
std::optional<std::filesystem::path> find_minecraft_jar();

// Block textures from the user's own client jar as "data:image/png;base64,..."
// URLs keyed by texture name. Missing textures are simply absent; throws
// std::runtime_error if the jar cannot be read.
std::map<std::string, std::string>
load_block_textures(const std::filesystem::path &jar);

} // namespace rs::host

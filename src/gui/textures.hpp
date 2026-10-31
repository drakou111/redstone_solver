#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "imgui.h"

namespace rs::gui {

// Block textures read at runtime from the user's own Minecraft client jar.
// Nothing is bundled with this program.
class TextureLibrary {
public:
  TextureLibrary() = default;
  ~TextureLibrary();
  TextureLibrary(const TextureLibrary &) = delete;
  TextureLibrary &operator=(const TextureLibrary &) = delete;

  // Replaces the loaded textures; throws std::runtime_error on failure.
  void load_from_jar(const std::filesystem::path &jar);
  void clear();
  bool loaded() const { return !this->textures_.empty(); }
  const std::filesystem::path &source() const { return this->source_; }

  // Texture of assets/minecraft/textures/block/<name>.png, or 0 if missing.
  ImTextureID get(std::string_view name) const;

  // Newest client jar of a Prism or vanilla launcher install, if any.
  static std::optional<std::filesystem::path> find_default_jar();

private:
  std::unordered_map<std::string, unsigned int> textures_;
  std::filesystem::path source_;
};

} // namespace rs::gui

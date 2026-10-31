#include "gui/textures.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <GLFW/glfw3.h>

#include "miniz.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace rs::gui {

namespace {

constexpr const char *kBlockTextureDir = "assets/minecraft/textures/block/";
constexpr GLint kClampToEdge = 0x812F; // GL 1.2, missing from Windows' gl.h

constexpr std::array<const char *, 30> kTextureNames{
    "cyan_wool",           "cyan_stained_glass", "redstone_dust_dot",
    "redstone_dust_line0", "redstone_torch",     "redstone_torch_off",
    "redstone_block",      "repeater",           "repeater_on",
    "comparator",          "comparator_on",      "smooth_stone",
    "target_side",         "target_top",         "note_block",
    "hopper_outside",      "hopper_top",         "barrel_side",
    "barrel_top",          "barrel_bottom",      "oak_planks",
    "observer_front",      "observer_back",      "observer_back_on",
    "observer_side",       "observer_top",       "lime_concrete",
    "light_blue_concrete", "redstone_lamp",      "stone",
};

// Uploads RGBA pixels; tall animated strips keep only their first frame.
GLuint upload(const unsigned char *pixels, int width, int height) {
  int frame_height = std::min(width, height);
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, frame_height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  return id;
}

std::optional<std::filesystem::path>
newest(const std::vector<std::filesystem::path> &paths) {
  std::optional<std::filesystem::path> best;
  std::filesystem::file_time_type best_time{};
  for (const auto &path : paths) {
    std::error_code error;
    auto time = std::filesystem::last_write_time(path, error);
    if (!error && (!best || time > best_time)) {
      best = path;
      best_time = time;
    }
  }
  return best;
}

} // namespace

TextureLibrary::~TextureLibrary() { this->clear(); }

void TextureLibrary::clear() {
  for (auto &[name, id] : this->textures_)
    glDeleteTextures(1, &id);
  this->textures_.clear();
  this->source_.clear();
}

void TextureLibrary::load_from_jar(const std::filesystem::path &jar) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, jar.string().c_str(), 0))
    throw std::runtime_error("cannot open " + jar.string() + " as a jar");

  std::unordered_map<std::string, unsigned int> loaded;
  for (const char *name : kTextureNames) {
    std::string entry = std::string(kBlockTextureDir) + name + ".png";
    std::size_t size = 0;
    void *data =
        mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
    if (!data)
      continue; // older or newer versions may lack a texture; it falls back
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(
        static_cast<const stbi_uc *>(data), static_cast<int>(size), &width,
        &height, &channels, 4);
    mz_free(data);
    if (!pixels)
      continue;
    loaded[name] = upload(pixels, width, height);
    stbi_image_free(pixels);
  }
  mz_zip_reader_end(&zip);
  if (loaded.empty())
    throw std::runtime_error("no block textures found in " + jar.string());

  this->clear();
  this->textures_ = std::move(loaded);
  this->source_ = jar;
}

ImTextureID TextureLibrary::get(std::string_view name) const {
  auto it = this->textures_.find(std::string(name));
  return it == this->textures_.end() ? ImTextureID{}
                                     : static_cast<ImTextureID>(it->second);
}

std::optional<std::filesystem::path> TextureLibrary::find_default_jar() {
  namespace fs = std::filesystem;
  std::vector<fs::path> roots;
  if (const char *appdata = std::getenv("APPDATA")) {
    roots.emplace_back(fs::path(appdata) / "PrismLauncher");
    roots.emplace_back(fs::path(appdata) / ".minecraft");
  }
  if (const char *home = std::getenv("HOME")) {
    roots.emplace_back(fs::path(home) / ".local/share/PrismLauncher");
    roots.emplace_back(fs::path(home) / ".minecraft");
    roots.emplace_back(fs::path(home) /
                       "Library/Application Support/minecraft");
  }

  std::vector<fs::path> jars;
  std::error_code error;
  for (const fs::path &root : roots) {
    fs::path prism = root / "libraries/com/mojang/minecraft";
    for (const auto &version : fs::directory_iterator(prism, error))
      for (const auto &file : fs::directory_iterator(version.path(), error))
        if (file.path().filename().string().ends_with("-client.jar"))
          jars.push_back(file.path());
    for (const auto &version :
         fs::directory_iterator(root / "versions", error)) {
      fs::path jar =
          version.path() / (version.path().filename().string() + ".jar");
      if (fs::exists(jar, error))
        jars.push_back(jar);
    }
  }
  return newest(jars);
}

} // namespace rs::gui

#include "host/minecraft_assets.hpp"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "miniz.h"

namespace rs::host {

namespace {

constexpr const char *kBlockTextureDir = "assets/minecraft/textures/block/";

constexpr std::array<const char *, 32> kTextureNames{"cyan_wool",
                                                     "cyan_stained_glass",
                                                     "redstone_dust_dot",
                                                     "redstone_dust_line0",
                                                     "redstone_torch",
                                                     "redstone_torch_off",
                                                     "redstone_block",
                                                     "repeater",
                                                     "repeater_on",
                                                     "comparator",
                                                     "comparator_on",
                                                     "smooth_stone",
                                                     "target_side",
                                                     "target_top",
                                                     "note_block",
                                                     "hopper_outside",
                                                     "hopper_top",
                                                     "hopper_inside",
                                                     "barrel_side",
                                                     "barrel_top",
                                                     "barrel_bottom",
                                                     "oak_planks",
                                                     "observer_front",
                                                     "observer_back",
                                                     "observer_back_on",
                                                     "observer_side",
                                                     "observer_top",
                                                     "lime_concrete",
                                                     "light_blue_concrete",
                                                     "red_concrete",
                                                     "stone",
                                                     "barrier"};

std::string base64(const unsigned char *data, std::size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    std::uint32_t chunk = data[i] << 16;
    if (i + 1 < size)
      chunk |= data[i + 1] << 8;
    if (i + 2 < size)
      chunk |= data[i + 2];
    out += kAlphabet[(chunk >> 18) & 63];
    out += kAlphabet[(chunk >> 12) & 63];
    out += i + 1 < size ? kAlphabet[(chunk >> 6) & 63] : '=';
    out += i + 2 < size ? kAlphabet[chunk & 63] : '=';
  }
  return out;
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

std::optional<std::filesystem::path> find_minecraft_jar() {
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
    for (const auto &version :
         fs::directory_iterator(root / "libraries/com/mojang/minecraft", error))
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

std::map<std::string, std::string>
load_block_textures(const std::filesystem::path &jar) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, jar.string().c_str(), 0))
    throw std::runtime_error("cannot open " + jar.string() + " as a jar");

  std::map<std::string, std::string> textures;
  for (const char *name : kTextureNames) {
    std::string entry = std::string(kBlockTextureDir) + name + ".png";
    std::size_t size = 0;
    void *data =
        mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
    if (!data)
      continue; // other game versions may lack a texture; the UI falls back
    textures[name] = "data:image/png;base64," +
                     base64(static_cast<const unsigned char *>(data), size);
    mz_free(data);
  }
  mz_zip_reader_end(&zip);
  if (textures.empty())
    throw std::runtime_error("no block textures found in " + jar.string());
  return textures;
}

} // namespace rs::host

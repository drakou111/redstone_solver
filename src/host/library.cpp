#include "host/library.hpp"

#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rs::host {

namespace {

// FNV-1a: a stable file name for a key (std::hash may differ between runs).
std::uint64_t stable_hash(const std::string &text) {
  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= kPrime;
  }
  return hash;
}

} // namespace

CircuitLibrary::CircuitLibrary(std::filesystem::path directory)
    : directory_(std::move(directory)) {
  std::filesystem::create_directories(this->directory_);
}

std::filesystem::path CircuitLibrary::file_for(const std::string &key) const {
  return this->directory_ / std::format("{:016x}.json", stable_hash(key));
}

std::optional<CircuitLibrary::Entry>
CircuitLibrary::read(const std::filesystem::path &file,
                     const std::string *key) {
  std::ifstream in(file);
  if (!in)
    return std::nullopt;
  std::stringstream text;
  text << in.rdbuf();
  Json json = Json::parse(text.str(), nullptr, false);
  // A damaged file, or a different key with the same hash: not this entry.
  if (json.is_discarded() || !json.is_object() || !json.contains("spec") ||
      !json.contains("layout"))
    return std::nullopt;
  if (key && json.value("key", "") != *key)
    return std::nullopt;
  return Entry{json.at("spec"), json.at("layout"), json.value("blocks", 0),
               json.value("volume", 0), json.value("shape", "")};
}

std::optional<CircuitLibrary::Entry>
CircuitLibrary::find(const std::string &key) const {
  std::lock_guard lock(this->mutex_);
  return read(this->file_for(key), &key);
}

std::optional<CircuitLibrary::Entry>
CircuitLibrary::smallest_of_shape(const std::string &shape) const {
  std::lock_guard lock(this->mutex_);
  std::optional<Entry> best;
  for (const auto &file :
       std::filesystem::directory_iterator(this->directory_)) {
    if (file.path().extension() != ".json")
      continue;
    std::optional<Entry> entry = read(file.path(), nullptr);
    if (!entry || entry->shape.empty() || entry->shape != shape)
      continue;
    if (!best || entry->volume < best->volume ||
        (entry->volume == best->volume && entry->blocks < best->blocks))
      best = std::move(entry);
  }
  return best;
}

bool CircuitLibrary::offer(const std::string &key, const Entry &entry) {
  if (std::optional<Entry> old = this->find(key))
    if (old->volume < entry.volume ||
        (old->volume == entry.volume && old->blocks <= entry.blocks))
      return false;
  std::lock_guard lock(this->mutex_);
  const std::filesystem::path file = this->file_for(key);
  const std::filesystem::path temporary = file.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot write " + temporary.string());
    out << Json{{"key", key},
                {"spec", entry.spec},
                {"layout", entry.layout},
                {"blocks", entry.blocks},
                {"volume", entry.volume},
                {"shape", entry.shape}}
               .dump();
  }
  std::filesystem::rename(temporary, file);
  return true;
}

} // namespace rs::host

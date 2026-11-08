#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "host/json_io.hpp"

namespace rs::host {

// Verified circuits kept on disk between sessions, one per request: asking
// for the same thing again (the same spec, or the same on/off table for
// Compose) returns the smallest circuit found so far at once.
class CircuitLibrary {
public:
  struct Entry {
    Json spec;   // what the circuit solves (Compose: its own box and ports)
    Json layout; // the verified circuit
    int blocks = 0;
    int volume = 0;
    // Requests of the same shape (box, ports, blocks; any truth table) can
    // start from this circuit as a sketch.
    std::string shape;
  };

  explicit CircuitLibrary(std::filesystem::path directory);

  std::optional<Entry> find(const std::string &key) const;

  // Keeps the circuit unless the library already holds one at least as
  // small (fewer cells, then fewer blocks). Returns whether it was kept.
  bool offer(const std::string &key, const Entry &entry);

  // The smallest circuit kept for any request of this shape.
  std::optional<Entry> smallest_of_shape(const std::string &shape) const;

private:
  std::filesystem::path file_for(const std::string &key) const;
  static std::optional<Entry> read(const std::filesystem::path &file,
                                   const std::string *key);

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
};

} // namespace rs::host

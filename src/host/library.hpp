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
  };

  explicit CircuitLibrary(std::filesystem::path directory);

  std::optional<Entry> find(const std::string &key) const;

  // Keeps the circuit unless the library already holds one at least as
  // small (fewer cells, then fewer blocks). Returns whether it was kept.
  bool offer(const std::string &key, const Entry &entry);

private:
  std::filesystem::path file_for(const std::string &key) const;

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
};

} // namespace rs::host

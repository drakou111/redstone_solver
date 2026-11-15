#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "host/json_io.hpp"
#include "host/library.hpp"
#include "redstone/gate_library.hpp"
#include "redstone/solve_job.hpp"

namespace rs::host {

// The mod's backend: every request is a route name plus a JSON body and gets
// a JSON reply. Throws std::invalid_argument for bad requests. Thread safe.
class Api {
public:
  // With `library_dir`, verified circuits are kept there and requests seen
  // before are answered from it at once.
  explicit Api(std::optional<std::filesystem::path> library_dir = {});

  Json handle(std::string_view route, const Json &body);

private:
  GateCache gate_cache_; // gate modules shared by every compose job
  // Modules built ahead in many arrangements, kept next to the library.
  GateCatalog catalog_;
  std::optional<std::filesystem::path> catalog_file_;
  Json start_solve(const Json &body);
  Json start_shrink(const Json &body);
  Json start_evolve(const Json &body);
  Json start_compose(const Json &body);
  Json start_catalog(const Json &body);
  Json solve_status(const Json &body) const;
  Json analyze_result(const Json &body) const;
  Json verify(const Json &body) const;
  Json export_commands(const Json &body) const;

  std::unique_ptr<CircuitLibrary> library_;
  mutable std::mutex mutex_;
  std::unique_ptr<SolveJob> job_;
};

} // namespace rs::host

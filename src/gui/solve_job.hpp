#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "redstone/solver.hpp"
#include "redstone/spec.hpp"

namespace rs::gui {

// Runs the solver on a background thread and collects its progress log.
class SolveJob {
public:
  SolveJob(Spec spec, SolverOptions options);
  ~SolveJob();
  SolveJob(const SolveJob &) = delete;
  SolveJob &operator=(const SolveJob &) = delete;

  void cancel() { this->cancel_ = true; }
  bool is_done() const { return this->done_; }
  double seconds() const;
  std::vector<std::string> log() const;
  const Spec &spec() const { return this->spec_; }

  // Valid once is_done(); exactly one of them is set.
  std::optional<SolveReport> report() const;
  std::optional<std::string> error() const;

private:
  void run();

  Spec spec_;
  SolverOptions options_;
  std::atomic<bool> cancel_ = false;
  std::atomic<bool> done_ = false;
  std::chrono::steady_clock::time_point start_;
  mutable std::mutex mutex_;
  std::vector<std::string> log_;
  std::optional<SolveReport> report_;
  std::optional<std::string> error_;
  std::thread thread_; // last, so everything above exists when it starts
};

} // namespace rs::gui

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace rs {

// Several CaDiCaL instances with different seeds race on the same clauses;
// the first to answer wins and stops the others. Clauses are added
// incrementally, so each solver keeps what it learned between calls.
class SatPortfolio {
public:
  enum class Outcome { Satisfiable, Unsatisfiable, Stopped };

  explicit SatPortfolio(int workers);
  ~SatPortfolio();
  SatPortfolio(const SatPortfolio &) = delete;
  SatPortfolio &operator=(const SatPortfolio &) = delete;

  // Adds clauses[from..] (DIMACS literals) to every solver.
  void add(const std::vector<std::vector<int>> &clauses, std::size_t from);
  Outcome solve(std::chrono::steady_clock::time_point deadline,
                const std::atomic<bool> *cancel);
  // values[i] of variable i + 1, from the solver that answered.
  std::vector<bool> model(int variables) const;

private:
  struct Worker;
  std::vector<std::unique_ptr<Worker>> workers_;
  int winner_ = -1;
};

} // namespace rs

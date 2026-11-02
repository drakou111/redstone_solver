#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rs {

// Several CaDiCaL instances with different seeds race on the same clauses;
// the first to answer wins and stops the others. Clauses are added
// incrementally, so each solver keeps what it learned between calls.
class SatPortfolio {
public:
  enum class Outcome { Satisfiable, Unsatisfiable, Stopped };

  explicit SatPortfolio(int workers, int seed = 0);
  ~SatPortfolio();
  SatPortfolio(const SatPortfolio &) = delete;
  SatPortfolio &operator=(const SatPortfolio &) = delete;

  // Makes every solver try these literals true first.
  void prefer(const std::vector<int> &lits);
  // Adds clauses[from..] (DIMACS literals) to every solver.
  void add(const std::vector<std::vector<int>> &clauses, std::size_t from);
  // Stopped when the deadline passes or `cancel` or `interrupt` is set.
  Outcome solve(std::chrono::steady_clock::time_point deadline,
                const std::atomic<bool> *cancel,
                const std::atomic<bool> *interrupt = nullptr);
  // values[i] of variable i + 1, from the solver that answered.
  std::vector<bool> model(int variables) const;
  // Conflicts so far, summed over workers: a measure of search effort that,
  // unlike time, does not depend on machine load.
  std::int64_t conflicts() const;

private:
  struct Worker;
  std::vector<std::unique_ptr<Worker>> workers_;
  int winner_ = -1;
};

} // namespace rs

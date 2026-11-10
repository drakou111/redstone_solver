#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rs {

// A directed edge of a graph over SAT literals: active while every literal
// holds (DIMACS literals).
struct GraphEdge {
  int from = 0;
  int to = 0;
  int gap = 0; // 1: a loop through this edge is forbidden
  std::vector<int> lits;
};

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
  // Every solver rejects, as soon as its partial assignment has one, a loop
  // of active edges through an edge with a gap of 1 (with a clause made of
  // the loop's literals). Nodes are 0..nodes-1.
  void forbid_loops(int nodes, const std::vector<GraphEdge> &edges);
  // Adds clauses[from..] (DIMACS literals) to every solver. Stops between
  // clauses once `stop` says so (loading millions of clauses took up to
  // half a minute); false then, and the portfolio is incomplete: solving it
  // again is wrong.
  bool add(const std::vector<std::vector<int>> &clauses, std::size_t from,
           const std::function<bool()> &stop = {});
  // Stopped when the deadline passes or `cancel` or `interrupt` is set.
  // `assumptions` hold for this call only.
  Outcome solve(std::chrono::steady_clock::time_point deadline,
                const std::atomic<bool> *cancel,
                const std::atomic<bool> *interrupt = nullptr,
                const std::vector<int> &assumptions = {});
  // After Unsatisfiable: the assumptions the answer depends on (empty when
  // the clauses alone are unsatisfiable).
  std::vector<int> failed(const std::vector<int> &assumptions) const;
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

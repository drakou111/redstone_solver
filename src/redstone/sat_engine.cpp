#include "redstone/sat_engine.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <thread>

#ifdef _MSC_VER
#include "msvc_compat.h"
#endif
#include "cadical.hpp"

namespace rs {

namespace {

constexpr int kSatisfiable = 10;
constexpr int kUnsatisfiable = 20;
constexpr std::array<const char *, 2> kConfigurations{"sat", "default"};

class Stopper : public CaDiCaL::Terminator {
public:
  bool terminate() override {
    return this->done->load(std::memory_order_relaxed) ||
           (this->cancel && this->cancel->load(std::memory_order_relaxed)) ||
           (this->interrupt &&
            this->interrupt->load(std::memory_order_relaxed)) ||
           std::chrono::steady_clock::now() >= this->deadline;
  }

  const std::atomic<bool> *done = nullptr;
  const std::atomic<bool> *cancel = nullptr;
  const std::atomic<bool> *interrupt = nullptr;
  std::chrono::steady_clock::time_point deadline;
};

// Watches a graph whose edges switch on with literals and rejects every
// loop through a gap-1 edge the moment its edges are all on: the clause
// "not all of these literals" is falsified, so CaDiCaL learns from it at
// once. Edges switch on and off in trail order, so the active edges of a
// node form a stack.
class LoopGuard : public CaDiCaL::ExternalPropagator {
public:
  LoopGuard(int nodes, std::vector<GraphEdge> edges)
      : edges_(std::move(edges)), on_count_(this->edges_.size(), 0),
        out_(nodes), in_count_(nodes, 0) {
    int top = 0;
    for (const GraphEdge &e : this->edges_)
      for (int lit : e.lits)
        top = std::max(top, std::abs(lit));
    this->by_lit_.resize(2 * (top + 1));
    for (std::size_t e = 0; e < this->edges_.size(); ++e)
      for (int lit : this->edges_[e].lits)
        this->by_lit_[slot(lit)].push_back(static_cast<int>(e));
    this->mark_.assign(2 * nodes, 0);
    this->via_.assign(2 * nodes, -1);
    this->parent_.assign(2 * nodes, -1);
  }

  std::vector<int> watched_vars() const {
    std::vector<int> vars;
    for (std::size_t s = 0; s < this->by_lit_.size(); ++s)
      if (!this->by_lit_[s].empty())
        vars.push_back(static_cast<int>(s / 2));
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
  }

  void notify_assignment(const std::vector<int> &lits) override {
    for (int lit : lits) {
      this->trail_.push_back(lit);
      if (slot(lit) >= this->by_lit_.size())
        continue;
      for (int e : this->by_lit_[slot(lit)])
        if (++this->on_count_[e] ==
            static_cast<int>(this->edges_[e].lits.size()))
          this->switch_on(e);
    }
  }
  void notify_new_decision_level() override {
    this->levels_.push_back(this->trail_.size());
  }
  void notify_backtrack(std::size_t new_level) override {
    while (this->levels_.size() > new_level) {
      const std::size_t start = this->levels_.back();
      this->levels_.pop_back();
      while (this->trail_.size() > start) {
        const int lit = this->trail_.back();
        this->trail_.pop_back();
        if (slot(lit) >= this->by_lit_.size())
          continue;
        const std::vector<int> &list = this->by_lit_[slot(lit)];
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
          const int e = *it;
          if (this->on_count_[e] ==
              static_cast<int>(this->edges_[e].lits.size())) {
            std::vector<int> &stack = this->out_[this->edges_[e].from];
            if (!stack.empty() && stack.back() == e)
              stack.pop_back();
            else
              std::erase(stack, e);
            --this->in_count_[this->edges_[e].to];
          }
          --this->on_count_[e];
        }
      }
    }
  }
  bool cb_check_found_model(const std::vector<int> &) override {
    return this->clauses_.empty();
  }
  bool cb_has_external_clause(bool &forgettable) override {
    forgettable = false;
    return !this->clauses_.empty();
  }
  int cb_add_external_clause_lit() override {
    std::vector<int> &clause = this->clauses_.front();
    if (this->next_lit_ < clause.size())
      return clause[this->next_lit_++];
    this->clauses_.erase(this->clauses_.begin());
    this->next_lit_ = 0;
    return 0;
  }

private:
  static std::size_t slot(int lit) {
    return 2 * static_cast<std::size_t>(std::abs(lit)) + (lit < 0 ? 1 : 0);
  }

  // Edge e just switched on: is there a way back from its end to its start
  // (through a gap-1 edge if e has none)? Search over (node, gap-1 seen).
  void switch_on(int e) {
    const GraphEdge &edge = this->edges_[e];
    this->out_[edge.from].push_back(e);
    ++this->in_count_[edge.to];
    // A loop through e needs a way into its start and out of its end.
    if (this->clauses_.size() >= kMaxQueued ||
        this->in_count_[edge.from] == 0 || this->out_[edge.to].empty())
      return;
    ++this->stamp_;
    const int start = 2 * edge.to + (edge.gap > 0 ? 1 : 0);
    const int goal = 2 * edge.from + 1;
    std::vector<int> &queue = this->queue_;
    queue.assign(1, start);
    this->mark_[start] = this->stamp_;
    this->via_[start] = -1;
    bool found = start == goal;
    for (std::size_t i = 0; i < queue.size() && !found; ++i) {
      const int state = queue[i];
      for (int f : this->out_[state / 2]) {
        const GraphEdge &next = this->edges_[f];
        const int to = 2 * next.to + ((state & 1) | (next.gap > 0 ? 1 : 0));
        if (this->mark_[to] == this->stamp_)
          continue;
        this->mark_[to] = this->stamp_;
        this->via_[to] = f;
        this->parent_[to] = state;
        queue.push_back(to);
        if (to == goal) {
          found = true;
          break;
        }
      }
    }
    if (!found)
      return;
    std::vector<int> clause;
    auto add_edge = [&](int f) {
      for (int lit : this->edges_[f].lits)
        clause.push_back(-lit);
    };
    add_edge(e);
    for (int state = goal; state != start;) {
      const int f = this->via_[state];
      add_edge(f);
      state = this->parent_[state];
    }
    std::sort(clause.begin(), clause.end());
    clause.erase(std::unique(clause.begin(), clause.end()), clause.end());
    this->clauses_.push_back(std::move(clause));
  }

  static constexpr std::size_t kMaxQueued = 16;

  std::vector<GraphEdge> edges_;
  std::vector<int> on_count_;
  std::vector<std::vector<int>> out_; // switched-on edges by start node
  std::vector<int> in_count_;         // switched-on edges into each node
  std::vector<std::vector<int>> by_lit_;
  std::vector<int> trail_;
  std::vector<std::size_t> levels_;
  std::vector<std::vector<int>> clauses_;
  std::size_t next_lit_ = 0;
  // Search scratch, by state 2 * node + (gap-1 edge seen).
  std::vector<int> mark_;
  std::vector<int> via_;
  std::vector<int> parent_; // state each state was reached from
  std::vector<int> queue_;
  int stamp_ = 0;
};

} // namespace

struct SatPortfolio::Worker {
  CaDiCaL::Solver solver;
  Stopper stopper;
  std::unique_ptr<LoopGuard> guard;
};

SatPortfolio::SatPortfolio(int workers, int seed) {
  if (workers < 1)
    throw std::invalid_argument("a SAT portfolio needs at least one worker");
  for (int i = 0; i < workers; ++i) {
    auto worker = std::make_unique<Worker>();
    // The "sat" configuration (tuned for satisfiable formulas) was up to 6x
    // faster on room-sized specs; a second worker runs the default one,
    // which is better at proving that no smaller circuit exists.
    worker->solver.configure(kConfigurations[i % kConfigurations.size()]);
    // Variables are numbered by the encoder, so the solver must not add its
    // own (bounded variable addition).
    worker->solver.set("factor", 0);
#ifdef _MSC_VER
    // Congruence closure changes gates while they sit in a hash table and
    // crashes with MSVC's standard library (fine with libstdc++).
    worker->solver.set("congruence", 0);
#endif
    worker->solver.set("seed", seed + i);
    // The seed alone hardly changes CaDiCaL's search; shuffling the variable
    // order makes other seeds real alternatives (seed 0 keeps the default).
    if (seed + i > 0) {
      worker->solver.set("shuffle", 1);
      worker->solver.set("shufflerandom", 1);
    }
    worker->solver.connect_terminator(&worker->stopper);
    this->workers_.push_back(std::move(worker));
  }
}

SatPortfolio::~SatPortfolio() {
  for (auto &worker : this->workers_) {
    worker->solver.disconnect_terminator();
    if (worker->guard)
      worker->solver.disconnect_external_propagator();
  }
}

void SatPortfolio::forbid_loops(int nodes,
                                const std::vector<GraphEdge> &edges) {
  for (auto &worker : this->workers_) {
    if (worker->guard)
      worker->solver.disconnect_external_propagator();
    worker->guard = std::make_unique<LoopGuard>(nodes, edges);
    worker->solver.connect_external_propagator(worker->guard.get());
    for (int var : worker->guard->watched_vars())
      worker->solver.add_observed_var(var);
  }
}

void SatPortfolio::prefer(const std::vector<int> &lits) {
  for (auto &worker : this->workers_)
    for (int lit : lits)
      worker->solver.phase(lit);
}

void SatPortfolio::add(const std::vector<std::vector<int>> &clauses,
                       std::size_t from) {
  auto add_to = [&](Worker &worker) {
    for (std::size_t i = from; i < clauses.size(); ++i) {
      for (int lit : clauses[i])
        worker.solver.add(lit);
      worker.solver.add(0);
    }
  };
  std::vector<std::jthread> threads;
  for (std::size_t w = 1; w < this->workers_.size(); ++w)
    threads.emplace_back([&, w] { add_to(*this->workers_[w]); });
  add_to(*this->workers_[0]);
}

SatPortfolio::Outcome
SatPortfolio::solve(std::chrono::steady_clock::time_point deadline,
                    const std::atomic<bool> *cancel,
                    const std::atomic<bool> *interrupt,
                    const std::vector<int> &assumptions) {
  std::atomic<bool> done = false;
  std::atomic<int> winner = -1;
  std::atomic<int> result = 0;
  {
    std::vector<std::jthread> threads;
    for (std::size_t w = 0; w < this->workers_.size(); ++w) {
      Worker &worker = *this->workers_[w];
      worker.stopper.done = &done;
      worker.stopper.cancel = cancel;
      worker.stopper.interrupt = interrupt;
      worker.stopper.deadline = deadline;
      threads.emplace_back([&, w] {
        for (int lit : assumptions)
          worker.solver.assume(lit);
        int answer = worker.solver.solve();
        if (answer != kSatisfiable && answer != kUnsatisfiable)
          return;
        int none = -1;
        if (winner.compare_exchange_strong(none, static_cast<int>(w))) {
          result = answer;
          done = true;
        }
      });
    }
  }
  this->winner_ = winner;
  if (this->winner_ < 0)
    return Outcome::Stopped;
  return result == kSatisfiable ? Outcome::Satisfiable : Outcome::Unsatisfiable;
}

std::vector<int>
SatPortfolio::failed(const std::vector<int> &assumptions) const {
  if (this->winner_ < 0)
    throw std::logic_error("no SAT solver has answered");
  CaDiCaL::Solver &solver = this->workers_[this->winner_]->solver;
  std::vector<int> lits;
  for (int lit : assumptions)
    if (solver.failed(lit))
      lits.push_back(lit);
  return lits;
}

std::int64_t SatPortfolio::conflicts() const {
  std::int64_t total = 0;
  for (const auto &worker : this->workers_)
    total += std::max<std::int64_t>(
        0, worker->solver.get_statistic_value("conflicts"));
  return total;
}

std::vector<bool> SatPortfolio::model(int variables) const {
  if (this->winner_ < 0)
    throw std::logic_error("no SAT solver has a model");
  CaDiCaL::Solver &solver = this->workers_[this->winner_]->solver;
  int known = solver.vars();
  std::vector<bool> values(variables, false);
  for (int v = 1; v <= std::min(variables, known); ++v)
    values[v - 1] = solver.val(v) > 0;
  return values;
}

} // namespace rs

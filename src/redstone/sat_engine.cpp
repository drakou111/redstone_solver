#include "redstone/sat_engine.hpp"

#include <algorithm>
#include <array>
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

} // namespace

struct SatPortfolio::Worker {
  CaDiCaL::Solver solver;
  Stopper stopper;
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
  for (auto &worker : this->workers_)
    worker->solver.disconnect_terminator();
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
                    const std::atomic<bool> *interrupt) {
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

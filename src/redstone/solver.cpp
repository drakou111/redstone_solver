#include "redstone/solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "redstone/evolve.hpp"
#include "redstone/report.hpp"
#include "redstone/sat_engine.hpp"

namespace rs {

namespace sat = operations_research::sat;

namespace {

using Clock = std::chrono::steady_clock;
// A solver round never gets less than this unless it is the last one.
constexpr double kMinRoundSeconds = 5.0;
constexpr std::chrono::milliseconds kCancelPollInterval{50};
// Transitions make the model several times larger, so a misbehaving input
// change is first answered by banning layouts; only once it has caused this
// many rejections is it added to the model.
constexpr int kRejectionsBeforeTransition = 3;
constexpr int kFirstRejectionsBeforeTransition = 10;

// Keep first-solution callers (notably Compose's gate and wiring stages)
// responsive; longer searches can spend more time polishing their best.
constexpr std::chrono::seconds kFinalRefinementTime{10};
constexpr std::chrono::seconds kFirstSolutionRefinementTime{1};

// Blocks a plain solid block may stand in for when the circuit does not use
// what makes them special (a target's pull on dust, glass not conducting, a
// container's signal, a note block, a redstone block's power).
bool simplifies_to_solid(BlockType type) {
  switch (type) {
  case BlockType::Target:
  case BlockType::Glass:
  case BlockType::Barrel:
  case BlockType::Hopper:
  case BlockType::Chest:
  case BlockType::NoteBlock:
  case BlockType::RedstoneBlock:
    return true;
  default:
    return false;
  }
}

} // namespace

// Repeatedly lower repeater delays and remove blocks while the layout still
// passes verification. Every accepted edit is checked against the full set of
// verification vectors and placements.
LayoutRefinement refine_layout(World &layout, const Spec &spec,
                               const std::vector<InputVector> &vectors,
                               const VerificationOptions &options,
                               const std::atomic<bool> *cancel,
                               std::chrono::steady_clock::time_point deadline) {
  LayoutRefinement result;
  std::vector<bool> kept(layout.volume(), false);
  for (const auto &[position, block] : spec.fixed)
    kept[layout.index_of(position)] = true;
  bool changed = true;
  while (changed && !(cancel && cancel->load()) &&
         std::chrono::steady_clock::now() < deadline) {
    changed = false;

    // Try each shorter delay, from shortest upward. The first passing value
    // is the smallest delay that works with the current layout.
    for (int i = 0; i < layout.volume(); ++i) {
      if ((cancel && cancel->load()) ||
          std::chrono::steady_clock::now() >= deadline)
        return result;
      Vec3 p = layout.position_of(i);
      Block block = layout.at(p);
      if (kept[i] || block.type != BlockType::Repeater ||
          block.repeater_delay() <= kMinRepeaterDelay)
        continue;
      for (int delay = kMinRepeaterDelay; delay < block.repeater_delay();
           ++delay) {
        layout.set(p, Block::repeater(block.facing, delay));
        if (!verify_layout(layout, spec, vectors, options)) {
          ++result.reduced_repeaters;
          changed = true;
          break;
        }
        layout.set(p, block);
      }
    }

    // Remove blocks one at a time; repeat after delay changes/removals since
    // either kind of simplification can make another edit possible.
    for (int i = 0; i < layout.volume(); ++i) {
      if ((cancel && cancel->load()) ||
          std::chrono::steady_clock::now() >= deadline)
        return result;
      Vec3 p = layout.position_of(i);
      Block block = layout.at(p);
      if (kept[i] || block.type == BlockType::Air ||
          block.type == BlockType::Input || block.type == BlockType::Output)
        continue;
      layout.set(p, Block::air());
      if (verify_layout(layout, spec, vectors, options)) {
        layout.set(p, block);
        continue;
      }
      ++result.removed_blocks;
      changed = true;
    }

    // Replace special blocks by simpler ones where the circuit still works:
    // a plain solid block first, and a chest that must stay a container by a
    // barrel holding the same signal (one block, never joins a neighbour).
    const bool solid_allowed = spec.palette_contains(BlockType::Solid);
    const bool barrel_allowed = spec.palette_contains(BlockType::Barrel);
    for (int i = 0; i < layout.volume(); ++i) {
      if ((cancel && cancel->load()) ||
          std::chrono::steady_clock::now() >= deadline)
        return result;
      Vec3 p = layout.position_of(i);
      Block block = layout.at(p);
      if (kept[i])
        continue;
      std::vector<Block> simpler;
      if (solid_allowed && simplifies_to_solid(block.type))
        simpler.push_back(Block::solid());
      if (barrel_allowed && block.type == BlockType::Chest)
        simpler.push_back(Block::barrel(block.stored_signal()));
      for (const Block &replacement : simpler) {
        layout.set(p, replacement);
        if (!verify_layout(layout, spec, vectors, options)) {
          ++result.simplified_blocks;
          changed = true;
          break;
        }
        layout.set(p, block);
      }
    }
  }
  return result;
}

namespace {

int find_or_add_steady(ModelPlan &plan, const Spec &spec,
                       const InputVector &inputs) {
  for (std::size_t i = 0; i < plan.steady.size(); ++i)
    if (plan.steady[i].inputs == inputs)
      return static_cast<int>(i);
  plan.steady.push_back({inputs, spec.expected_outputs(inputs)});
  return static_cast<int>(plan.steady.size()) - 1;
}

// Settled states only: unrolling every transition up front makes the model far
// larger, and the verifier adds the transitions that actually misbehave. For
// large tables, begin with a well-spread subset; exact verification supplies
// counterexamples and every returned candidate still passes the full table.
// With one_row_each, candidates use each row's low endpoint. Fully specified
// multi-input tables also get the high endpoint, so the quick palette search
// sees weak and full-strength combinations without expanding don't-care rows.
ModelPlan initial_plan(const Spec &spec, bool one_row_each = false,
                       int max_cases = 0) {
  constexpr std::size_t kMaxInitialCases = 64;
  const std::size_t limit =
      max_cases > 0 ? std::min<std::size_t>(max_cases, kMaxInitialCases)
                    : kMaxInitialCases;
  std::vector<InputVector> candidates;
  std::set<InputVector> seen;
  auto add_candidate = [&](InputVector inputs) {
    if (seen.insert(inputs).second)
      candidates.push_back(std::move(inputs));
  };
  if (one_row_each) {
    const bool every_row_explicit =
        spec.inputs.size() > 1 &&
        std::all_of(
            spec.rows.begin(), spec.rows.end(), [](const TruthRow &row) {
              return std::none_of(row.inputs.begin(), row.inputs.end(),
                                  [](SignalRange range) {
                                    return range == SignalRange{0, kMaxSignal};
                                  });
            });
    for (const TruthRow &row : spec.rows) {
      InputVector lowest, highest;
      for (SignalRange range : row.inputs) {
        lowest.push_back(range.min);
        highest.push_back(range.max);
      }
      add_candidate(std::move(lowest));
      if (every_row_explicit)
        add_candidate(std::move(highest));
    }
  } else {
    for (InputVector inputs : spec.representative_vectors())
      add_candidate(std::move(inputs));
  }

  if (candidates.size() > limit) {
    const std::size_t count = candidates.size();
    std::size_t stride = std::max<std::size_t>(1, count / limit);
    while (std::gcd(stride, count) != 1)
      ++stride;
    std::vector<InputVector> sampled;
    sampled.reserve(kMaxInitialCases);
    for (std::size_t i = 0; i < limit; ++i)
      sampled.push_back(std::move(candidates[(i * stride) % count]));
    candidates = std::move(sampled);
  }

  ModelPlan plan;
  for (const InputVector &inputs : candidates)
    find_or_add_steady(plan, spec, inputs);
  return plan;
}

// Adds what the failure exercised to the model. Returns false if the model
// already contained it, meaning the model disagrees with the simulator.
bool add_transition(ModelPlan &plan, TransitionCase transition) {
  if (std::find(plan.transitions.begin(), plan.transitions.end(), transition) !=
      plan.transitions.end())
    return false;
  plan.transitions.push_back(transition);
  return true;
}

bool refine_plan(ModelPlan &plan, const Spec &spec,
                 const VerificationFailure &failure, bool allow_transitions) {
  if (!failure.at_identity_placement() || failure.tiling_interference)
    return false;
  std::size_t steady_before = plan.steady.size();
  int to = find_or_add_steady(plan, spec, failure.to);
  if (plan.steady.size() != steady_before && !failure.from)
    return true;

  if (failure.from) {
    int from = find_or_add_steady(plan, spec, *failure.from);
    return allow_transitions && add_transition(plan, {from, to});
  }
  if (!allow_transitions)
    return false;
  // The circuit settled somewhere other than the model's state for `to`; make
  // the model prove that state is reached from every other one.
  bool added = false;
  for (int from = 0; from < static_cast<int>(plan.steady.size()); ++from)
    if (from != to)
      added |= add_transition(plan, {from, to});
  return added;
}

} // namespace

std::string_view to_string(SolveStatus status) {
  switch (status) {
  case SolveStatus::Optimal:
    return "optimal";
  case SolveStatus::Feasible:
    return "feasible (not proven optimal)";
  case SolveStatus::Infeasible:
    return "infeasible";
  case SolveStatus::OutOfTime:
    return "out of time";
  case SolveStatus::Cancelled:
    return "cancelled";
  case SolveStatus::RefinementLimit:
    return "refinement limit reached";
  }
  return "?";
}

// Anytime loop: each round solves the model with at most half of the
// remaining time. A verified candidate becomes the best so far and later
// rounds must beat it; a rejected one refines the model or is banned. When a
// round proves nothing smaller exists, the best circuit is optimal.
namespace {

SolveReport solve_cp_sat(const Spec &spec, const SolverOptions &options,
                         const ProgressCallback &progress) {
  auto report_progress = [&](std::string message) {
    if (progress)
      progress(message);
  };
  const Clock::time_point start = Clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(Clock::now() - start).count();
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };

  ModelPlan plan = initial_plan(spec);
  std::vector<World> forbidden;
  std::map<std::pair<InputVector, InputVector>, int> rejections;
  bool previous_round_found_nothing = false;
  std::vector<InputVector> verification_vectors = spec.verification_vectors();
  int threads =
      options.threads > 0
          ? options.threads
          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

  SolveReport report;
  auto finish = [&](SolveStatus status) {
    report.status = status;
    if (report.solution) {
      LayoutRefinement refined = refine_layout(
          report.solution->layout, spec, verification_vectors,
          options.verification, options.cancel,
          std::chrono::steady_clock::now() + (options.stop_at_first
                                                  ? kFirstSolutionRefinementTime
                                                  : kFinalRefinementTime));
      report.solution->block_count -= refined.removed_blocks;
      if (refined.removed_blocks > 0 || refined.reduced_repeaters > 0 ||
          refined.simplified_blocks > 0)
        report_progress(std::format(
            "refinement: removed {} unneeded blocks, reduced {} repeater "
            "delays, simplified {} special blocks",
            refined.removed_blocks, refined.reduced_repeaters,
            refined.simplified_blocks));
    }
    report.seconds = elapsed();
    return report;
  };

  for (int iteration = 0; iteration <= options.max_refinements; ++iteration) {
    if (cancelled())
      return finish(SolveStatus::Cancelled);
    EncoderOptions encoder_options{.forbidden_layouts = forbidden};
    if (report.solution)
      encoder_options.max_blocks = report.solution->block_count - 1;
    CircuitEncoder encoder(spec, plan, encoder_options);
    sat::CpModelProto proto = encoder.model().Build();
    double remaining = options.time_limit_seconds - elapsed();
    if (remaining <= 0)
      return finish(SolveStatus::OutOfTime);
    report_progress(std::format(
        "round {}: {} steady states, {} transitions, {} variables{}", iteration,
        plan.steady.size(), plan.transitions.size(), proto.variables_size(),
        report.solution ? std::format(", looking for fewer than {} blocks",
                                      report.solution->block_count)
                        : std::string()));

    sat::SatParameters parameters;
    // A round that found nothing gets no second chance to restart from
    // scratch: the next one gets all the remaining time.
    bool last_chance = options.stop_at_first || previous_round_found_nothing ||
                       remaining < kMinRoundSeconds * 2;
    parameters.set_max_time_in_seconds(
        last_chance ? remaining : std::max(kMinRoundSeconds, remaining / 2));
    parameters.set_num_workers(threads);
    parameters.set_log_search_progress(options.log_search);
    parameters.set_stop_after_first_solution(options.stop_at_first);
    sat::Model solver_model;
    solver_model.Add(sat::NewSatParameters(parameters));
    // CP-SAT's parallel workers only stop through StopSearch, so a watcher
    // forwards the cancel flag while the round runs.
    std::atomic<bool> round_done = false;
    std::thread watcher;
    if (options.cancel)
      watcher = std::thread([&] {
        while (!round_done.load()) {
          if (options.cancel->load())
            sat::StopSearch(&solver_model);
          std::this_thread::sleep_for(kCancelPollInterval);
        }
      });
    sat::CpSolverResponse response = sat::SolveCpModel(proto, &solver_model);
    round_done = true;
    if (watcher.joinable())
      watcher.join();
    report.refinements = iteration;

    switch (response.status()) {
    case sat::CpSolverStatus::OPTIMAL:
    case sat::CpSolverStatus::FEASIBLE:
      break;
    case sat::CpSolverStatus::INFEASIBLE:
      // With a best circuit in hand this proves nothing smaller exists.
      return finish(report.solution ? SolveStatus::Optimal
                                    : SolveStatus::Infeasible);
    case sat::CpSolverStatus::UNKNOWN:
      if (cancelled())
        return finish(SolveStatus::Cancelled);
      previous_round_found_nothing = true;
      continue;
    default:
      throw std::logic_error("CP-SAT rejected the model: " +
                             response.solution_info());
    }

    previous_round_found_nothing = false;
    bool optimal = response.status() == sat::CpSolverStatus::OPTIMAL;
    EncodedSolution candidate = encoder.decode(response);
    report_progress(std::format(
        "candidate with {} blocks ({}), verifying", candidate.block_count,
        optimal ? "optimal for the model" : "feasible"));
    std::optional<VerificationFailure> failure = verify_layout(
        candidate.layout, spec, verification_vectors, options.verification);
    if (!failure) {
      report_progress(
          std::format("verified a {}-block circuit", candidate.block_count));
      report.solution = std::move(candidate);
      if (optimal)
        return finish(SolveStatus::Optimal);
      if (options.stop_at_first)
        return finish(SolveStatus::Feasible);
      continue;
    }

    if (options.show_rejected)
      report_progress("rejected layout:\n" + list_blocks(candidate.layout));
    std::string where =
        failure->from ? std::format("{} -> {}", to_string(*failure->from),
                                    to_string(failure->to))
                      : std::format("settling at {}", to_string(failure->to));
    int &count = rejections[{failure->from.value_or(failure->to), failure->to}];
    bool allow_transitions =
        !options.stop_at_first && ++count >= kRejectionsBeforeTransition;
    if (refine_plan(plan, spec, *failure, allow_transitions)) {
      report_progress(
          std::format("rejected ({}: {}); adding that case to the model", where,
                      failure->reason));
    } else {
      report_progress(std::format(
          "rejected ({}: {}, origin {} {} {}, rotation {}{}); banning this "
          "layout",
          where, failure->reason, failure->placement.origin.x,
          failure->placement.origin.y, failure->placement.origin.z,
          failure->placement.symmetry.quarter_turns(),
          failure->placement.symmetry.mirrored() ? " mirrored" : ""));
      forbidden.push_back(candidate.layout);
    }
  }
  return finish(SolveStatus::RefinementLimit);
}

// Extra searches: seeds this far apart, and the settled states the lazy ones
// start from.
constexpr int kExtraSeedStride = 1000;
constexpr int kLazyCases = 2;
// Loops forbidden per candidate that has some (lazy loops).
constexpr int kLoopClausesPerModel = 64;
constexpr int kLoopRoundsPerMessage = 500;

// Blocks the quick search may place (those the spec's palette allows).
constexpr std::array<BlockType, 4> kQuickPalette{
    BlockType::Solid, BlockType::Dust, BlockType::Torch, BlockType::Repeater};

// What the SAT searches running side by side share.
struct SharedSearch {
  std::mutex mutex;
  std::optional<EncodedSolution> best; // guarded by mutex
  std::atomic<int> best_blocks = std::numeric_limits<int>::max();
  // Set once the answer is settled (optimality proven, or a first circuit
  // found in stop-at-first mode); every search then stops.
  std::atomic<bool> finished = false;
  // Recent rejected candidates (guarded by mutex): near misses Evolve may
  // start from.
  std::deque<World> near_misses;
};

// Rejected candidates kept for Evolve; older ones make way.
constexpr std::size_t kMaxNearMisses = 16;

struct SatSearch {
  std::string name;
  Spec spec;
  bool one_row_each = false; // start from row endpoints where fully specified
  // Its palette is the spec's, so running out of circuits proves the best
  // one optimal.
  bool complete = false;
  // Set when another search finds a better circuit, so this one stops its
  // current solve and takes the tighter bound.
  std::atomic<bool> interrupt = false;
  int seed_offset = 0;   // added to the SAT seed
  int initial_cases = 0; // 0: as the options say
};

// Settles the answer: every search stops.
void finish_all(SharedSearch &shared,
                std::vector<std::unique_ptr<SatSearch>> &searches) {
  shared.finished = true;
  for (auto &search : searches)
    search->interrupt = true;
}

// Keeps `found` if it beats the best so far; the other searches then stop
// their solve to take the tighter bound.
void publish(SharedSearch &shared,
             std::vector<std::unique_ptr<SatSearch>> &searches,
             const SatSearch &from, EncodedSolution found) {
  std::lock_guard lock(shared.mutex);
  if (found.block_count >= shared.best_blocks)
    return;
  shared.best_blocks = found.block_count;
  shared.best = std::move(found);
  for (auto &other : searches)
    if (other.get() != &from)
      other->interrupt = true;
}

enum class SearchEnd {
  Proven,
  Exhausted,
  Found,
  Stopped,
  Cancelled,
  OutOfTime,
  RefinementLimit
};

struct SearchResult {
  SearchEnd end = SearchEnd::Stopped;
  int refinements = 0;
  int loop_rounds = 0; // models sent back for a loop (lazy loops)
  std::int64_t conflicts = 0;
};

// Incremental loop on one SAT portfolio: a verified circuit tightens the
// block bound, a rejected one is banned or adds the case it failed (new
// steady states or transitions), all as new clauses, so the solvers keep
// what they learned throughout.
SearchResult run_sat_search(SatSearch &search,
                            std::vector<std::unique_ptr<SatSearch>> &searches,
                            SharedSearch &shared, const SolverOptions &options,
                            Clock::time_point deadline,
                            const std::function<void(std::string)> &say) {
  const Spec &spec = search.spec;
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  ModelPlan plan = initial_plan(
      spec, search.one_row_each,
      search.initial_cases > 0 ? search.initial_cases : options.initial_cases);
  std::map<std::pair<InputVector, InputVector>, int> rejections;
  std::vector<InputVector> verification_vectors = spec.verification_vectors();
  int threads =
      options.threads > 0
          ? options.threads
          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  int workers = std::max(1, std::min(threads, options.max_sat_workers));

  CircuitEncoder encoder(spec, plan,
                         EncoderOptions{.minimize_blocks = false,
                                        .build_cp_model = false,
                                        .lazy_loops = options.lazy_loops});
  SatPortfolio portfolio(workers, options.sat_seed + search.seed_offset);
  std::size_t clauses_sent = 0;
  int applied_bound = std::numeric_limits<int>::max();
  say(std::format("model: {} steady states, {} variables, {} clauses",
                  plan.steady.size(), encoder.variable_count(),
                  encoder.clauses().size()));
  SearchResult result;
  auto end = [&](SearchEnd how) {
    result.end = how;
    result.conflicts = portfolio.conflicts();
    return result;
  };

  // A sketch (hint lines) is first assumed right everywhere. When that is
  // impossible, the solver names the cells it could not keep; those and
  // their neighbours are freed and the rest still assumed, so the search
  // repairs the sketch around its mistakes instead of starting over.
  std::vector<SketchCell> sketch;
  if (!spec.hints.empty()) {
    std::vector<SketchCell> cells = encoder.sketch_cells();
    std::erase_if(cells, [](const SketchCell &c) { return c.literal == 0; });
    sketch = std::move(cells);
    say(std::format("sketch: assuming {} cells as drawn", sketch.size()));
  }

  while (result.refinements <= options.max_refinements) {
    if (cancelled())
      return end(SearchEnd::Cancelled);
    if (shared.finished)
      return end(SearchEnd::Stopped);
    // Cleared before reading the bound, so a better circuit published from
    // here on interrupts the coming solve.
    search.interrupt = false;
    int bound = shared.best_blocks;
    if (bound < applied_bound) {
      encoder.limit_blocks(bound - 1);
      applied_bound = bound;
    }
    if (!portfolio.add(encoder.clauses(), clauses_sent,
                       [&] { return cancelled() || Clock::now() >= deadline; }))
      return end(cancelled() ? SearchEnd::Cancelled : SearchEnd::OutOfTime);
    if (clauses_sent == 0) {
      portfolio.prefer(encoder.preferred_literals());
      if (options.lazy_loops)
        portfolio.forbid_loops(
            static_cast<int>(spec.size.x * spec.size.y * spec.size.z),
            encoder.loop_edges());
    }
    clauses_sent = encoder.clauses().size();

    std::vector<int> assumptions;
    for (const SketchCell &cell : sketch)
      assumptions.push_back(cell.literal);
    Clock::time_point solve_deadline =
        assumptions.empty()
            ? deadline
            : std::min(deadline,
                       Clock::now() +
                           std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(
                                   options.sketch_step_seconds)));
    SatPortfolio::Outcome outcome = portfolio.solve(
        solve_deadline, options.cancel, &search.interrupt, assumptions);
    if (outcome == SatPortfolio::Outcome::Stopped) {
      if (cancelled())
        return end(SearchEnd::Cancelled);
      if (shared.finished)
        return end(SearchEnd::Stopped);
      if (Clock::now() >= deadline)
        return end(SearchEnd::OutOfTime);
      if (!assumptions.empty() && Clock::now() >= solve_deadline) {
        sketch.clear();
        say("sketch: repair is slow; searching freely, trying the sketch "
            "first");
        continue;
      }
      continue; // interrupted by a better circuit: take its bound
    }
    if (outcome == SatPortfolio::Outcome::Unsatisfiable &&
        !assumptions.empty()) {
      std::vector<int> failed = portfolio.failed(assumptions);
      if (!failed.empty()) {
        std::set<int> failed_set(failed.begin(), failed.end());
        auto key = [](Vec3 p) { return std::array<int, 3>{p.x, p.y, p.z}; };
        std::set<std::array<int, 3>> freed;
        std::string culprits;
        for (const SketchCell &cell : sketch)
          if (failed_set.contains(cell.literal)) {
            freed.insert(key(cell.position));
            for (Dir d : kAllDirs)
              freed.insert(key(cell.position + offset(d)));
            culprits += std::format(" ({},{},{})", cell.position.x,
                                    cell.position.y, cell.position.z);
          }
        std::erase_if(sketch, [&](const SketchCell &cell) {
          return freed.contains(key(cell.position));
        });
        say(std::format("sketch: freed cells the solver could not keep{}; {} "
                        "still as drawn",
                        culprits, sketch.size()));
        continue;
      }
    }
    if (outcome == SatPortfolio::Outcome::Unsatisfiable) {
      if (!search.complete)
        return end(SearchEnd::Exhausted);
      finish_all(shared, searches);
      return end(SearchEnd::Proven);
    }

    std::vector<bool> values = portfolio.model(encoder.variable_count());
    if (options.lazy_loops &&
        encoder.add_loop_clauses(values, kLoopClausesPerModel) > 0) {
      if (++result.loop_rounds % kLoopRoundsPerMessage == 0)
        say(std::format("{} layouts with signal loops forbidden so far",
                        result.loop_rounds));
      continue; // a loop through a component: forbidden, solve again
    }
    ++result.refinements;
    EncodedSolution candidate = encoder.decode(values);
    say(std::format("candidate with {} blocks, verifying",
                    candidate.block_count));
    std::optional<VerificationFailure> failure = verify_layout(
        candidate.layout, spec, verification_vectors, options.verification);
    if (!failure) {
      say(std::format("verified a {}-block circuit, refining",
                      candidate.block_count));
      LayoutRefinement refined = refine_layout(
          candidate.layout, spec, verification_vectors, options.verification,
          options.cancel,
          std::min(deadline, Clock::now() + (options.stop_at_first
                                                 ? kFirstSolutionRefinementTime
                                                 : kFinalRefinementTime)));
      candidate.block_count -= refined.removed_blocks;
      int blocks = candidate.block_count;
      say(std::format(
          "refined to {} blocks (removed {}, reduced {} repeater delays, "
          "simplified {} special blocks)",
          blocks, refined.removed_blocks, refined.reduced_repeaters,
          refined.simplified_blocks));
      // The sketch has done its job; smaller circuits may look different.
      sketch.clear();
      if (options.preview && blocks < shared.best_blocks)
        options.preview(candidate.layout);
      publish(shared, searches, search, std::move(candidate));
      if (options.stop_at_first) {
        finish_all(shared, searches);
        return end(SearchEnd::Found);
      }
      continue;
    }

    if (options.show_rejected)
      say("rejected layout:\n" + list_blocks(candidate.layout));
    std::string where =
        failure->from ? std::format("{} -> {}", to_string(*failure->from),
                                    to_string(failure->to))
                      : std::format("settling at {}", to_string(failure->to));
    int &count = rejections[{failure->from.value_or(failure->to), failure->to}];
    // Rejections are cheap here, so even first-circuit mode would otherwise
    // ban layout after layout that fail the same input change; it waits
    // longer since the transition makes the model several times larger.
    bool allow_transitions =
        ++count >= (options.stop_at_first ? kFirstRejectionsBeforeTransition
                                          : kRejectionsBeforeTransition);
    {
      std::lock_guard lock(shared.mutex);
      shared.near_misses.push_back(candidate.layout);
      if (shared.near_misses.size() > kMaxNearMisses)
        shared.near_misses.pop_front();
    }
    std::size_t steady_before = plan.steady.size();
    std::size_t transitions_before = plan.transitions.size();
    bool refined = refine_plan(plan, spec, *failure, allow_transitions);
    // New cases only add clauses, so the solvers keep what they learned.
    // refine_plan may add steady states even when it reports no refinement.
    for (std::size_t i = steady_before; i < plan.steady.size(); ++i)
      encoder.add_steady(plan.steady[i]);
    for (std::size_t i = transitions_before; i < plan.transitions.size(); ++i)
      encoder.add_transition(plan.transitions[i], plan.steady);
    if (refined) {
      say(std::format("rejected ({}: {}); adding that case to the model", where,
                      failure->reason));
      say(std::format("model: {} steady states, {} transitions, {} variables, "
                      "{} clauses",
                      plan.steady.size(), plan.transitions.size(),
                      encoder.variable_count(), encoder.clauses().size()));
    } else {
      say(std::format(
          "rejected ({}: {}, origin {} {} {}, rotation {}{}); banning this "
          "layout",
          where, failure->reason, failure->placement.origin.x,
          failure->placement.origin.y, failure->placement.origin.z,
          failure->placement.symmetry.quarter_turns(),
          failure->placement.symmetry.mirrored() ? " mirrored" : ""));
      encoder.forbid_layout(candidate.layout);
    }
  }
  return end(SearchEnd::RefinementLimit);
}

// Evolve beside the SAT searches (2-high boxes): it judges grids by the
// simulator, so it often finds circuits the SAT model struggles with
// (loops, tricky analog ones), and it starts some runs from the SAT
// search's rejected candidates, near misses it can repair. A circuit it
// finds is refined and published like any other; in first-circuit mode it
// settles the answer. Returns whether it found one.
bool evolve_alongside(const Spec &spec, const SolverOptions &options,
                      SharedSearch &shared,
                      std::vector<std::unique_ptr<SatSearch>> &searches,
                      Clock::time_point deadline, std::atomic<bool> &stop,
                      const std::function<void(std::string)> &say) {
  EvolveOptions o;
  o.time_limit_seconds =
      std::chrono::duration<double>(deadline - Clock::now()).count();
  if (o.time_limit_seconds <= 0)
    return false;
  const int hardware =
      options.threads > 0
          ? options.threads
          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  o.threads = options.evolve_threads > 0
                  ? options.evolve_threads
                  : std::max(1, hardware - static_cast<int>(searches.size()));
  o.seed = static_cast<unsigned>(options.sat_seed) + 1;
  o.verification = options.verification;
  o.cancel = &stop;
  o.fresh_start = [&shared]() -> std::optional<World> {
    std::lock_guard lock(shared.mutex);
    if (shared.near_misses.empty())
      return std::nullopt;
    World w = std::move(shared.near_misses.front());
    shared.near_misses.pop_front();
    return w;
  };
  EvolveResult result;
  try {
    result = evolve(spec, o, [&](std::string_view m) { say(std::string(m)); });
  } catch (const std::invalid_argument &e) {
    say(std::format("not used here: {}", e.what()));
    return false;
  }
  if (!result.layout || shared.finished)
    return false;
  World layout = std::move(*result.layout);
  refine_layout(
      layout, spec, spec.verification_vectors(), options.verification,
      options.cancel,
      std::min(deadline, Clock::now() + (options.stop_at_first
                                             ? kFirstSolutionRefinementTime
                                             : kFinalRefinementTime)));
  int blocks = 0;
  for (int i = 0; i < layout.volume(); ++i) {
    const BlockType t = layout.at(layout.position_of(i)).type;
    if (t != BlockType::Air && t != BlockType::Input && t != BlockType::Output)
      ++blocks;
  }
  say(std::format("a {}-block circuit, published to the other searches",
                  blocks));
  if (options.preview && blocks < shared.best_blocks)
    options.preview(layout);
  SatSearch self; // not one of `searches`: every one of them takes the bound
  publish(shared, searches, self, EncodedSolution{layout, blocks, {}, {}, {}});
  if (options.stop_at_first)
    finish_all(shared, searches);
  return true;
}

// The main search on the spec's palette, plus (unless the palette is already
// that small) a quick one on a few basic blocks that starts from one input
// vector per truth row. Both run at once and share the best circuit.
SolveReport solve_sat(const Spec &spec, const SolverOptions &options,
                      const ProgressCallback &progress) {
  const Clock::time_point start = Clock::now();
  const Clock::time_point deadline =
      start + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(options.time_limit_seconds));

  std::vector<std::unique_ptr<SatSearch>> searches;
  auto main_search = std::make_unique<SatSearch>();
  main_search->spec = spec;
  main_search->complete = true;
  searches.push_back(std::move(main_search));
  for (int k = 1; k <= options.extra_searches; ++k) {
    auto search = std::make_unique<SatSearch>();
    search->name = std::format("seed {}", k);
    search->spec = spec;
    search->complete = true;
    search->seed_offset = k * kExtraSeedStride;
    search->initial_cases = k % 2 == 1 ? kLazyCases : 0;
    searches.push_back(std::move(search));
  }
  if (options.quick_palette_search) {
    Spec quick = spec;
    quick.palette.clear();
    for (BlockType type : kQuickPalette)
      if (spec.palette_contains(type))
        quick.palette.push_back(type);
    if (!quick.palette.empty() && quick.palette.size() < spec.palette.size()) {
      auto search = std::make_unique<SatSearch>();
      search->name = "basic blocks";
      search->spec = std::move(quick);
      search->one_row_each = true;
      searches.push_back(std::move(search));
    }
  }

  std::mutex progress_mutex;
  SharedSearch shared;
  std::vector<SearchResult> results(searches.size());
  std::atomic<bool> evolve_stop = false;
  bool evolve_found = false;
  std::jthread evolver;
  if (options.evolve_alongside && spec.size.y == 2)
    evolver = std::jthread([&] {
      evolve_found = evolve_alongside(spec, options, shared, searches, deadline,
                                      evolve_stop, [&](std::string message) {
                                        if (!progress)
                                          return;
                                        std::lock_guard lock(progress_mutex);
                                        progress("[evolve] " + message);
                                      });
    });
  {
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < searches.size(); ++i)
      threads.emplace_back([&, i] {
        SatSearch &search = *searches[i];
        auto say = [&](std::string message) {
          if (!progress)
            return;
          std::lock_guard lock(progress_mutex);
          progress(search.name.empty()
                       ? message
                       : std::format("[{}] {}", search.name, message));
        };
        results[i] =
            run_sat_search(search, searches, shared, options, deadline, say);
        if (results[i].end == SearchEnd::Exhausted)
          say(shared.best_blocks < std::numeric_limits<int>::max()
                  ? "no smaller circuit with these blocks; the main search "
                    "goes on"
                  : "no circuit with only these blocks; the main search goes "
                    "on");
      });
  }

  // The SAT searches are done (settled, cancelled or out of time): so is
  // Evolve.
  evolve_stop = true;
  if (evolver.joinable())
    evolver.join();

  SolveReport report;
  report.solution = std::move(shared.best);
  for (const SearchResult &result : results) {
    report.refinements += result.refinements;
    report.sat_conflicts += result.conflicts;
  }
  auto ended = [&](SearchEnd how) {
    return std::any_of(results.begin(), results.end(),
                       [&](const SearchResult &r) { return r.end == how; });
  };
  SolveStatus status = SolveStatus::OutOfTime;
  if (ended(SearchEnd::Proven))
    status = report.solution ? SolveStatus::Optimal : SolveStatus::Infeasible;
  else if (ended(SearchEnd::Cancelled))
    status = SolveStatus::Cancelled;
  else if (ended(SearchEnd::Found) || (evolve_found && options.stop_at_first))
    status = SolveStatus::Feasible;
  else if (results[0].end == SearchEnd::RefinementLimit)
    status = SolveStatus::RefinementLimit;

  report.status = status;
  report.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return report;
}

} // namespace

SolveReport solve(const Spec &spec, const SolverOptions &options,
                  const ProgressCallback &progress) {
  spec.validate();
  return options.engine == SolverEngine::Sat
             ? solve_sat(spec, options, progress)
             : solve_cp_sat(spec, options, progress);
}

} // namespace rs

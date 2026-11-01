#include "redstone/solver.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_parameters.pb.h"
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

// Removes blocks one at a time while the layout still passes verification.
// Cheap compared to solving, and it cleans up first-found solutions.
int prune_layout(World &layout, const Spec &spec,
                 const std::vector<InputVector> &vectors,
                 const VerificationOptions &options,
                 const std::atomic<bool> *cancel) {
  int removed = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < layout.volume(); ++i) {
      if (cancel && cancel->load())
        return removed;
      Vec3 p = layout.position_of(i);
      Block block = layout.at(p);
      if (block.type == BlockType::Air || block.type == BlockType::Input ||
          block.type == BlockType::Output)
        continue;
      layout.set(p, Block::air());
      if (verify_layout(layout, spec, vectors, options)) {
        layout.set(p, block);
        continue;
      }
      ++removed;
      changed = true;
    }
  }
  return removed;
}

int find_or_add_steady(ModelPlan &plan, const Spec &spec,
                       const InputVector &inputs) {
  for (std::size_t i = 0; i < plan.steady.size(); ++i)
    if (plan.steady[i].inputs == inputs)
      return static_cast<int>(i);
  plan.steady.push_back({inputs, spec.expected_outputs(inputs)});
  return static_cast<int>(plan.steady.size()) - 1;
}

// Settled states only: unrolling every transition up front makes the model far
// larger, and the verifier adds the transitions that actually misbehave.
ModelPlan initial_plan(const Spec &spec) {
  ModelPlan plan;
  for (const InputVector &inputs : spec.representative_vectors())
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
    if (report.solution && status != SolveStatus::Optimal) {
      report.status = SolveStatus::Feasible;
      int removed =
          prune_layout(report.solution->layout, spec, verification_vectors,
                       options.verification, options.cancel);
      report.solution->block_count -= removed;
      if (removed > 0)
        report_progress(std::format("removed {} unneeded blocks", removed));
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

// Incremental loop on one SAT portfolio: a verified circuit tightens the
// block bound, a rejected one is banned or adds the case it failed (new
// steady states or transitions), all as new clauses, so the solvers keep
// what they learned throughout.
SolveReport solve_sat(const Spec &spec, const SolverOptions &options,
                      const ProgressCallback &progress) {
  auto report_progress = [&](std::string message) {
    if (progress)
      progress(message);
  };
  const Clock::time_point start = Clock::now();
  const Clock::time_point deadline =
      start + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(options.time_limit_seconds));
  auto elapsed = [&] {
    return std::chrono::duration<double>(Clock::now() - start).count();
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };

  ModelPlan plan = initial_plan(spec);
  std::map<std::pair<InputVector, InputVector>, int> rejections;
  std::vector<InputVector> verification_vectors = spec.verification_vectors();
  int threads =
      options.threads > 0
          ? options.threads
          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  int workers = std::max(1, std::min(threads, options.max_sat_workers));

  SolveReport report;
  auto finish = [&](SolveStatus status) {
    report.status = status;
    if (report.solution && status != SolveStatus::Optimal) {
      report.status = SolveStatus::Feasible;
      int removed =
          prune_layout(report.solution->layout, spec, verification_vectors,
                       options.verification, options.cancel);
      report.solution->block_count -= removed;
      if (removed > 0)
        report_progress(std::format("removed {} unneeded blocks", removed));
    }
    report.seconds = elapsed();
    return report;
  };

  CircuitEncoder encoder(
      spec, plan,
      EncoderOptions{.minimize_blocks = false, .build_cp_model = false});
  SatPortfolio portfolio(workers, options.sat_seed);
  std::size_t clauses_sent = 0;
  report_progress(std::format(
      "model: {} steady states, {} variables, {} clauses", plan.steady.size(),
      encoder.variable_count(), encoder.clauses().size()));
  auto finish_sat = [&](SolveStatus status) {
    report.sat_conflicts = portfolio.conflicts();
    return finish(status);
  };
  for (int iteration = 0; iteration <= options.max_refinements; ++iteration) {
    if (cancelled())
      return finish_sat(SolveStatus::Cancelled);
    portfolio.add(encoder.clauses(), clauses_sent);
    clauses_sent = encoder.clauses().size();

    SatPortfolio::Outcome outcome = portfolio.solve(deadline, options.cancel);
    report.refinements = iteration;
    if (outcome == SatPortfolio::Outcome::Stopped)
      return finish_sat(cancelled() ? SolveStatus::Cancelled
                                    : SolveStatus::OutOfTime);
    if (outcome == SatPortfolio::Outcome::Unsatisfiable)
      return finish_sat(report.solution ? SolveStatus::Optimal
                                        : SolveStatus::Infeasible);

    EncodedSolution candidate =
        encoder.decode(portfolio.model(encoder.variable_count()));
    report_progress(std::format("candidate with {} blocks, verifying",
                                candidate.block_count));
    std::optional<VerificationFailure> failure = verify_layout(
        candidate.layout, spec, verification_vectors, options.verification);
    if (!failure) {
      // Pruning is cheap next to solving and lets the bound drop in jumps.
      candidate.block_count -=
          prune_layout(candidate.layout, spec, verification_vectors,
                       options.verification, options.cancel);
      report_progress(
          std::format("verified a {}-block circuit", candidate.block_count));
      int blocks = candidate.block_count;
      report.solution = std::move(candidate);
      if (options.stop_at_first)
        return finish_sat(SolveStatus::Feasible);
      encoder.limit_blocks(blocks - 1);
      continue;
    }

    if (options.show_rejected)
      report_progress("rejected layout:\n" + list_blocks(candidate.layout));
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
      report_progress(
          std::format("rejected ({}: {}); adding that case to the model", where,
                      failure->reason));
      report_progress(std::format("model: {} steady states, {} transitions, "
                                  "{} variables, {} clauses",
                                  plan.steady.size(), plan.transitions.size(),
                                  encoder.variable_count(),
                                  encoder.clauses().size()));
    } else {
      report_progress(std::format(
          "rejected ({}: {}, origin {} {} {}, rotation {}{}); banning this "
          "layout",
          where, failure->reason, failure->placement.origin.x,
          failure->placement.origin.y, failure->placement.origin.z,
          failure->placement.symmetry.quarter_turns(),
          failure->placement.symmetry.mirrored() ? " mirrored" : ""));
      encoder.forbid_layout(candidate.layout);
    }
  }
  return finish_sat(SolveStatus::RefinementLimit);
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

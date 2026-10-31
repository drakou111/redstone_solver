#include "redstone/solver.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <stdexcept>
#include <thread>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/util/time_limit.h"
#include "redstone/report.hpp"

namespace rs {

namespace sat = operations_research::sat;

namespace {

using Clock = std::chrono::steady_clock;
// A solver round never gets less than this unless it is the last one.
constexpr double kMinRoundSeconds = 5.0;
// Transitions make the model several times larger, so a misbehaving input
// change is first answered by banning layouts; only once it has caused this
// many rejections is it added to the model.
constexpr int kRejectionsBeforeTransition = 3;

// Removes blocks one at a time while the layout still passes verification.
// Cheap compared to solving, and it cleans up first-found solutions.
int prune_layout(World &layout, const Spec &spec,
                 const std::vector<InputVector> &vectors,
                 const VerificationOptions &options) {
  int removed = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < layout.volume(); ++i) {
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
SolveReport solve(const Spec &spec, const SolverOptions &options,
                  const ProgressCallback &progress) {
  spec.validate();
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
      int removed = prune_layout(report.solution->layout, spec,
                                 verification_vectors, options.verification);
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
    bool last_chance =
        previous_round_found_nothing || remaining < kMinRoundSeconds * 2;
    parameters.set_max_time_in_seconds(
        last_chance ? remaining : std::max(kMinRoundSeconds, remaining / 2));
    parameters.set_num_workers(threads);
    parameters.set_log_search_progress(options.log_search);
    parameters.set_stop_after_first_solution(options.stop_at_first);
    sat::Model solver_model;
    solver_model.Add(sat::NewSatParameters(parameters));
    if (options.cancel)
      solver_model.GetOrCreate<operations_research::TimeLimit>()
          ->RegisterExternalBooleanAsLimit(options.cancel);
    sat::CpSolverResponse response = sat::SolveCpModel(proto, &solver_model);
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

} // namespace rs

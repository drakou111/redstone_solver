#include "redstone/solver.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "redstone/report.hpp"

namespace rs {

namespace sat = operations_research::sat;

namespace {

using Clock = std::chrono::steady_clock;

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
                 const VerificationFailure &failure) {
  if (!failure.at_identity_placement() || failure.tiling_interference)
    return false;
  std::size_t steady_before = plan.steady.size();
  int to = find_or_add_steady(plan, spec, failure.to);
  if (plan.steady.size() != steady_before && !failure.from)
    return true;

  if (failure.from) {
    int from = find_or_add_steady(plan, spec, *failure.from);
    return add_transition(plan, {from, to});
  }
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
  case SolveStatus::RefinementLimit:
    return "refinement limit reached";
  }
  return "?";
}

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

  ModelPlan plan = initial_plan(spec);
  std::vector<World> forbidden;
  std::vector<InputVector> verification_vectors = spec.verification_vectors();
  int threads =
      options.threads > 0
          ? options.threads
          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

  SolveReport report;
  for (int iteration = 0; iteration <= options.max_refinements; ++iteration) {
    if (elapsed() >= options.time_limit_seconds)
      break;
    CircuitEncoder encoder(spec, plan,
                           EncoderOptions{.forbidden_layouts = forbidden});
    sat::CpModelProto proto = encoder.model().Build();
    double remaining = options.time_limit_seconds - elapsed();
    if (remaining <= 0)
      break;
    report_progress(std::format("iteration {}: {} steady states, {} "
                                "transitions, {} variables, {} constraints",
                                iteration, plan.steady.size(),
                                plan.transitions.size(), proto.variables_size(),
                                proto.constraints_size()));

    sat::SatParameters parameters;
    parameters.set_max_time_in_seconds(remaining);
    parameters.set_num_workers(threads);
    parameters.set_log_search_progress(options.log_search);
    parameters.set_stop_after_first_solution(options.stop_at_first);
    sat::CpSolverResponse response =
        sat::SolveWithParameters(proto, parameters);
    report.refinements = iteration;

    switch (response.status()) {
    case sat::CpSolverStatus::OPTIMAL:
    case sat::CpSolverStatus::FEASIBLE:
      break;
    case sat::CpSolverStatus::INFEASIBLE:
      report.status = SolveStatus::Infeasible;
      report.seconds = elapsed();
      return report;
    case sat::CpSolverStatus::UNKNOWN:
      report.status = SolveStatus::OutOfTime;
      report.seconds = elapsed();
      return report;
    default:
      throw std::logic_error("CP-SAT rejected the model: " +
                             response.solution_info());
    }

    EncodedSolution candidate = encoder.decode(response);
    report_progress(std::format(
        "candidate with {} blocks ({}), verifying", candidate.block_count,
        response.status() == sat::CpSolverStatus::OPTIMAL ? "optimal"
                                                          : "feasible"));
    std::optional<VerificationFailure> failure = verify_layout(
        candidate.layout, spec, verification_vectors, options.verification);
    if (!failure) {
      bool optimal = response.status() == sat::CpSolverStatus::OPTIMAL;
      if (!optimal) {
        int removed = prune_layout(candidate.layout, spec, verification_vectors,
                                   options.verification);
        candidate.block_count -= removed;
        if (removed > 0)
          report_progress(std::format("removed {} unneeded blocks", removed));
      }
      report.status = optimal ? SolveStatus::Optimal : SolveStatus::Feasible;
      report.solution = std::move(candidate);
      report.seconds = elapsed();
      return report;
    }

    if (options.show_rejected)
      report_progress("rejected layout:\n" + list_blocks(candidate.layout));
    std::string where =
        failure->from ? std::format("{} -> {}", to_string(*failure->from),
                                    to_string(failure->to))
                      : std::format("settling at {}", to_string(failure->to));
    if (refine_plan(plan, spec, *failure)) {
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

  report.status = elapsed() >= options.time_limit_seconds
                      ? SolveStatus::OutOfTime
                      : SolveStatus::RefinementLimit;
  report.seconds = elapsed();
  return report;
}

} // namespace rs

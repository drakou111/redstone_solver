#include "redstone/verifier.hpp"

#include <format>
#include <future>
#include <stdexcept>

#include "redstone/simulator.hpp"

namespace rs {

namespace {

constexpr std::array<Vec3, 3> kOrigins{Vec3{0, 0, 0}, Vec3{5, 1, 3},
                                       Vec3{-7, 64, 12}};

std::vector<int> read_outputs(const Simulator &sim, std::size_t count) {
  std::vector<int> values;
  for (std::size_t o = 0; o < count; ++o)
    values.push_back(sim.output(static_cast<int>(o)));
  return values;
}

void apply_inputs(Simulator &sim, const InputVector &inputs) {
  for (std::size_t i = 0; i < inputs.size(); ++i)
    sim.set_input(static_cast<int>(i), inputs[i]);
}

std::optional<std::string>
outside_range(const std::vector<int> &outputs,
              const std::vector<SignalRange> &expected, int tick) {
  for (std::size_t o = 0; o < outputs.size(); ++o)
    if (!expected[o].contains(outputs[o]))
      return std::format("OUT{} is {} at tick {}, expected {}", o + 1,
                         outputs[o], tick, to_string(expected[o]));
  return std::nullopt;
}

// Switches a settled simulator to `to` and checks the output timeline.
std::optional<std::string>
check_transition(Simulator sim, const Spec &spec, const InputVector &to,
                 const std::vector<SignalRange> &expected,
                 const VerificationOptions &options) {
  const TimingSpec &timing = spec.timing;
  const int horizon = timing.horizon();
  const int deadline = timing.latest ? *timing.latest : horizon;
  std::vector<int> previous = read_outputs(sim, spec.outputs.size());
  apply_inputs(sim, to);

  for (int t = 0; t <= horizon + options.max_extra_ticks; ++t) {
    if (t > 0)
      sim.tick();
    if (t > horizon && !sim.has_pending_ticks())
      return std::nullopt;
    std::vector<int> outputs = read_outputs(sim, spec.outputs.size());
    if (t >= deadline)
      if (auto problem = outside_range(outputs, expected, t))
        return problem;
    if (timing.hold_previous && timing.earliest && t < *timing.earliest &&
        outputs != previous)
      return std::format(
          "outputs changed at tick {}, before the earliest allowed tick {}", t,
          *timing.earliest);
  }
  return std::format("still active {} ticks after the horizon",
                     options.max_extra_ticks);
}

std::optional<VerificationFailure>
check_placement(const World &layout, const Spec &spec,
                const std::vector<InputVector> &vectors,
                const VerificationOptions &options, Placement placement) {
  auto fail = [&](std::optional<InputVector> from, InputVector to,
                  std::string reason) {
    return VerificationFailure{placement, std::move(from), std::move(to),
                               std::move(reason)};
  };

  std::optional<Simulator> built;
  try {
    built.emplace(layout, placement);
  } catch (const std::invalid_argument &error) {
    return fail(std::nullopt, vectors.front(),
                std::string("invalid layout: ") + error.what());
  }
  if (!built->run_until_quiet(options.max_settle_ticks))
    return fail(std::nullopt, vectors.front(),
                "does not settle after being built");

  std::vector<Simulator> settled;
  std::vector<std::vector<SignalRange>> expected;
  for (const InputVector &vector : vectors) {
    std::optional<std::vector<SignalRange>> wanted =
        spec.expected_outputs(vector);
    if (!wanted)
      throw std::invalid_argument("verification vector " + to_string(vector) +
                                  " has no row");
    Simulator sim = *built;
    apply_inputs(sim, vector);
    if (!sim.run_until_quiet(options.max_settle_ticks))
      return fail(std::nullopt, vector, "does not settle");
    if (auto problem =
            outside_range(read_outputs(sim, spec.outputs.size()), *wanted, 0))
      return fail(std::nullopt, vector, "settled state wrong: " + *problem);
    settled.push_back(std::move(sim));
    expected.push_back(std::move(*wanted));
  }

  for (std::size_t from = 0; from < vectors.size(); ++from) {
    for (std::size_t to = 0; to < vectors.size(); ++to) {
      if (from == to)
        continue;
      if (auto problem = check_transition(settled[from], spec, vectors[to],
                                          expected[to], options))
        return fail(vectors[from], vectors[to], *problem);
    }
  }
  return std::nullopt;
}

} // namespace

bool VerificationFailure::at_identity_placement() const {
  return this->placement.origin == Vec3{} &&
         this->placement.symmetry.quarter_turns() == 0 &&
         !this->placement.symmetry.mirrored();
}

std::vector<Placement> standard_placements() {
  std::vector<Placement> placements;
  for (Vec3 origin : kOrigins)
    for (Symmetry symmetry : Symmetry::all())
      placements.push_back({origin, symmetry});
  return placements;
}

std::optional<VerificationFailure>
verify_layout(const World &layout, const Spec &spec,
              const std::vector<InputVector> &vectors,
              const VerificationOptions &options) {
  if (vectors.empty())
    throw std::invalid_argument("nothing to verify: no input vectors");
  std::vector<Placement> placements =
      options.placements.empty() ? standard_placements() : options.placements;

  std::vector<std::future<std::optional<VerificationFailure>>> checks;
  for (const Placement &placement : placements)
    checks.push_back(std::async(
        std::launch::async, check_placement, std::cref(layout), std::cref(spec),
        std::cref(vectors), std::cref(options), placement));

  std::optional<VerificationFailure> first;
  for (auto &check : checks) {
    std::optional<VerificationFailure> failure = check.get();
    if (failure && !first)
      first = std::move(failure);
  }
  return first;
}

std::string to_string(const InputVector &vector) {
  std::string text = "[";
  for (std::size_t i = 0; i < vector.size(); ++i)
    text += std::format("{}{}", i ? " " : "", vector[i]);
  return text + "]";
}

} // namespace rs

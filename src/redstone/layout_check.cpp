#include "redstone/layout_check.hpp"

#include <format>
#include <stdexcept>

#include "redstone/simulator.hpp"

namespace rs {

namespace {

std::vector<int> outputs_of(const Simulator &sim, std::size_t count) {
  std::vector<int> values;
  for (std::size_t o = 0; o < count; ++o)
    values.push_back(sim.output(static_cast<int>(o)));
  return values;
}

void apply(Simulator &sim, const InputVector &inputs) {
  for (std::size_t i = 0; i < inputs.size(); ++i)
    sim.set_input(static_cast<int>(i), inputs[i]);
}

bool within(const std::vector<int> &values,
            const std::vector<SignalRange> &expected) {
  for (std::size_t o = 0; o < values.size(); ++o)
    if (!expected[o].contains(values[o]))
      return false;
  return true;
}

std::string ranges_text(const std::vector<SignalRange> &ranges) {
  std::string text;
  for (std::size_t o = 0; o < ranges.size(); ++o)
    text +=
        std::format("{}OUT{} {}", o ? ", " : "", o + 1, to_string(ranges[o]));
  return text;
}

std::string values_text(const std::vector<int> &values) {
  std::string text;
  for (std::size_t o = 0; o < values.size(); ++o)
    text += std::format("{}OUT{} {}", o ? ", " : "", o + 1, values[o]);
  return text;
}

} // namespace

bool LayoutCheck::passes() const {
  if (!this->invalid.empty() || this->unstable || this->elsewhere)
    return false;
  for (const SettledCheck &s : this->settled)
    if (!s.ok)
      return false;
  for (const SwitchCheck &s : this->switches)
    if (s.problem)
      return false;
  return true;
}

World layout_from_hints(const Spec &spec) {
  for (const InputPort &port : spec.inputs)
    if (port.is_free())
      throw std::invalid_argument("checking a layout needs fixed ports");
  for (const OutputPort &port : spec.outputs)
    if (port.is_free())
      throw std::invalid_argument("checking a layout needs fixed ports");
  World layout = spec.base_world();
  for (const auto &[position, block] : spec.hints)
    layout.set(position, block);
  return layout;
}

LayoutCheck check_layout(const World &layout, const Spec &spec,
                         const VerificationOptions &options) {
  LayoutCheck check;
  check.invalid = layout.validate();
  if (!check.invalid.empty())
    return check;
  Simulator built(layout);
  if (!built.run_until_quiet(options.max_settle_ticks)) {
    check.unstable = "does not come to rest after being built";
    return check;
  }

  std::vector<InputVector> vectors = spec.verification_vectors();
  std::vector<Simulator> rest;
  std::size_t outputs = spec.outputs.size();
  for (const InputVector &inputs : vectors) {
    SettledCheck settled{inputs, {}, *spec.expected_outputs(inputs), false};
    Simulator sim = built;
    apply(sim, inputs);
    if (sim.run_until_quiet(options.max_settle_ticks)) {
      settled.outputs = outputs_of(sim, outputs);
      settled.ok = within(settled.outputs, settled.expected);
    }
    check.settled.push_back(std::move(settled));
    rest.push_back(std::move(sim));
  }

  const TimingSpec &timing = spec.timing;
  int horizon = timing.horizon();
  int deadline = timing.latest ? *timing.latest : horizon;
  for (std::size_t u = 0; u < vectors.size(); ++u) {
    if (check.settled[u].outputs.empty())
      continue;
    for (std::size_t v = 0; v < vectors.size(); ++v) {
      if (u == v || check.settled[v].outputs.empty())
        continue;
      SwitchCheck change{vectors[u], vectors[v], {}, std::nullopt};
      Simulator sim = rest[u];
      std::vector<int> before = outputs_of(sim, outputs);
      apply(sim, vectors[v]);
      for (int t = 0;; ++t) {
        if (t > 0)
          sim.tick();
        std::vector<int> now = outputs_of(sim, outputs);
        change.timeline.push_back(now);
        if (!change.problem) {
          if (t >= deadline && !within(now, check.settled[v].expected))
            change.problem =
                std::format("wrong at tick {} (deadline {})", t, deadline);
          else if (timing.hold_previous && timing.earliest &&
                   t < *timing.earliest && now != before)
            change.problem =
                std::format("changed at tick {}, before the earliest tick {}",
                            t, *timing.earliest);
        }
        if (t >= horizon && !sim.has_pending_ticks())
          break;
        if (t >= horizon + options.max_extra_ticks) {
          change.problem = "still active long after the horizon";
          break;
        }
      }
      check.switches.push_back(std::move(change));
    }
  }

  bool here = true;
  for (const SettledCheck &s : check.settled)
    here = here && s.ok;
  for (const SwitchCheck &s : check.switches)
    here = here && !s.problem;
  if (here)
    check.elsewhere = verify_layout(layout, spec, vectors, options);
  return check;
}

std::string format_check(const LayoutCheck &check, const Spec &spec) {
  std::string text;
  if (!check.invalid.empty()) {
    text += "INVALID LAYOUT\n";
    for (const std::string &problem : check.invalid)
      text += "  " + problem + "\n";
    return text;
  }
  if (check.unstable)
    return "UNSTABLE: " + *check.unstable + "\n";

  text += "Settled outputs:\n";
  for (const SettledCheck &s : check.settled) {
    if (s.outputs.empty()) {
      text +=
          std::format("  FAIL {}: never comes to rest\n", to_string(s.inputs));
      continue;
    }
    text += std::format("  {} {}: {}", s.ok ? "ok  " : "FAIL",
                        to_string(s.inputs), values_text(s.outputs));
    if (!s.ok)
      text += " (expected " + ranges_text(s.expected) + ")";
    text += "\n";
  }

  text += "Input changes (output value@first tick):\n";
  for (const SwitchCheck &s : check.switches) {
    text += std::format("  {} {} -> {}:", s.problem ? "FAIL" : "ok  ",
                        to_string(s.from), to_string(s.to));
    for (std::size_t o = 0; o < spec.outputs.size(); ++o) {
      text += std::format(" OUT{}", o + 1);
      for (std::size_t t = 0; t < s.timeline.size(); ++t)
        if (t == 0 || s.timeline[t][o] != s.timeline[t - 1][o])
          text += std::format(" {}@{}", s.timeline[t][o], t);
    }
    if (s.problem)
      text += " - " + *s.problem;
    text += "\n";
  }
  if (check.elsewhere)
    text += std::format(
        "FAIL at another position/rotation (update order): {} -> {}: {}\n",
        check.elsewhere->from ? to_string(*check.elsewhere->from) : "rest",
        to_string(check.elsewhere->to), check.elsewhere->reason);
  text += check.passes() ? "PASS\n" : "NOT PASSING\n";
  return text;
}

} // namespace rs

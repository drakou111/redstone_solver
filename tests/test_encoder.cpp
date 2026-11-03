#include <set>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "redstone/encoder.hpp"
#include "redstone/simulator.hpp"

// Last, so Catch2's CHECK replaces the one abseil (via OR-Tools) defines.
#include <catch2/catch_test_macros.hpp>

using namespace rs;
namespace sat = operations_research::sat;

namespace {

constexpr int kHorizon = 12;
constexpr int kSettleTicks = 400;
constexpr int kSolverThreads = 8;
constexpr double kSolverSeconds = 30.0;

World with_glass_floor(Vec3 size) {
  World world(size);
  for (int x = 0; x < size.x; ++x)
    for (int z = 0; z < size.z; ++z)
      world.set({x, 0, z}, Block::glass());
  return world;
}

// A spec whose ports are the layout's ports and whose rows accept anything.
Spec spec_for(const World &layout) {
  Spec spec;
  spec.size = layout.size();
  std::set<BlockType> palette;
  for (int i = 0; i < layout.volume(); ++i) {
    const Block &b = layout.at(layout.position_of(i));
    if (b.type == BlockType::Input) {
      if (static_cast<int>(spec.inputs.size()) <= b.channel)
        spec.inputs.resize(b.channel + 1);
      spec.inputs[b.channel] = {b.input_kind(), layout.position_of(i),
                                b.facing};
    } else if (b.type == BlockType::Output) {
      if (static_cast<int>(spec.outputs.size()) <= b.channel)
        spec.outputs.resize(b.channel + 1);
      spec.outputs[b.channel] = {b.output_kind(), layout.position_of(i),
                                 b.facing};
    } else if (b.type != BlockType::Air) {
      palette.insert(b.type);
    }
  }
  spec.palette.assign(palette.begin(), palette.end());
  spec.rows.push_back({std::vector<SignalRange>(spec.inputs.size()),
                       std::vector<SignalRange>(spec.outputs.size())});
  spec.timing.horizon_ticks = kHorizon;
  // The layouts stand on glass, which the support layer would not allow.
  spec.support_layer = false;
  return spec;
}

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

// Solves the model with the layout fixed and no output requirements, then
// checks that every predicted output timeline matches the simulator. Steady
// states are reached by visiting the vectors in order, as a player would.
void check_model_matches_simulator(
    const World &layout, const std::vector<InputVector> &vectors,
    const std::vector<TransitionCase> &transitions,
    bool allow_feedback = false) {
  Spec spec = spec_for(layout);
  spec.allow_feedback = allow_feedback;
  ModelPlan plan;
  for (const InputVector &v : vectors)
    plan.steady.push_back({v, std::nullopt});
  plan.transitions = transitions;
  CircuitEncoder encoder(spec, plan,
                         EncoderOptions{.enforce_outputs = false,
                                        .minimize_blocks = false,
                                        .fixed_layout = layout});

  sat::SatParameters parameters;
  parameters.set_num_workers(kSolverThreads);
  parameters.set_max_time_in_seconds(kSolverSeconds);
  sat::CpSolverResponse response =
      sat::SolveWithParameters(encoder.model().Build(), parameters);
  REQUIRE((response.status() == sat::CpSolverStatus::OPTIMAL ||
           response.status() == sat::CpSolverStatus::FEASIBLE));
  EncodedSolution solution = encoder.decode(response);

  std::vector<Simulator> settled;
  Simulator walker(layout);
  REQUIRE(walker.run_until_quiet(kSettleTicks));
  for (std::size_t i = 0; i < vectors.size(); ++i) {
    apply(walker, vectors[i]);
    REQUIRE(walker.run_until_quiet(kSettleTicks));
    INFO("steady state " << i);
    CHECK(outputs_of(walker, spec.outputs.size()) ==
          solution.steady_outputs[i]);
    settled.push_back(walker);
  }

  for (const TransitionTrace &trace : solution.traces) {
    Simulator sim = settled[trace.transition.from];
    apply(sim, vectors[trace.transition.to]);
    std::vector<std::vector<int>> timeline{
        outputs_of(sim, spec.outputs.size())};
    for (int t = 1; t <= kHorizon; ++t) {
      sim.tick();
      timeline.push_back(outputs_of(sim, spec.outputs.size()));
    }
    INFO("transition " << trace.transition.from << " -> "
                       << trace.transition.to);
    CHECK(timeline == trace.outputs);
  }
}

std::vector<TransitionCase> all_pairs(int count) {
  std::vector<TransitionCase> pairs;
  for (int a = 0; a < count; ++a)
    for (int b = 0; b < count; ++b)
      if (a != b)
        pairs.push_back({a, b});
  return pairs;
}

} // namespace

TEST_CASE("model matches simulator: torch inverter") {
  World w = with_glass_floor({4, 2, 1});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::solid());
  w.set({2, 1, 0}, Block::torch(Dir::West));
  w.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0}, {15}, {3}}, all_pairs(3));
}

TEST_CASE("model matches simulator: repeater chain and dust decay") {
  World w = with_glass_floor({6, 2, 1});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::dust());
  w.set({2, 1, 0}, Block::repeater(Dir::East, 3));
  w.set({3, 1, 0}, Block::repeater(Dir::East, 1));
  w.set({4, 1, 0}, Block::dust());
  w.set({5, 1, 0}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0}, {15}, {1}}, all_pairs(3));
}

TEST_CASE("model matches simulator: comparator subtract with container and "
          "side input") {
  World w = with_glass_floor({4, 2, 2});
  w.set({0, 1, 0}, Block::input(0, InputKind::Container));
  w.set({1, 1, 0}, Block::solid());
  w.set({2, 1, 0}, Block::comparator(Dir::East, ComparatorMode::Subtract));
  w.set({2, 1, 1}, Block::input(1, InputKind::Comparator, Dir::North));
  w.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0, 0}, {12, 0}, {12, 5}, {3, 7}},
                                all_pairs(4));
}

TEST_CASE("model matches simulator: observer pulse from a note block") {
  World w = with_glass_floor({2, 2, 3});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::note_block());
  w.set({1, 1, 1}, Block::observer(Dir::North));
  w.set({1, 1, 2}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0}, {15}}, all_pairs(2));
}

TEST_CASE("model matches simulator: observer watching a hopper, strong and "
          "weak power") {
  World w = with_glass_floor({4, 2, 3});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::dust());
  w.set({2, 1, 0}, Block::solid());
  w.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  w.set({2, 1, 1}, Block::hopper(4));
  w.set({2, 1, 2}, Block::observer(Dir::North));
  w.set({1, 1, 2}, Block::output(1, OutputKind::Repeater, Dir::West));
  check_model_matches_simulator(w, {{0}, {15}}, all_pairs(2));
}

TEST_CASE("model matches simulator: dust fading before the block it powers") {
  // No comparator is placed, so conductors are modelled on/off while the
  // dust keeps its level: input 2 dies out one dust short of the block.
  World w = with_glass_floor({7, 2, 1});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::dust());
  w.set({2, 1, 0}, Block::dust());
  w.set({3, 1, 0}, Block::dust());
  w.set({4, 1, 0}, Block::solid());
  w.set({5, 1, 0}, Block::torch(Dir::West));
  w.set({6, 1, 0}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0}, {15}, {2}, {3}}, all_pairs(4));
}

TEST_CASE("model matches simulator: dust stepping up glass and down solid") {
  World w = with_glass_floor({5, 3, 1});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::dust());
  w.set({2, 1, 0}, Block::glass());
  w.set({2, 2, 0}, Block::dust());
  w.set({3, 1, 0}, Block::solid());
  w.set({3, 2, 0}, Block::dust());
  w.set({4, 1, 0}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0}, {15}, {9}}, all_pairs(3));
}

TEST_CASE("model matches simulator: repeater lock with one input changing at a "
          "time") {
  World w = with_glass_floor({4, 2, 4});
  w.set({1, 1, 1}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({2, 1, 1}, Block::repeater(Dir::East, 1));
  w.set({3, 1, 1}, Block::output(0, OutputKind::Dust));
  w.set({2, 1, 3}, Block::input(1, InputKind::Comparator, Dir::North));
  w.set({2, 1, 2}, Block::repeater(Dir::North, 1));
  // [0 0] -> [1 0] -> [1 1]: the main repeater is on before the lock engages.
  // Locks are memory, which truth-table mode forbids, so allow feedback.
  check_model_matches_simulator(w, {{0, 0}, {15, 0}, {15, 15}},
                                {{0, 1}, {1, 0}, {1, 2}, {2, 1}}, true);
}

TEST_CASE("model matches simulator: torch AND gate") {
  World w = with_glass_floor({4, 3, 3});
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({0, 1, 2}, Block::input(1, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::solid());
  w.set({1, 1, 1}, Block::solid());
  w.set({1, 1, 2}, Block::solid());
  w.set({1, 2, 0}, Block::torch(Dir::Down));
  w.set({1, 2, 1}, Block::dust());
  w.set({1, 2, 2}, Block::torch(Dir::Down));
  w.set({2, 1, 1}, Block::torch(Dir::West));
  w.set({3, 1, 1}, Block::output(0, OutputKind::Dust));
  check_model_matches_simulator(w, {{0, 0}, {15, 0}, {15, 15}, {0, 15}},
                                all_pairs(4));
}

TEST_CASE("feedback loops are rejected unless allowed") {
  // Torch on block B powers the block above it, which powers dust on B: a loop.
  World w(Vec3{4, 3, 1});
  for (int x = 0; x < 4; ++x)
    w.set({x, 0, 0}, Block::glass());
  w.set({0, 1, 0}, Block::input(0, InputKind::Comparator, Dir::East));
  w.set({1, 1, 0}, Block::solid());
  w.set({2, 1, 0}, Block::torch(Dir::West));
  w.set({2, 2, 0}, Block::solid());
  w.set({1, 2, 0}, Block::dust());
  w.set({3, 1, 0}, Block::output(0, OutputKind::Dust));
  Spec spec = spec_for(w);
  ModelPlan plan{{{{0}, std::nullopt}}, {}};

  auto solve_status = [&](bool allow) {
    spec.allow_feedback = allow;
    CircuitEncoder encoder(
        spec, plan,
        EncoderOptions{.enforce_outputs = false, .fixed_layout = w});
    sat::SatParameters parameters;
    parameters.set_num_workers(kSolverThreads);
    return sat::SolveWithParameters(encoder.model().Build(), parameters)
        .status();
  };
  CHECK(solve_status(false) == sat::CpSolverStatus::INFEASIBLE);
  // With feedback allowed this loop is a clock and has no steady state either.
  CHECK(solve_status(true) == sat::CpSolverStatus::INFEASIBLE);
}

TEST_CASE("model matches simulator: dust and repeater inputs, comparator "
          "output") {
  World w = with_glass_floor({5, 2, 3});
  w.set({0, 1, 0}, Block::input(0, InputKind::Dust));
  w.set({1, 1, 0}, Block::dust());
  w.set({2, 1, 0}, Block::solid());
  w.set({3, 1, 0}, Block::output(0, OutputKind::Comparator, Dir::East));
  w.set({0, 1, 2}, Block::input(1, InputKind::Repeater, Dir::East));
  w.set({1, 1, 2}, Block::dust());
  w.set({2, 1, 2}, Block::output(1, OutputKind::Repeater, Dir::East));
  check_model_matches_simulator(w, {{0, 0}, {12, 0}, {12, 4}, {1, 9}},
                                all_pairs(4));
}

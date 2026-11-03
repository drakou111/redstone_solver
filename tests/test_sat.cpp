#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "redstone/cp_builder.hpp"
#include "redstone/encoder.hpp"
#include "redstone/sat_engine.hpp"
#include "redstone/solver.hpp"

// Last, so Catch2's CHECK replaces the one abseil (via OR-Tools) defines.
#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

constexpr auto kSolveTime = std::chrono::seconds(30);

std::chrono::steady_clock::time_point deadline() {
  return std::chrono::steady_clock::now() + kSolveTime;
}

// Builds a clause model, solves it with CaDiCaL and reads values back.
struct ClauseModel {
  operations_research::sat::CpModelBuilder model;
  cp::Builder b{model, false};

  std::optional<std::vector<bool>> solve() {
    SatPortfolio portfolio(1);
    portfolio.add(this->b.clauses(), 0);
    if (portfolio.solve(deadline(), nullptr) !=
        SatPortfolio::Outcome::Satisfiable)
      return std::nullopt;
    return portfolio.model(this->b.variable_count());
  }
};

bool value(const std::vector<bool> &values, const cp::Lit &lit) {
  if (lit.is_constant())
    return lit.is_true();
  int index = lit.var().index();
  return index >= 0 ? values[index] : !values[-index - 1];
}

int value(const std::vector<bool> &values, const cp::Int &x) {
  int result = x.lo();
  for (const cp::Lit &level : x.levels())
    result += value(values, level) ? 1 : 0;
  return result;
}

constexpr int kMax = 4;

// Fixes two inputs to every pair of values and checks the operation's result.
void check_binary(const std::function<cp::Int(cp::Builder &, const cp::Int &,
                                              const cp::Int &)> &operation,
                  const std::function<int(int, int)> &expected) {
  for (int a = 0; a <= kMax; ++a)
    for (int c = 0; c <= kMax; ++c) {
      ClauseModel m;
      cp::Int x = m.b.new_int(0, kMax);
      cp::Int y = m.b.new_int(0, kMax);
      cp::Int r = operation(m.b, x, y);
      m.b.require_in_range(x, a, a);
      m.b.require_in_range(y, c, c);
      auto values = m.solve();
      REQUIRE(values);
      INFO("a=" << a << " b=" << c);
      CHECK(value(*values, r) == expected(a, c));
    }
}

} // namespace

TEST_CASE("SAT portfolio answers satisfiable and unsatisfiable formulas") {
  SatPortfolio portfolio(2);
  portfolio.add({{1, 2}, {-1, 2}, {1, -2}}, 0);
  REQUIRE(portfolio.solve(deadline(), nullptr) ==
          SatPortfolio::Outcome::Satisfiable);
  CHECK(portfolio.model(2) == std::vector<bool>{true, true});

  // Clauses added later keep the earlier ones.
  std::vector<std::vector<int>> clauses{{1, 2}, {-1, 2}, {1, -2}, {-1, -2}};
  portfolio.add(clauses, 3);
  CHECK(portfolio.solve(deadline(), nullptr) ==
        SatPortfolio::Outcome::Unsatisfiable);
}

TEST_CASE("SAT portfolio stops when cancelled") {
  // Pigeonhole 10 into 9: hard enough that it is still running when asked.
  constexpr int kHoles = 9;
  constexpr int kPigeons = kHoles + 1;
  auto var = [](int pigeon, int hole) { return pigeon * kHoles + hole + 1; };
  std::vector<std::vector<int>> clauses;
  for (int p = 0; p < kPigeons; ++p) {
    std::vector<int> some_hole;
    for (int h = 0; h < kHoles; ++h)
      some_hole.push_back(var(p, h));
    clauses.push_back(some_hole);
  }
  for (int h = 0; h < kHoles; ++h)
    for (int p = 0; p < kPigeons; ++p)
      for (int q = p + 1; q < kPigeons; ++q)
        clauses.push_back({-var(p, h), -var(q, h)});

  SatPortfolio portfolio(1);
  portfolio.add(clauses, 0);
  std::atomic<bool> cancel = true;
  CHECK(portfolio.solve(deadline(), &cancel) == SatPortfolio::Outcome::Stopped);
}

TEST_CASE("clause integers compute max, select, if-else and difference") {
  check_binary([](cp::Builder &b, const cp::Int &x,
                  const cp::Int &y) { return b.max_of({x, y}); },
               [](int a, int c) { return std::max(a, c); });
  check_binary(
      [](cp::Builder &b, const cp::Int &x, const cp::Int &y) {
        return b.if_else(b.greater(x, y), x.plus(-1), y);
      },
      [](int a, int c) { return a > c ? a - 1 : c; });
  check_binary([](cp::Builder &b, const cp::Int &x,
                  const cp::Int &y) { return b.select(b.not_equal(x, y), x); },
               [](int a, int c) { return a != c ? a : 0; });
  check_binary(
      [](cp::Builder &b, const cp::Int &x, const cp::Int &y) {
        cp::Int r = b.new_int(0, kMax);
        cp::Lit ordered = !b.greater(y, x);
        b.require_difference(r, x, y, {ordered});
        b.require_in_range(r, 0, 0, {!ordered});
        return r;
      },
      [](int a, int c) { return a >= c ? a - c : 0; });
}

TEST_CASE("chosen max pins its result to exactly the largest term") {
  // Every result value is tried, so a loose encoding would show up as a
  // second satisfiable value.
  for (int a = 0; a <= kMax; ++a)
    for (int c = 0; c <= kMax; ++c) {
      int conditional = a > c ? a - 1 : 0;
      int on_off = a <= c && a >= 3 ? kMax : 0;
      int expected = std::max({conditional, c - 1, on_off, 0});
      for (int v = 0; v <= kMax; ++v) {
        ClauseModel m;
        cp::Int x = m.b.new_int(0, kMax);
        cp::Int y = m.b.new_int(0, kMax);
        cp::Lit x_greater = m.b.greater(x, y);
        cp::Int r = m.b.new_int(0, kMax);
        m.b.require_equal_max_chosen(r,
                                     {{x_greater, x.plus(-1)},
                                      {m.b.constant(true), y.plus(-1)},
                                      {!x_greater, m.b.scaled(x.ge(3), kMax)}});
        m.b.require_in_range(x, a, a);
        m.b.require_in_range(y, c, c);
        m.b.require_in_range(r, v, v);
        INFO("a=" << a << " b=" << c << " result=" << v);
        CHECK(m.solve().has_value() == (v == expected));
      }
    }
}

TEST_CASE("block count bound limits how many literals hold") {
  constexpr int kLiterals = 7;
  for (int limit = 0; limit <= kLiterals; ++limit) {
    ClauseModel m;
    std::vector<cp::Lit> lits;
    for (int i = 0; i < kLiterals; ++i)
      lits.push_back(m.b.new_bool());
    cp::Int count = m.b.count_for_upper_bound(lits);
    m.b.require(!count.ge(limit + 1));
    // Ask for one more than allowed: must be unsatisfiable.
    ClauseModel over;
    std::vector<cp::Lit> over_lits;
    for (int i = 0; i < kLiterals; ++i)
      over_lits.push_back(over.b.new_bool());
    cp::Int over_count = over.b.count_for_upper_bound(over_lits);
    over.b.require(!over_count.ge(limit + 1));
    for (int i = 0; i <= limit && i < kLiterals; ++i)
      over.b.require(over_lits[i]);

    auto values = m.solve();
    REQUIRE(values);
    int holding = 0;
    for (const cp::Lit &lit : lits)
      holding += value(*values, lit) ? 1 : 0;
    CHECK(holding <= limit);
    CHECK(over.solve().has_value() == (limit == kLiterals));
  }
}

TEST_CASE("both solver engines find the smallest NOT gate") {
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
palette solid glass dust torch repeater comparator
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  for (SolverEngine engine : {SolverEngine::Sat, SolverEngine::CpSat}) {
    SolverOptions options;
    options.engine = engine;
    options.time_limit_seconds = 60.0;
    SolveReport report = solve(spec, options);
    REQUIRE(report.status == SolveStatus::Optimal);
    CHECK(report.solution->block_count == 4);
  }
}

TEST_CASE("both solver engines place free ports inside their regions") {
  Spec spec = parse_spec(R"(
size 4 2 3
input IN1 repeater free
output OUT1 dust free
region IN1 0 1 0..2
region OUT1 3 1 0..2
palette solid glass dust torch repeater comparator
row IN1(ss15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  for (SolverEngine engine : {SolverEngine::Sat, SolverEngine::CpSat}) {
    SolverOptions options;
    options.engine = engine;
    options.time_limit_seconds = 60.0;
    SolveReport report = solve(spec, options);
    REQUIRE(report.status == SolveStatus::Optimal);
    // Torch and its block plus supports under the input and output.
    CHECK(report.solution->block_count == 4);

    const World &layout = report.solution->layout;
    int inputs = 0;
    int outputs = 0;
    for (int i = 0; i < layout.volume(); ++i) {
      Vec3 p = layout.position_of(i);
      const Block &block = layout.at(p);
      if (block.type == BlockType::Input) {
        ++inputs;
        CHECK(p.x == 0);
        CHECK(p.y == 1);
      }
      if (block.type == BlockType::Output) {
        ++outputs;
        CHECK(p.x == 3);
      }
    }
    CHECK(inputs == 1);
    CHECK(outputs == 1);
    CHECK_FALSE(verify_layout(layout, spec, spec.verification_vectors()));
  }
}

TEST_CASE("SAT engine adds failed cases to the running model") {
  // Rejections here add steady states and transitions to the live model,
  // sometimes a steady state without a transition.
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 comparator 0 1 0 east
output OUT1 dust 3 1 0
timing 4 4
hold_previous on
row IN1(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
)");
  for (int seed = 0; seed < 3; ++seed) {
    SolverOptions options;
    options.sat_seed = seed;
    options.time_limit_seconds = 60.0;
    SolveReport report = solve(spec, options);
    REQUIRE(report.status == SolveStatus::Optimal);
    CHECK(report.solution->block_count == 5);
  }
}

TEST_CASE("model size profile accounts for every clause and variable") {
  Spec spec = parse_spec(R"(
size 4 2 1
input IN1 strong 0 1 0 east
output OUT1 dust 3 1 0
horizon 8
row IN1(ss1..15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)");
  ModelPlan plan;
  for (const InputVector &inputs : spec.representative_vectors())
    plan.steady.push_back({inputs, spec.expected_outputs(inputs)});
  plan.transitions.push_back({0, 1});
  CircuitEncoder encoder(spec, plan, {.build_cp_model = false});
  encoder.limit_blocks(3);

  long long variables = 0;
  long long clauses = 0;
  std::vector<std::string> names;
  for (const ModelPart &part : encoder.size_profile()) {
    variables += part.variables;
    clauses += part.clauses;
    names.push_back(part.name);
  }
  CHECK(variables == encoder.variable_count());
  CHECK(clauses == static_cast<long long>(encoder.clauses().size()));
  for (const char *expected :
       {"steady: dust", "ticks: comparators", "block limit"})
    CHECK(std::find(names.begin(), names.end(), expected) != names.end());
}

TEST_CASE("the basic-block search does not hide circuits that need more") {
  // Only a comparator can tell strength 8 from 7, so the basic-block search
  // finds nothing and the main search must still prove the optimum.
  Spec spec = parse_spec(R"(
size 5 2 3
input IN1 comparator 0 1 1 east
output OUT1 dust 4 1 1
horizon 10
row IN1(ss8..15) -> OUT1(ss1..15)
row IN1(ss0..7) -> OUT1(ss0)
)");
  std::optional<int> blocks[2];
  for (bool quick : {true, false}) {
    SolverOptions options;
    options.quick_palette_search = quick;
    options.time_limit_seconds = 60.0;
    SolveReport report = solve(spec, options);
    REQUIRE(report.status == SolveStatus::Optimal);
    blocks[quick ? 1 : 0] = report.solution->block_count;
  }
  CHECK(blocks[0] == blocks[1]);
}

TEST_CASE("first-circuit mode takes the basic-block search's circuit") {
  Spec spec = parse_spec(R"(
size 5 3 5
input IN1 comparator 0 1 0 east
input IN2 comparator 0 1 4 east
output OUT1 dust 4 1 2
horizon 12
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
)");
  SolverOptions options;
  options.stop_at_first = true;
  options.time_limit_seconds = 60.0;
  std::vector<std::string> messages;
  SolveReport report = solve(spec, options, [&](std::string_view message) {
    messages.emplace_back(message);
  });
  REQUIRE(report.status == SolveStatus::Feasible);
  REQUIRE(report.solution);
  CHECK_FALSE(verify_layout(report.solution->layout, spec,
                            spec.verification_vectors()));
  CHECK(std::any_of(messages.begin(), messages.end(), [](const auto &m) {
    return m.starts_with("[basic blocks]");
  }));
}

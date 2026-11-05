#include "redstone/netlist.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <stdexcept>

#include "redstone/sat_engine.hpp"

namespace rs {

namespace {

constexpr int kMaxInputs = 6;
constexpr SignalRange kAnySignal{0, kMaxSignal};

bool off(SignalRange r) { return r.max == 0; }
bool on(SignalRange r) { return r.min >= 1; }
// A gate's output strength is whatever it is: only "any of 1..15" is on.
bool output_on(SignalRange r) { return r == SignalRange{1, kMaxSignal}; }
bool any(SignalRange r) { return r == kAnySignal; }

// Clauses over DIMACS literals, with fresh variables on demand.
class Cnf {
public:
  int fresh() { return ++this->variables_; }
  int variables() const { return this->variables_; }
  void add(std::vector<int> clause) {
    this->clauses_.push_back(std::move(clause));
  }
  void exactly_one(const std::vector<int> &lits) {
    this->add(lits);
    for (std::size_t i = 0; i < lits.size(); ++i)
      for (std::size_t j = i + 1; j < lits.size(); ++j)
        this->add({-lits[i], -lits[j]});
  }
  const std::vector<std::vector<int>> &clauses() const {
    return this->clauses_;
  }

private:
  int variables_ = 0;
  std::vector<std::vector<int>> clauses_;
};

// A node's value for one vector: a constant (input) or a variable (gate).
struct Value {
  int literal = 0; // 0: constant
  bool constant = false;
};

// Nothing if no network of `gates` gates (at most `max_depth` levels deep,
// 0: any depth) exists; sets `stopped` (and returns nothing) when the
// deadline or cancel ended the search first.
std::optional<Netlist>
synthesize_with(const BoolTable &table, int gates, int max_depth,
                const SynthesisOptions &options,
                std::chrono::steady_clock::time_point deadline, bool &stopped) {
  const int n = table.inputs;
  const int vectors = 1 << n;
  const int nodes = n + gates;
  Cnf cnf;

  // value[node][t]
  std::vector<std::vector<Value>> value(nodes, std::vector<Value>(vectors));
  for (int i = 0; i < n; ++i)
    for (int t = 0; t < vectors; ++t)
      value[i][t] = {0, ((t >> i) & 1) != 0};
  for (int g = 0; g < gates; ++g)
    for (int t = 0; t < vectors; ++t)
      value[n + g][t] = {cnf.fresh(), false};

  // Literal "node has value v at t", or 0 for true / nullopt for false.
  auto is = [&](int node, int t, bool v) -> std::optional<int> {
    const Value &x = value[node][t];
    if (x.literal == 0)
      return x.constant == v ? std::optional<int>(0) : std::nullopt;
    return v ? x.literal : -x.literal;
  };

  struct Pair {
    int a, b, selected;
  };
  std::vector<std::vector<Pair>> pairs(gates);
  std::vector<std::array<int, 4>> function(gates);
  for (int g = 0; g < gates; ++g) {
    int node = n + g;
    std::vector<int> choices;
    for (int a = 0; a < node; ++a)
      for (int b = a + 1; b < node; ++b) {
        pairs[g].push_back({a, b, cnf.fresh()});
        choices.push_back(pairs[g].back().selected);
      }
    cnf.exactly_one(choices);
    for (int &bit : function[g])
      bit = cnf.fresh();
    // Only allowed functions.
    for (int f = 0; f < 16; ++f) {
      if (std::find(options.functions.begin(), options.functions.end(),
                    static_cast<std::uint8_t>(f)) != options.functions.end())
        continue;
      std::vector<int> not_f;
      for (int bit = 0; bit < 4; ++bit)
        not_f.push_back(((f >> bit) & 1) != 0 ? -function[g][bit]
                                              : function[g][bit]);
      cnf.add(not_f);
    }
    // Simulation: selected operands with values (va, vb) give f(va, vb).
    for (const Pair &p : pairs[g])
      for (int t = 0; t < vectors; ++t)
        for (int va = 0; va < 2; ++va)
          for (int vb = 0; vb < 2; ++vb) {
            std::optional<int> a = is(p.a, t, va != 0);
            std::optional<int> b = is(p.b, t, vb != 0);
            if (!a || !b)
              continue; // those operand values cannot happen together
            std::vector<int> premise{-p.selected};
            if (*a != 0)
              premise.push_back(-*a);
            if (*b != 0)
              premise.push_back(-*b);
            int f = function[g][(va << 1) | vb];
            int x = value[node][t].literal;
            std::vector<int> c1 = premise, c2 = premise;
            c1.insert(c1.end(), {-f, x});
            c2.insert(c2.end(), {f, -x});
            cnf.add(c1);
            cnf.add(c2);
          }
  }

  // Depth bound: shallow[g][d] means gate g is at most d levels deep; a
  // gate's selected operands are one level shallower (inputs are level 0).
  if (max_depth > 0) {
    std::vector<std::vector<int>> shallow(gates,
                                          std::vector<int>(max_depth + 1));
    for (int g = 0; g < gates; ++g) {
      for (int d = 1; d <= max_depth; ++d)
        shallow[g][d] = cnf.fresh();
      cnf.add({shallow[g][max_depth]});
      for (int d = 1; d < max_depth; ++d)
        cnf.add({-shallow[g][d], shallow[g][d + 1]});
      for (const Pair &p : pairs[g])
        for (int d = 1; d <= max_depth; ++d)
          for (int operand : {p.a, p.b}) {
            if (operand < n)
              continue; // an input is level 0
            int og = operand - n;
            if (d == 1)
              cnf.add({-p.selected, -shallow[g][d]});
            else
              cnf.add({-p.selected, -shallow[g][d], shallow[og][d - 1]});
          }
    }
  }

  // Each output reads one node that matches it wherever it is cared about.
  std::vector<std::vector<int>> reads(table.values.size());
  for (std::size_t o = 0; o < table.values.size(); ++o) {
    for (int node = 0; node < nodes; ++node) {
      int r = cnf.fresh();
      reads[o].push_back(r);
      for (int t = 0; t < vectors; ++t) {
        std::optional<bool> want = table.values[o][t];
        if (!want)
          continue;
        std::optional<int> match = is(node, t, *want);
        if (!match)
          cnf.add({-r});
        else if (*match != 0)
          cnf.add({-r, *match});
      }
    }
    cnf.exactly_one(reads[o]);
  }
  // Every gate is used, by a later gate or an output.
  for (int g = 0; g < gates; ++g) {
    int node = n + g;
    std::vector<int> users;
    for (int later = g + 1; later < gates; ++later)
      for (const Pair &p : pairs[later])
        if (p.a == node || p.b == node)
          users.push_back(p.selected);
    for (const auto &r : reads)
      users.push_back(r[node]);
    cnf.add(users);
  }

  SatPortfolio sat(1);
  sat.add(cnf.clauses(), 0);
  SatPortfolio::Outcome outcome = sat.solve(deadline, options.cancel);
  if (outcome == SatPortfolio::Outcome::Unsatisfiable)
    return std::nullopt;
  if (outcome == SatPortfolio::Outcome::Stopped) {
    stopped = true;
    return std::nullopt;
  }
  std::vector<bool> model = sat.model(cnf.variables());
  auto truth = [&](int literal) { return model[literal - 1]; };

  Netlist netlist;
  netlist.inputs = n;
  for (int g = 0; g < gates; ++g) {
    Gate gate;
    for (const Pair &p : pairs[g])
      if (truth(p.selected)) {
        gate.a = p.a;
        gate.b = p.b;
      }
    for (int bit = 0; bit < 4; ++bit)
      if (truth(function[g][bit]))
        gate.function |= static_cast<std::uint8_t>(1 << bit);
    netlist.gates.push_back(gate);
  }
  for (const auto &r : reads)
    for (int node = 0; node < nodes; ++node)
      if (truth(r[node]))
        netlist.outputs.push_back(node);
  return netlist;
}

} // namespace

BoolTable boolean_table(const Spec &spec) {
  const int n = static_cast<int>(spec.inputs.size());
  if (n < 1 || n > kMaxInputs)
    throw std::invalid_argument(
        std::format("compose handles 1 to {} inputs", kMaxInputs));
  for (const TruthRow &row : spec.rows)
    for (std::size_t i = 0; i < row.inputs.size(); ++i)
      if (!off(row.inputs[i]) && !on(row.inputs[i]) && !any(row.inputs[i]))
        throw std::invalid_argument(std::format(
            "IN{} is {} in a row: compose only knows on (1..15) and off (0)",
            i + 1, to_string(row.inputs[i])));
  BoolTable table;
  table.inputs = n;
  table.values.assign(spec.outputs.size(),
                      std::vector<std::optional<bool>>(1u << n));
  for (int t = 0; t < (1 << n); ++t) {
    InputVector vector;
    for (int i = 0; i < n; ++i)
      vector.push_back(((t >> i) & 1) != 0 ? kMaxSignal : 0);
    std::optional<std::vector<SignalRange>> wanted =
        spec.expected_outputs(vector);
    if (!wanted)
      continue;
    for (std::size_t o = 0; o < wanted->size(); ++o) {
      SignalRange r = (*wanted)[o];
      if (any(r))
        continue;
      if (!off(r) && !output_on(r))
        throw std::invalid_argument(std::format(
            "OUT{} must be {} for some inputs: compose only makes on (1..15) "
            "and off (0) outputs",
            o + 1, to_string(r)));
      table.values[o][t] = output_on(r);
    }
  }
  return table;
}

std::string function_name(std::uint8_t f) {
  switch (f) {
  case 0b1000:
    return "AND";
  case 0b1110:
    return "OR";
  case 0b0110:
    return "XOR";
  case 0b0111:
    return "NAND";
  case 0b0001:
    return "NOR";
  case 0b1001:
    return "XNOR";
  case 0b0100:
    return "A AND NOT B";
  case 0b0010:
    return "B AND NOT A";
  case 0b1101:
    return "A OR NOT B";
  case 0b1011:
    return "B OR NOT A";
  default:
    return std::format("f{:04b}", f);
  }
}

std::vector<std::uint8_t> two_input_functions() {
  return {0b1000, 0b1110, 0b0110, 0b0111, 0b0001,
          0b1001, 0b0100, 0b0010, 0b1101, 0b1011};
}

std::vector<bool> Netlist::evaluate(int vector) const {
  std::vector<bool> v;
  for (int i = 0; i < this->inputs; ++i)
    v.push_back(((vector >> i) & 1) != 0);
  for (const Gate &g : this->gates) {
    int index = (static_cast<int>(v[g.a]) << 1) | static_cast<int>(v[g.b]);
    v.push_back(((g.function >> index) & 1) != 0);
  }
  return v;
}

std::vector<int> Netlist::levels() const {
  std::vector<int> level(this->inputs, 0);
  for (const Gate &g : this->gates)
    level.push_back(1 + std::max(level[g.a], level[g.b]));
  return level;
}

std::string Netlist::describe() const {
  auto name = [&](int node) {
    return node < this->inputs ? std::format("IN{}", node + 1)
                               : std::format("g{}", node - this->inputs + 1);
  };
  std::string text;
  for (std::size_t g = 0; g < this->gates.size(); ++g)
    text += std::format("g{} = {}({}, {})\n", g + 1,
                        function_name(this->gates[g].function),
                        name(this->gates[g].a), name(this->gates[g].b));
  for (std::size_t o = 0; o < this->outputs.size(); ++o)
    text += std::format("OUT{} = {}\n", o + 1, name(this->outputs[o]));
  return text;
}

std::optional<Netlist> synthesize(const BoolTable &table,
                                  const SynthesisOptions &options) {
  auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  for (int gates = 0; gates <= options.max_gates; ++gates) {
    if (std::chrono::steady_clock::now() >= deadline ||
        (options.cancel && options.cancel->load()))
      return std::nullopt;
    bool stopped = false;
    std::optional<Netlist> netlist =
        synthesize_with(table, gates, 0, options, deadline, stopped);
    if (stopped)
      return std::nullopt;
    if (!netlist)
      continue;
    // Same gate count with only the preferred functions, if possible: their
    // modules are smaller and the networks are the familiar ones.
    SynthesisOptions chosen = options;
    chosen.functions.clear();
    for (std::uint8_t f : options.preferred)
      if (std::find(options.functions.begin(), options.functions.end(), f) !=
          options.functions.end())
        chosen.functions.push_back(f);
    if (chosen.functions.empty() ||
        chosen.functions.size() >= options.functions.size() ||
        !synthesize_with(table, gates, 0, chosen, deadline, stopped))
      chosen.functions = options.functions;
    // Then as shallow as possible: fewer levels, fewer channels to wire.
    for (int depth = 1; depth <= gates; ++depth) {
      if (auto shallow =
              synthesize_with(table, gates, depth, chosen, deadline, stopped))
        return shallow;
      if (stopped)
        break;
    }
    return netlist;
  }
  return std::nullopt;
}

} // namespace rs

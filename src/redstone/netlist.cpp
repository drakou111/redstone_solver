#include "redstone/netlist.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <format>
#include <map>
#include <set>
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

bool exactly_one_of_three(const BoolTable &table) {
  if (table.inputs != 3 || table.values.size() != 1 ||
      table.values[0].size() != 8)
    return false;
  for (int vector = 0; vector < 8; ++vector) {
    if (!table.values[0][vector] ||
        *table.values[0][vector] !=
            (std::popcount(static_cast<unsigned>(vector)) == 1))
      return false;
  }
  return true;
}

Netlist exactly_one_of_three_netlist() {
  // X = a XOR b; Y = b AND c; exactly_one = c XOR (X OR Y).
  // For c=0 this is a XOR b; for c=1 it is NOT(a OR b).
  return {3,
          {{0, 1, 0b0110},  // a XOR b
           {1, 2, 0b1000},  // b AND c
           {3, 4, 0b1110},  // X OR Y
           {2, 5, 0b0110}}, // c XOR (X OR Y)
          {6}};
}

std::optional<BoolTable> remove_input(const BoolTable &table, int input) {
  BoolTable projected;
  projected.inputs = table.inputs - 1;
  const int mask = 1 << input;
  projected.values.resize(table.values.size(), std::vector<std::optional<bool>>(
                                                   1 << projected.inputs));
  for (int vector = 0; vector < (1 << projected.inputs); ++vector) {
    const int lower = vector & (mask - 1);
    const int upper = vector & ~(mask - 1);
    const int with_input_off = lower | (upper << 1);
    const int with_input_on = with_input_off | mask;
    for (std::size_t output = 0; output < table.values.size(); ++output) {
      const auto &off = table.values[output][with_input_off];
      const auto &on = table.values[output][with_input_on];
      if (off && on && off != on)
        return std::nullopt;
      projected.values[output][vector] = off ? off : on;
    }
  }
  return projected;
}

struct SupportReduction {
  BoolTable table;
  std::vector<int> inputs;
};

SupportReduction remove_irrelevant_inputs(const BoolTable &table) {
  SupportReduction result{table, {}};
  for (int input = 0; input < table.inputs; ++input)
    result.inputs.push_back(input);

  // Eliminate a dimension only when the projected care sets are consistent.
  // This handles don't-cares safely even when several dimensions are
  // individually removable but their combination would create a conflict.
  while (result.inputs.size() > 1) {
    bool removed = false;
    for (std::size_t i = 0; i < result.inputs.size(); ++i) {
      if (auto projected = remove_input(result.table, static_cast<int>(i))) {
        result.table = std::move(*projected);
        result.inputs.erase(result.inputs.begin() + i);
        removed = true;
        break;
      }
    }
    if (!removed)
      break;
  }
  return result;
}

Netlist restore_input_nodes(Netlist netlist, int input_count,
                            const std::vector<int> &inputs) {
  auto restore = [&](int node) {
    return node < netlist.inputs ? inputs[node]
                                 : input_count + (node - netlist.inputs);
  };
  for (Gate &gate : netlist.gates) {
    gate.a = restore(gate.a);
    gate.b = restore(gate.b);
  }
  for (int &output : netlist.outputs)
    output = restore(output);
  netlist.inputs = input_count;
  return netlist;
}

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
      // Tied operands are needed for unary functions and constants: e.g.
      // NAND(x, x) is NOT x, and XOR(x, x) is zero.
      for (int b = a; b < node; ++b) {
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

  SatPortfolio sat(1, options.sat_seed);
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

Netlist parse_netlist(std::string_view text, int inputs) {
  Netlist netlist;
  netlist.inputs = inputs;
  auto trim = [](std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
      return std::string_view{};
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
  };
  auto number_after = [](std::string_view s, std::string_view prefix,
                         int &value) {
    if (!s.starts_with(prefix) || s.size() == prefix.size())
      return false;
    value = 0;
    for (char ch : s.substr(prefix.size())) {
      if (ch < '0' || ch > '9')
        return false;
      value = value * 10 + (ch - '0');
    }
    return true;
  };
  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    std::size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos)
      line_end = text.size();
    const std::string_view line =
        trim(text.substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (line.empty() || line.starts_with('#'))
      continue;
    auto fail = [&](std::string_view why) {
      throw std::invalid_argument(std::format(
          "network line \"{}\": {}", std::string(line), std::string(why)));
    };
    // A node already defined: an input or an earlier gate.
    auto node = [&](std::string_view name) {
      name = trim(name);
      int k = 0;
      if (number_after(name, "IN", k) && k >= 1 && k <= inputs)
        return k - 1;
      if (number_after(name, "g", k) && k >= 1 &&
          k <= static_cast<int>(netlist.gates.size()))
        return inputs + k - 1;
      fail(std::format("\"{}\" is not an input or an earlier gate",
                       std::string(name)));
      return 0;
    };
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos)
      fail("expected \"name = ...\"");
    const std::string_view lhs = trim(line.substr(0, equals));
    const std::string_view rhs = trim(line.substr(equals + 1));
    int k = 0;
    if (number_after(lhs, "OUT", k)) {
      if (k != static_cast<int>(netlist.outputs.size()) + 1)
        fail("outputs must come in order OUT1, OUT2, ...");
      netlist.outputs.push_back(node(rhs));
      continue;
    }
    if (!number_after(lhs, "g", k))
      fail("expected a gate gN or an output OUTN");
    if (k != static_cast<int>(netlist.gates.size()) + 1)
      fail("gates must come in order g1, g2, ...");
    const std::size_t open = rhs.find('(');
    const std::size_t comma = rhs.find(',');
    if (open == std::string_view::npos || comma == std::string_view::npos ||
        comma < open || !rhs.ends_with(')'))
      fail("expected FUNCTION(a, b)");
    const std::string_view name = trim(rhs.substr(0, open));
    std::optional<std::uint8_t> function;
    for (int f = 0; f < 16; ++f)
      if (function_name(static_cast<std::uint8_t>(f)) == name)
        function = static_cast<std::uint8_t>(f);
    if (!function)
      fail(std::format("unknown function \"{}\"", std::string(name)));
    Gate gate;
    gate.a = node(rhs.substr(open + 1, comma - open - 1));
    gate.b = node(rhs.substr(comma + 1, rhs.size() - comma - 2));
    gate.function = *function;
    netlist.gates.push_back(gate);
  }
  if (netlist.outputs.empty())
    throw std::invalid_argument("network has no outputs (OUT1 = ...)");
  return netlist;
}

std::optional<Netlist> synthesize(const BoolTable &table,
                                  const SynthesisOptions &options) {
  auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  // Inputs that never change a cared-about output only inflate the SAT
  // network's possible gate connections. Project them away, synthesize the
  // smaller truth table, then restore the original input node numbering.
  SupportReduction support = remove_irrelevant_inputs(table);
  if (support.inputs.size() < static_cast<std::size_t>(table.inputs)) {
    SynthesisOptions reduced_options = options;
    reduced_options.time_limit_seconds =
        std::chrono::duration<double>(deadline -
                                      std::chrono::steady_clock::now())
            .count();
    if (reduced_options.time_limit_seconds <= 0)
      return std::nullopt;
    std::optional<Netlist> reduced = synthesize(support.table, reduced_options);
    if (!reduced)
      return std::nullopt;
    return restore_input_nodes(std::move(*reduced), table.inputs,
                               support.inputs);
  }

  const bool exact_one = exactly_one_of_three(table);
  constexpr std::array<std::uint8_t, 3> exact_one_functions{
      0b0110, 0b1000, 0b1110}; // XOR, AND, OR
  const bool can_build_exact_one = std::all_of(
      exact_one_functions.begin(), exact_one_functions.end(),
      [&](std::uint8_t function) {
        return std::find(options.functions.begin(), options.functions.end(),
                         function) != options.functions.end();
      });
  for (int gates = 0; gates <= options.max_gates; ++gates) {
    if (std::chrono::steady_clock::now() >= deadline ||
        (options.cancel && options.cancel->load()))
      return std::nullopt;
    if (gates == 4 && exact_one && can_build_exact_one) {
      // The lower gate counts have already been ruled out by the exact
      // search. A four-gate depth-two circuit would have an unused gate,
      // since the output's single 2-input gate can consume only two of the
      // three possible first-layer gates. Return the known depth-three
      // network without building another SAT model to rediscover it.
      return exactly_one_of_three_netlist();
    }
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

std::vector<Netlist> decoded_networks(const BoolTable &table,
                                      const SynthesisOptions &options) {
  constexpr int kMaxDecodedInputs = 3;
  constexpr int kMinOutputs = 3;
  constexpr std::size_t kCodes = 4; // what two signals can tell apart
  const int outputs = static_cast<int>(table.values.size());
  if (table.inputs > kMaxDecodedInputs || outputs < kMinOutputs)
    return {};
  const int vectors = 1 << table.inputs;
  // Each input vector's class: the combination of outputs it gives.
  std::vector<std::vector<bool>> classes;
  std::vector<int> class_of(vectors);
  for (int t = 0; t < vectors; ++t) {
    std::vector<bool> combination;
    for (const auto &column : table.values) {
      if (!column[t])
        return {};
      combination.push_back(*column[t]);
    }
    auto it = std::find(classes.begin(), classes.end(), combination);
    class_of[t] = static_cast<int>(it - classes.begin());
    if (it == classes.end())
      classes.push_back(std::move(combination));
  }
  if (classes.size() > kCodes || classes.size() < 3)
    return {};

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  std::vector<Netlist> found;
  std::set<std::string> seen;
  std::vector<int> codes{0, 1, 2, 3}; // the first classes.size() are used
  do {
    if (std::chrono::steady_clock::now() >= deadline ||
        (options.cancel && options.cancel->load()))
      break;
    // Two signals: the high and low bit of each vector's class code.
    BoolTable bits;
    bits.inputs = table.inputs;
    bits.values.assign(2, {});
    for (int t = 0; t < vectors; ++t) {
      const int code = codes[class_of[t]];
      bits.values[0].push_back((code & 2) != 0);
      bits.values[1].push_back((code & 1) != 0);
    }
    SynthesisOptions o = options;
    o.time_limit_seconds = std::chrono::duration<double>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (o.time_limit_seconds <= 0)
      break;
    std::optional<Netlist> signals = synthesize(bits, o);
    if (!signals)
      continue;
    Netlist network = *signals;
    network.outputs.clear();
    const int high = signals->outputs[0];
    const int low = signals->outputs[1];
    for (int k = 0; k < outputs; ++k) {
      std::uint8_t function = 0;
      for (std::size_t c = 0; c < classes.size(); ++c)
        if (classes[c][k])
          function |= static_cast<std::uint8_t>(1 << codes[c]);
      network.outputs.push_back(network.inputs +
                                static_cast<int>(network.gates.size()));
      network.gates.push_back({high, low, function});
    }
    if (seen.insert(network.describe()).second)
      found.push_back(std::move(network));
  } while (std::next_permutation(codes.begin(), codes.end()));
  return found;
}

std::vector<Netlist> synthesize_alternatives(const BoolTable &table,
                                             const Netlist &initial,
                                             std::size_t max_candidates,
                                             const SynthesisOptions &options) {
  std::vector<Netlist> candidates{initial};
  if (max_candidates <= 1 || initial.gates.empty())
    return candidates;

  const int gates = static_cast<int>(initial.gates.size());
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  std::set<std::string> seen{initial.describe()};
  const int max_attempts = static_cast<int>(max_candidates * 4);
  for (int attempt = 1;
       candidates.size() < max_candidates && attempt <= max_attempts;
       ++attempt) {
    if (std::chrono::steady_clock::now() >= deadline ||
        (options.cancel && options.cancel->load()))
      break;

    SynthesisOptions variant = options;
    variant.sat_seed += attempt;
    std::vector<std::uint8_t> preferred;
    for (std::uint8_t function : options.preferred)
      if (std::find(options.functions.begin(), options.functions.end(),
                    function) != options.functions.end())
        preferred.push_back(function);
    variant.functions =
        preferred.empty() || preferred.size() >= options.functions.size()
            ? options.functions
            : std::move(preferred);

    bool stopped = false;
    std::optional<Netlist> candidate =
        synthesize_with(table, gates, 0, variant, deadline, stopped);
    if (!candidate && !stopped && variant.functions != options.functions) {
      variant.functions = options.functions;
      candidate = synthesize_with(table, gates, 0, variant, deadline, stopped);
    }
    if (!candidate)
      continue;

    Netlist best = std::move(*candidate);
    for (int depth = 1; depth <= gates; ++depth) {
      if (auto shallow = synthesize_with(table, gates, depth, variant, deadline,
                                         stopped)) {
        best = std::move(*shallow);
        break;
      }
      if (stopped)
        break;
    }
    std::string description = best.describe();
    if (seen.insert(std::move(description)).second)
      candidates.push_back(std::move(best));
  }
  return candidates;
}

namespace {

// A function of the table's inputs: per input vector 1, 0 or -1 (any).
using Truth = std::vector<std::int8_t>;

// What a sub-function became: a node, a constant, or anything at all.
struct Ref {
  enum class Kind { Node, Zero, One, Any } kind = Kind::Any;
  int node = 0;
};

class Splitter {
public:
  explicit Splitter(int inputs) : inputs_(inputs) {
    this->net_.inputs = inputs;
  }

  std::optional<Ref> build(const Truth &f) {
    if (auto it = this->memo_.find(f); it != this->memo_.end())
      return it->second;
    std::optional<Ref> result = this->build_new(f);
    if (result)
      this->memo_.emplace(f, *result);
    return result;
  }

  Netlist &netlist() { return this->net_; }

private:
  std::optional<Ref> build_new(const Truth &f) {
    bool zero = true, one = true;
    for (std::int8_t v : f) {
      zero &= v != 1;
      one &= v != 0;
    }
    if (zero && one)
      return Ref{Ref::Kind::Any, 0};
    if (zero)
      return Ref{Ref::Kind::Zero, 0};
    if (one)
      return Ref{Ref::Kind::One, 0};
    for (int i = 0; i < this->inputs_; ++i)
      if (this->matches_input(f, i))
        return Ref{Ref::Kind::Node, i};

    // Split on the input whose halves depend on the fewest inputs.
    int best = -1;
    int best_cost = 0;
    for (int i = 0; i < this->inputs_; ++i) {
      if (!this->depends_on(f, i))
        continue;
      int cost = this->support(this->cofactor(f, i, false)) +
                 this->support(this->cofactor(f, i, true));
      if (best < 0 || cost < best_cost) {
        best = i;
        best_cost = cost;
      }
    }
    if (best < 0)
      return std::nullopt;
    std::optional<Ref> low = this->build(this->cofactor(f, best, false));
    std::optional<Ref> high = this->build(this->cofactor(f, best, true));
    if (!low || !high)
      return std::nullopt;
    return this->combine(best, *low, *high);
  }

  // f = s ? high : low, with s an input node.
  std::optional<Ref> combine(int s, Ref low, Ref high) {
    using Kind = Ref::Kind;
    if (low.kind == Kind::Any)
      return high;
    if (high.kind == Kind::Any)
      return low;
    if (low.kind == Kind::Zero && high.kind == Kind::One)
      return Ref{Kind::Node, s};
    if (low.kind == Kind::One && high.kind == Kind::Zero)
      return std::nullopt; // NOT s alone needs a gate with one input
    if (low.kind == Kind::Zero)
      return this->gate(0b1000, s, high.node); // s AND high
    if (high.kind == Kind::Zero)
      return this->gate(0b0010, s, low.node); // low AND NOT s
    if (low.kind == Kind::One)
      return this->gate(0b1011, s, high.node); // NOT s OR high
    if (high.kind == Kind::One)
      return this->gate(0b1110, s, low.node); // s OR low
    if (low.node == high.node)
      return low;
    // low XOR (s AND (low XOR high)): a 2-to-1 multiplexer.
    Ref diff = this->gate(0b0110, low.node, high.node);
    Ref pick = this->gate(0b1000, s, diff.node);
    return this->gate(0b0110, low.node, pick.node);
  }

  Ref gate(std::uint8_t function, int a, int b) {
    auto found = this->gates_.find({function, a, b});
    if (found != this->gates_.end())
      return Ref{Ref::Kind::Node, found->second};
    this->net_.gates.push_back({a, b, function});
    int node = this->inputs_ + static_cast<int>(this->net_.gates.size()) - 1;
    this->gates_.emplace(std::array<int, 3>{function, a, b}, node);
    return Ref{Ref::Kind::Node, node};
  }

  bool matches_input(const Truth &f, int i) const {
    for (std::size_t t = 0; t < f.size(); ++t)
      if (f[t] >= 0 && f[t] != static_cast<int>((t >> i) & 1))
        return false;
    return true;
  }

  bool depends_on(const Truth &f, int i) const {
    for (std::size_t t = 0; t < f.size(); ++t) {
      if (((t >> i) & 1) != 0)
        continue;
      std::int8_t a = f[t];
      std::int8_t b = f[t | (std::size_t{1} << i)];
      if (a >= 0 && b >= 0 && a != b)
        return true;
    }
    return false;
  }

  int support(const Truth &f) const {
    int count = 0;
    for (int i = 0; i < this->inputs_; ++i)
      count += this->depends_on(f, i) ? 1 : 0;
    return count;
  }

  // f with input i fixed; still indexed by every input vector. Where only
  // one of the pair is known, that value is used for both.
  Truth cofactor(const Truth &f, int i, bool value) const {
    Truth result(f.size());
    for (std::size_t t = 0; t < f.size(); ++t) {
      std::size_t fixed =
          value ? (t | (std::size_t{1} << i)) : (t & ~(std::size_t{1} << i));
      result[t] = f[fixed];
    }
    return result;
  }

  int inputs_;
  Netlist net_;
  std::map<Truth, Ref> memo_;
  std::map<std::array<int, 3>, int> gates_;
};

} // namespace

std::optional<Netlist> split_network(const BoolTable &table) {
  Splitter splitter(table.inputs);
  std::vector<int> outputs;
  for (const auto &column : table.values) {
    Truth f(column.size());
    for (std::size_t t = 0; t < column.size(); ++t)
      f[t] = column[t] ? static_cast<std::int8_t>(*column[t]) : std::int8_t{-1};
    std::optional<Ref> ref = splitter.build(f);
    if (!ref || ref->kind != Ref::Kind::Node)
      return std::nullopt; // a constant output, or NOT of an input alone
    outputs.push_back(ref->node);
  }
  Netlist net = std::move(splitter.netlist());
  net.outputs = std::move(outputs);
  return net;
}

} // namespace rs

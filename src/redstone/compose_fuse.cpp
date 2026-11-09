// Fusing: a piece of the gate network that reads only three signals (two
// chained gates, or more) can be one module from the gate catalog instead of
// several modules and the wires between them.

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>

#include "redstone/compose_parts.hpp"

namespace rs::compose_parts {

namespace {

constexpr int kFuseLeaves = 3;
// Cuts kept per node (smallest first) and fusion choices looked at: plenty
// for the networks synthesis makes (ten gates or so).
constexpr std::size_t kMaxCutsPerNode = 24;
constexpr int kMaxChoices = 4096;

using Leaves = std::vector<int>; // node indices, increasing

// A piece that may become one module: the gates from `leaves` up to `root`.
struct Piece {
  int root = 0;
  Leaves leaves;
  std::uint8_t function = 0; // over the leaves in their order
};

// One thing to place: a gate of the network or a fused piece (with the
// gates that joined it as more outputs).
struct Item {
  std::vector<int> operands;
  std::vector<std::uint8_t> functions;
  std::vector<int> nodes;
};

// The root's truth table over the leaves (first leaf the highest bit).
std::uint8_t cone_function(const Netlist &netlist, int root,
                           const Leaves &leaves) {
  const int n = netlist.inputs;
  const int k = static_cast<int>(leaves.size());
  std::uint8_t function = 0;
  for (int bit = 0; bit < (1 << k); ++bit) {
    std::map<int, bool> value;
    for (int i = 0; i < k; ++i)
      value[leaves[i]] = ((bit >> (k - 1 - i)) & 1) != 0;
    std::function<bool(int)> eval = [&](int node) {
      if (auto it = value.find(node); it != value.end())
        return it->second;
      const Gate &gate = netlist.gates[node - n];
      const int index = (eval(gate.a) ? 2 : 0) | (eval(gate.b) ? 1 : 0);
      const bool v = ((gate.function >> index) & 1) != 0;
      value[node] = v;
      return v;
    };
    if (eval(root))
      function |= static_cast<std::uint8_t>(1 << bit);
  }
  return function;
}

// Every way of covering each node with at most three leaves (the node
// itself included), from its operands' cuts.
std::vector<std::vector<Leaves>> enumerate_cuts(const Netlist &netlist) {
  const int n = netlist.inputs;
  std::vector<std::vector<Leaves>> cuts(n + netlist.gates.size());
  for (int i = 0; i < n; ++i)
    cuts[i] = {{i}};
  for (std::size_t g = 0; g < netlist.gates.size(); ++g) {
    const int node = n + static_cast<int>(g);
    const Gate &gate = netlist.gates[g];
    std::set<Leaves> found{{node}};
    for (const Leaves &ca : cuts[gate.a])
      for (const Leaves &cb : cuts[gate.b]) {
        Leaves merged;
        std::set_union(ca.begin(), ca.end(), cb.begin(), cb.end(),
                       std::back_inserter(merged));
        if (static_cast<int>(merged.size()) <= kFuseLeaves)
          found.insert(std::move(merged));
      }
    std::vector<Leaves> list(found.begin(), found.end());
    std::stable_sort(
        list.begin(), list.end(),
        [](const Leaves &a, const Leaves &b) { return a.size() < b.size(); });
    if (list.size() > kMaxCutsPerNode)
      list.resize(kMaxCutsPerNode);
    cuts[node] = std::move(list);
  }
  return cuts;
}

// What has to be placed when the chosen pieces stand in for their gates:
// everything the outputs read, back to the inputs.
std::vector<Item> items_for(const Netlist &netlist,
                            const std::map<int, const Piece *> &chosen) {
  const int n = netlist.inputs;
  std::vector<Item> items;
  std::set<int> seen;
  std::vector<int> todo(netlist.outputs.begin(), netlist.outputs.end());
  while (!todo.empty()) {
    const int node = todo.back();
    todo.pop_back();
    if (node < n || !seen.insert(node).second)
      continue;
    if (auto it = chosen.find(node); it != chosen.end()) {
      items.push_back({it->second->leaves, {it->second->function}, {node}});
      todo.insert(todo.end(), it->second->leaves.begin(),
                  it->second->leaves.end());
      continue;
    }
    const Gate &gate = netlist.gates[node - n];
    items.push_back({{gate.a, gate.b}, {gate.function}, {node}});
    todo.push_back(gate.a);
    todo.push_back(gate.b);
  }
  return items;
}

// Whether `node` is `target` or is computed from it.
bool depends_on(const Netlist &netlist, int node, int target) {
  if (node == target)
    return true;
  if (node < netlist.inputs)
    return false;
  const Gate &gate = netlist.gates[node - netlist.inputs];
  return depends_on(netlist, gate.a, target) ||
         depends_on(netlist, gate.b, target);
}

// Gates that read only a fused piece's signals become more outputs of it,
// when a module computes them all.
std::vector<Item> join_pieces(
    const Netlist &netlist, std::vector<Item> items,
    const std::function<bool(const std::vector<std::uint8_t> &)> &has_module) {
  for (std::size_t p = 0; p < items.size(); ++p) {
    if (items[p].operands.size() != kFuseLeaves)
      continue;
    for (std::size_t q = 0; q < items.size();) {
      const Item &gate = items[q];
      const Leaves &leaves = items[p].operands;
      const bool reads_only_leaves =
          q != p && gate.nodes.size() == 1 &&
          gate.operands.size() < leaves.size() &&
          std::find(leaves.begin(), leaves.end(), gate.nodes[0]) ==
              leaves.end() &&
          std::all_of(
              gate.operands.begin(), gate.operands.end(),
              [&](int op) {
                return std::find(leaves.begin(), leaves.end(), op) !=
                       leaves.end();
              });
      if (!reads_only_leaves) {
        ++q;
        continue;
      }
      // A piece cannot read a signal it makes.
      if (std::any_of(leaves.begin(), leaves.end(), [&](int leaf) {
            return depends_on(netlist, leaf, gate.nodes[0]);
          })) {
        ++q;
        continue;
      }
      std::vector<std::uint8_t> functions = items[p].functions;
      functions.push_back(cone_function(netlist, gate.nodes[0], leaves));
      if (!has_module(functions)) {
        ++q;
        continue;
      }
      items[p].functions = std::move(functions);
      items[p].nodes.push_back(gate.nodes[0]);
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(q));
      if (q < p)
        --p;
    }
  }
  return items;
}

// Items in signal order as groups: 2-input gates of one level that read the
// same signals share a module (as group_gates does); a fused piece is a
// module of its own. `late` places everything as late as its readers allow
// and fused pieces after the gates of their step: a 3-input module's input
// rows are often next to each other, which leaves no room to branch off its
// wires, so it is better placed when its signals are already there.
std::vector<Group> groups_of(const std::vector<Item> &items, bool late) {
  std::map<int, std::size_t> item_of; // node -> item
  for (std::size_t i = 0; i < items.size(); ++i)
    for (int node : items[i].nodes)
      item_of[node] = i;
  std::vector<int> level(items.size(), -1);
  std::function<int(std::size_t)> level_of = [&](std::size_t i) {
    if (level[i] >= 0)
      return level[i];
    int deepest = 0; // inputs are level 0
    for (int operand : items[i].operands)
      if (auto it = item_of.find(operand); it != item_of.end())
        deepest = std::max(deepest, level_of(it->second));
    level[i] = deepest + 1;
    return level[i];
  };
  int top = 0;
  for (std::size_t i = 0; i < items.size(); ++i)
    top = std::max(top, level_of(i));
  if (late) {
    std::vector<std::vector<std::size_t>> readers(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
      for (int operand : items[i].operands)
        if (auto it = item_of.find(operand); it != item_of.end())
          readers[it->second].push_back(i);
    std::vector<int> latest(items.size(), -1);
    std::vector<bool> known(items.size(), false);
    std::function<int(std::size_t)> latest_of = [&](std::size_t i) {
      if (known[i])
        return latest[i];
      int step = top;
      for (std::size_t reader : readers[i])
        step = std::min(step, latest_of(reader) - 1);
      latest[i] = step;
      known[i] = true;
      return step;
    };
    for (std::size_t i = 0; i < items.size(); ++i)
      latest_of(i);
    level = std::move(latest);
  }
  auto fused = [&](std::size_t i) {
    return late && items[i].operands.size() > 2 ? 1 : 0;
  };
  std::vector<std::size_t> order(items.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     if (level[a] != level[b])
                       return level[a] < level[b];
                     if (fused(a) != fused(b))
                       return fused(a) < fused(b);
                     return items[a].nodes[0] < items[b].nodes[0];
                   });
  std::vector<Group> groups;
  std::vector<int> group_level;
  for (std::size_t i : order) {
    const Item &item = items[i];
    std::size_t g = groups.size();
    if (item.operands.size() == 2)
      for (std::size_t k = 0; k < groups.size(); ++k)
        if (groups[k].operands == item.operands && group_level[k] == level[i])
          g = k;
    if (g == groups.size()) {
      groups.push_back({item.operands, {}, {}});
      group_level.push_back(level[i]);
    }
    groups[g].nodes.insert(groups[g].nodes.end(), item.nodes.begin(),
                           item.nodes.end());
    groups[g].functions.insert(groups[g].functions.end(),
                               item.functions.begin(), item.functions.end());
  }
  return groups;
}

} // namespace

std::vector<std::vector<Group>> fused_groupings(
    const Netlist &netlist,
    const std::function<bool(const std::vector<std::uint8_t> &)> &has_module,
    int max_groupings) {
  const int n = netlist.inputs;
  const std::vector<std::vector<Leaves>> cuts = enumerate_cuts(netlist);
  std::map<int, std::vector<Piece>> pieces; // by root
  for (std::size_t g = 0; g < netlist.gates.size(); ++g) {
    const int root = n + static_cast<int>(g);
    for (const Leaves &leaves : cuts[root]) {
      if (static_cast<int>(leaves.size()) != kFuseLeaves)
        continue;
      const std::uint8_t function = cone_function(netlist, root, leaves);
      if (depends_on_all(function, kFuseLeaves) && has_module({function}))
        pieces[root].push_back({root, leaves, function});
    }
  }
  if (pieces.empty())
    return {};

  // Each root: no piece or one of its pieces. The groupings with the fewest
  // things to place win.
  std::vector<int> roots;
  for (const auto &[root, list] : pieces)
    roots.push_back(root);
  const std::size_t plain_items = items_for(netlist, {}).size();
  std::map<std::vector<std::pair<std::vector<int>, std::vector<int>>>,
           std::vector<Item>>
      found; // by what is placed, so equal groupings count once
  std::map<int, const Piece *> chosen;
  int looked = 0;
  std::function<void(std::size_t)> choose = [&](std::size_t r) {
    if (looked >= kMaxChoices)
      return;
    if (r == roots.size()) {
      ++looked;
      if (chosen.empty())
        return;
      std::vector<Item> plain = items_for(netlist, chosen);
      std::vector<Item> joined = join_pieces(netlist, plain, has_module);
      for (std::vector<Item> *items : {&plain, &joined}) {
        if (items->size() >= plain_items)
          continue; // the pieces' gates are needed anyway
        std::vector<std::pair<std::vector<int>, std::vector<int>>> signature;
        for (const Item &item : *items)
          signature.push_back({item.nodes, item.operands});
        std::sort(signature.begin(), signature.end());
        found.emplace(std::move(signature), std::move(*items));
      }
      return;
    }
    choose(r + 1);
    for (const Piece &piece : pieces[roots[r]]) {
      chosen[roots[r]] = &piece;
      choose(r + 1);
      chosen.erase(roots[r]);
    }
  };
  choose(0);

  std::vector<std::vector<Item>> ranked;
  for (auto &[signature, items] : found)
    ranked.push_back(std::move(items));
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const std::vector<Item> &a, const std::vector<Item> &b) {
                     return a.size() < b.size();
                   });
  // Each grouping in signal order and, when it differs, placed late.
  auto same = [](const std::vector<Group> &a, const std::vector<Group> &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const Group &x, const Group &y) {
                        return x.operands == y.operands && x.nodes == y.nodes;
                      });
  };
  std::vector<std::vector<Group>> groupings;
  for (const std::vector<Item> &items : ranked)
    for (bool late : {false, true}) {
      if (static_cast<int>(groupings.size()) >= max_groupings)
        return groupings;
      std::vector<Group> groups = groups_of(items, late);
      if (late && same(groups, groupings.back()))
        continue;
      groupings.push_back(std::move(groups));
    }
  return groupings;
}

std::string group_name(const Group &group) {
  std::string names;
  for (std::uint8_t f : group.functions) {
    std::string name;
    if (group.operands.size() == 2)
      name = function_name(f);
    else if (f == kXor3Function)
      name = "XOR3";
    else if (f == kMajorityFunction)
      name = "MAJORITY";
    else
      name = std::format("3-input {}", f);
    names += (names.empty() ? "" : "+") + name;
  }
  return names;
}

} // namespace rs::compose_parts

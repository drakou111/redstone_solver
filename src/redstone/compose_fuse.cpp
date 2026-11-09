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

// One thing to place: a gate of the network or a fused piece.
struct Item {
  std::vector<int> operands;
  std::uint8_t function = 0;
  int node = 0;
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
      items.push_back({it->second->leaves, it->second->function, node});
      todo.insert(todo.end(), it->second->leaves.begin(),
                  it->second->leaves.end());
      continue;
    }
    const Gate &gate = netlist.gates[node - n];
    items.push_back({{gate.a, gate.b}, gate.function, node});
    todo.push_back(gate.a);
    todo.push_back(gate.b);
  }
  return items;
}

// Items in signal order as groups: 2-input gates of one level that read the
// same signals share a module (as group_gates does); a fused piece is a
// module of its own. `late` places everything as late as its readers allow
// and fused pieces after the gates of their step: a 3-input module's input
// rows are often next to each other, which leaves no room to branch off its
// wires, so it is better placed when its signals are already there.
std::vector<Group> groups_of(std::vector<Item> items, bool late) {
  std::map<int, int> level;
  std::map<int, const Item *> by_node;
  for (const Item &item : items)
    by_node[item.node] = &item;
  std::function<int(int)> level_of = [&](int node) {
    auto it = by_node.find(node);
    if (it == by_node.end())
      return 0; // an input
    if (auto known = level.find(node); known != level.end())
      return known->second;
    int deepest = 0;
    for (int operand : it->second->operands)
      deepest = std::max(deepest, level_of(operand));
    level[node] = deepest + 1;
    return deepest + 1;
  };
  int top = 0;
  for (const Item &item : items)
    top = std::max(top, level_of(item.node));
  if (late) {
    std::map<int, std::vector<int>> readers;
    for (const Item &item : items)
      for (int operand : item.operands)
        readers[operand].push_back(item.node);
    std::map<int, int> latest;
    std::function<int(int)> latest_of = [&](int node) {
      if (auto known = latest.find(node); known != latest.end())
        return known->second;
      int step = top;
      for (int reader : readers[node])
        step = std::min(step, latest_of(reader) - 1);
      latest[node] = step;
      return step;
    };
    for (const Item &item : items)
      latest_of(item.node);
    level = std::move(latest);
  }
  auto fused = [&](const Item &item) {
    return late && item.operands.size() > 2 ? 1 : 0;
  };
  std::stable_sort(items.begin(), items.end(),
                   [&](const Item &a, const Item &b) {
                     if (level[a.node] != level[b.node])
                       return level[a.node] < level[b.node];
                     if (fused(a) != fused(b))
                       return fused(a) < fused(b);
                     return a.node < b.node;
                   });
  std::vector<Group> groups;
  for (const Item &item : items) {
    auto same = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
      return item.operands.size() == 2 && g.operands == item.operands &&
             level[g.nodes[0]] == level[item.node];
    });
    if (same == groups.end()) {
      groups.push_back({item.operands, {}, {}});
      same = groups.end() - 1;
    }
    same->nodes.push_back(item.node);
    same->functions.push_back(item.function);
  }
  return groups;
}

} // namespace

std::vector<std::vector<Group>>
fused_groupings(const Netlist &netlist,
                const std::function<bool(std::uint8_t)> &has_module,
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
      if (depends_on_all(function, kFuseLeaves) && has_module(function))
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
  std::map<std::vector<std::pair<int, std::vector<int>>>, std::vector<Item>>
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
      std::vector<Item> items = items_for(netlist, chosen);
      if (items.size() >= plain_items)
        return; // the pieces' gates are needed anyway
      std::vector<std::pair<int, std::vector<int>>> signature;
      for (const Item &item : items)
        signature.push_back({item.node, item.operands});
      std::sort(signature.begin(), signature.end());
      found.emplace(std::move(signature), std::move(items));
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

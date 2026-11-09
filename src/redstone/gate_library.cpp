#include "redstone/gate_library.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <stdexcept>

#include "redstone/netlist.hpp"

namespace rs {

namespace {

// Truth tables are 8 bits: three inputs at most.
constexpr std::size_t kMaxModuleInputs = 3;

std::string gate_spec_text(const std::vector<std::uint8_t> &functions,
                           Vec3 size, const std::vector<int> &in_rows,
                           const std::vector<int> &out_rows, int horizon) {
  const auto [mx, my, mz] = size;
  const int inputs = static_cast<int>(in_rows.size());
  std::string text = std::format("size {} {} {}\n", mx, my, mz);
  for (int k = 0; k < inputs; ++k)
    text += std::format("input IN{} repeater 0 1 {} east\n", k + 1, in_rows[k]);
  for (std::size_t o = 0; o < out_rows.size(); ++o)
    text +=
        std::format("output OUT{} dust {} 1 {}\n", o + 1, mx - 1, out_rows[o]);
  text += std::format("timing none\nhorizon {}\n", horizon);
  for (int bit = 0; bit < (1 << inputs); ++bit) {
    text += "row";
    for (int k = 0; k < inputs; ++k) {
      const bool on = ((bit >> (inputs - 1 - k)) & 1) != 0;
      text += std::format("{}IN{}(ss{})", k ? "," : " ", k + 1, on ? 15 : 0);
    }
    text += " ->";
    for (std::size_t o = 0; o < functions.size(); ++o) {
      bool on = ((functions[o] >> bit) & 1) != 0;
      text += std::format("{}OUT{}({})", o ? "," : " ", o + 1,
                          on ? "ss1..15" : "ss0");
    }
    text += "\n";
  }
  // Clean faces: above the floor, only the ports on the west and east faces.
  for (int y = 1; y < my; ++y)
    for (int z = 0; z < mz; ++z) {
      const bool in = y == 1 && std::find(in_rows.begin(), in_rows.end(), z) !=
                                    in_rows.end();
      if (!in)
        text += std::format("empty 0 {} {}\n", y, z);
      bool out = y == 1 && std::find(out_rows.begin(), out_rows.end(), z) !=
                               out_rows.end();
      if (!out)
        text += std::format("empty {} {} {}\n", mx - 1, y, z);
    }
  return text;
}

// Choices of output rows on an east face `rows` tall: one row per output,
// no two next to each other (their dust would join). A single output tries
// the middle first.
std::vector<std::vector<int>> output_row_choices(int outputs, int rows) {
  std::vector<std::vector<int>> choices;
  std::vector<int> current;
  std::function<void(int)> pick = [&](int from) {
    if (static_cast<int>(current.size()) == outputs) {
      choices.push_back(current);
      return;
    }
    for (int row = from; row < rows; ++row) {
      current.push_back(row);
      pick(row + 2);
      current.pop_back();
    }
  };
  pick(0);
  if (outputs == 1)
    std::stable_sort(choices.begin(), choices.end(),
                     [&](const auto &a, const auto &b) {
                       return std::abs(2 * a[0] - (rows - 1)) <
                              std::abs(2 * b[0] - (rows - 1));
                     });
  return choices;
}

} // namespace

std::uint8_t permute_function(std::uint8_t function, int inputs,
                              const std::vector<int> &order) {
  std::uint8_t result = 0;
  for (int bit = 0; bit < (1 << inputs); ++bit) {
    // The result's input k is the function's input order[k].
    int source = 0;
    for (int k = 0; k < inputs; ++k)
      if ((bit >> (inputs - 1 - k)) & 1)
        source |= 1 << (inputs - 1 - order[k]);
    if ((function >> source) & 1)
      result |= static_cast<std::uint8_t>(1 << bit);
  }
  return result;
}

bool depends_on_all(std::uint8_t function, int inputs) {
  for (int k = 0; k < inputs; ++k) {
    const int flip = 1 << (inputs - 1 - k);
    bool depends = false;
    for (int bit = 0; bit < (1 << inputs) && !depends; ++bit)
      depends = ((function >> bit) & 1) != ((function >> (bit ^ flip)) & 1);
    if (!depends)
      return false;
  }
  return true;
}

std::vector<GateArrangement> gate_arrangements(Vec3 size, int inputs,
                                               int outputs) {
  if (inputs < 2)
    throw std::invalid_argument("a gate module has at least two inputs");
  std::vector<GateArrangement> arrangements;
  std::vector<int> rows;
  std::function<void()> pick = [&] {
    if (static_cast<int>(rows.size()) == inputs) {
      for (const std::vector<int> &out : output_row_choices(outputs, size.z))
        arrangements.push_back({size, rows, out});
      return;
    }
    const int from = inputs > 2 && !rows.empty() ? rows.back() + 1 : 0;
    for (int z = from; z < size.z; ++z) {
      if (std::find(rows.begin(), rows.end(), z) != rows.end())
        continue;
      rows.push_back(z);
      pick();
      rows.pop_back();
    }
  };
  pick();
  return arrangements;
}

std::optional<GateModule>
build_gate_in(const std::vector<std::uint8_t> &functions,
              const GateArrangement &arrangement,
              const GateLibraryOptions &options) {
  if (arrangement.out_rows.size() != functions.size())
    throw std::invalid_argument("one output row per function");
  const Vec3 size = arrangement.size;
  if (arrangement.in_rows.size() > kMaxModuleInputs)
    throw std::invalid_argument("a module has at most three inputs");
  Spec spec = parse_spec(gate_spec_text(functions, size, arrangement.in_rows,
                                        arrangement.out_rows, options.horizon));
  if (options.palette)
    spec.palette = *options.palette;
  SolverOptions solver = options.solver;
  solver.time_limit_seconds = options.time_limit_seconds;
  solver.stop_at_first = false; // the fewest blocks found in the time
  solver.cancel = options.cancel;
  SolveReport report = solve(spec, solver);
  if (!report.solution)
    return std::nullopt;
  GateModule module{functions, report.solution->layout, {}, {}};
  for (int row : arrangement.in_rows)
    module.ins.push_back({0, 1, row});
  for (int row : arrangement.out_rows)
    module.outs.push_back({size.x - 1, 1, row});
  return module;
}

std::string catalog_key(const std::optional<std::vector<BlockType>> &palette,
                        const VerificationOptions &verification) {
  std::string key = "palette";
  for (BlockType type : palette.value_or(default_palette()))
    key += std::format(" {}", palette_name(type));
  key += std::format(" placements {}", verification.placements.size());
  return key;
}

namespace {

int module_blocks(const GateModule &module) {
  int blocks = 0;
  for (int i = 0; i < module.layout.volume(); ++i) {
    BlockType type = module.layout.at(module.layout.position_of(i)).type;
    blocks += type != BlockType::Air && type != BlockType::Input &&
                      type != BlockType::Output
                  ? 1
                  : 0;
  }
  return blocks;
}

bool same_arrangement(const GateModule &module,
                      const GateArrangement &arrangement) {
  if (module.layout.size() != arrangement.size ||
      module.ins.size() != arrangement.in_rows.size() ||
      module.outs.size() != arrangement.out_rows.size())
    return false;
  for (std::size_t k = 0; k < module.ins.size(); ++k)
    if (module.ins[k].z != arrangement.in_rows[k])
      return false;
  for (std::size_t o = 0; o < module.outs.size(); ++o)
    if (module.outs[o].z != arrangement.out_rows[o])
      return false;
  return true;
}

GateArrangement arrangement_of(const GateModule &module) {
  GateArrangement arrangement{module.layout.size(), {}, {}};
  for (Vec3 in : module.ins)
    arrangement.in_rows.push_back(in.z);
  for (Vec3 out : module.outs)
    arrangement.out_rows.push_back(out.z);
  return arrangement;
}

} // namespace

bool GateCatalog::add(const std::string &key, const GateModule &module) {
  std::lock_guard lock(this->mutex_);
  std::vector<GateModule> &list = this->modules_[{
      key, static_cast<int>(module.ins.size()), module.functions}];
  const GateArrangement arrangement = arrangement_of(module);
  for (GateModule &kept : list)
    if (same_arrangement(kept, arrangement)) {
      if (module_blocks(kept) <= module_blocks(module))
        return false;
      kept = module;
      return true;
    }
  list.push_back(module);
  return true;
}

std::vector<GateModule>
GateCatalog::variants(const std::string &key, int inputs,
                      const std::vector<std::uint8_t> &functions,
                      int max_height) const {
  std::lock_guard lock(this->mutex_);
  auto it = this->modules_.find({key, inputs, functions});
  if (it == this->modules_.end())
    return {};
  std::vector<GateModule> result;
  for (const GateModule &module : it->second)
    if (max_height <= 0 || module.layout.size().y <= max_height)
      result.push_back(module);
  return result;
}

bool GateCatalog::has_any(const std::string &key, int inputs,
                          const std::vector<std::uint8_t> &functions) const {
  std::lock_guard lock(this->mutex_);
  auto it = this->modules_.find({key, inputs, functions});
  return it != this->modules_.end() && !it->second.empty();
}

bool GateCatalog::has(const std::string &key,
                      const std::vector<std::uint8_t> &functions,
                      const GateArrangement &arrangement) const {
  std::lock_guard lock(this->mutex_);
  auto it = this->modules_.find(
      {key, static_cast<int>(arrangement.in_rows.size()), functions});
  if (it == this->modules_.end())
    return false;
  return std::any_of(
      it->second.begin(), it->second.end(),
      [&](const GateModule &m) { return same_arrangement(m, arrangement); });
}

std::vector<std::pair<std::string, GateModule>> GateCatalog::all() const {
  std::lock_guard lock(this->mutex_);
  std::vector<std::pair<std::string, GateModule>> result;
  for (const auto &[key, list] : this->modules_)
    for (const GateModule &module : list)
      result.emplace_back(std::get<0>(key), module);
  return result;
}

std::optional<GateModule> GateCache::find(const std::string &key) const {
  std::lock_guard lock(this->mutex_);
  auto it = this->modules_.find(key);
  if (it == this->modules_.end())
    return std::nullopt;
  return it->second;
}

void GateCache::store(const std::string &key, const GateModule &module) {
  std::lock_guard lock(this->mutex_);
  this->modules_.insert_or_assign(key, module);
}

std::string gate_cache_key(const std::vector<std::uint8_t> &functions,
                           const GateLibraryOptions &options) {
  std::string key = "f";
  for (std::uint8_t f : functions)
    key += std::format(" {}", f);
  key += " palette";
  const std::vector<BlockType> palette =
      options.palette.value_or(default_palette());
  for (BlockType type : palette)
    key += std::format(" {}", palette_name(type));
  key += " s";
  for (Vec3 size : options.sizes)
    key += std::format(" {}x{}x{}", size.x, size.y, size.z);
  const VerificationOptions &verification = options.solver.verification;
  key +=
      std::format(" h {} settle {} extra {} placements", options.horizon,
                  verification.max_settle_ticks, verification.max_extra_ticks);
  if (verification.placements.empty()) {
    // Empty means the verifier's built-in standard placement set.
    key += " standard";
  } else {
    for (const Placement &placement : verification.placements) {
      const Vec3 origin = placement.origin;
      key += std::format(" {}:{}:{}:{}:{}", origin.x, origin.y, origin.z,
                         placement.symmetry.quarter_turns(),
                         placement.symmetry.mirrored());
    }
  }
  return key;
}

std::optional<GateModule> build_gate(const std::vector<std::uint8_t> &functions,
                                     const GateLibraryOptions &options,
                                     const ProgressCallback &progress) {
  std::string names;
  for (std::uint8_t f : functions)
    names += (names.empty() ? "" : " + ") + function_name(f);
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.time_limit_seconds));
  int attempts_left = 0;
  for (Vec3 size : options.sizes)
    attempts_left += static_cast<int>(
        output_row_choices(static_cast<int>(functions.size()), size.z).size());
  for (Vec3 size : options.sizes) {
    for (const std::vector<int> &rows :
         output_row_choices(static_cast<int>(functions.size()), size.z)) {
      if (options.cancel && options.cancel->load())
        return std::nullopt;
      double remaining = std::chrono::duration<double>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
      if (remaining <= 0)
        return std::nullopt;
      Spec spec = parse_spec(gate_spec_text(functions, size, {0, size.z - 1},
                                            rows, options.horizon));
      if (options.palette)
        spec.palette = *options.palette;
      SolverOptions solver = options.solver;
      // A gate module is one logical build; share its budget across every
      // box size and output-row arrangement instead of restarting that timer
      // for each variant.
      solver.time_limit_seconds = remaining / attempts_left;
      solver.stop_at_first = true;
      solver.cancel = options.cancel;
      --attempts_left;
      SolveReport report = solve(spec, solver);
      if (progress)
        progress(std::format("{} in {}x{}x{}: {}", names, size.x, size.y,
                             size.z,
                             report.solution ? std::string_view("found")
                                             : to_string(report.status)));
      if (!report.solution)
        continue;
      GateModule module{functions,
                        report.solution->layout,
                        {{0, 1, 0}, {0, 1, size.z - 1}},
                        {}};
      for (int row : rows)
        module.outs.push_back({size.x - 1, 1, row});
      return module;
    }
  }
  return std::nullopt;
}

} // namespace rs

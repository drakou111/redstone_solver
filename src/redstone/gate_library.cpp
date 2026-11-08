#include "redstone/gate_library.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>

#include "redstone/netlist.hpp"

namespace rs {

namespace {

std::string gate_spec_text(const std::vector<std::uint8_t> &functions,
                           Vec3 size, const std::vector<int> &out_rows,
                           int horizon) {
  const auto [mx, my, mz] = size;
  std::string text = std::format("size {} {} {}\n"
                                 "input IN1 repeater 0 1 0 east\n"
                                 "input IN2 repeater 0 1 {} east\n",
                                 mx, my, mz, mz - 1);
  for (std::size_t o = 0; o < out_rows.size(); ++o)
    text +=
        std::format("output OUT{} dust {} 1 {}\n", o + 1, mx - 1, out_rows[o]);
  text += std::format("timing none\nhorizon {}\n", horizon);
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b) {
      text += std::format("row IN1(ss{}),IN2(ss{}) ->", 15 * a, 15 * b);
      for (std::size_t o = 0; o < functions.size(); ++o) {
        bool on = ((functions[o] >> ((a << 1) | b)) & 1) != 0;
        text += std::format("{}OUT{}({})", o ? "," : " ", o + 1,
                            on ? "ss1..15" : "ss0");
      }
      text += "\n";
    }
  // Clean faces: above the floor, only the ports on the west and east faces.
  for (int y = 1; y < my; ++y)
    for (int z = 0; z < mz; ++z) {
      if (!(y == 1 && (z == 0 || z == mz - 1)))
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
  key += std::format(" h {} settle {} extra {} placements", options.horizon,
                     verification.max_settle_ticks,
                     verification.max_extra_ticks);
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
      Spec spec =
          parse_spec(gate_spec_text(functions, size, rows, options.horizon));
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
                        {0, 1, 0},
                        {0, 1, size.z - 1},
                        {}};
      for (int row : rows)
        module.outs.push_back({size.x - 1, 1, row});
      return module;
    }
  }
  return std::nullopt;
}

} // namespace rs

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
                           Vec3 size, const std::vector<Vec3> &ins,
                           const std::vector<Vec3> &outs, int horizon) {
  const auto [mx, my, mz] = size;
  const int inputs = static_cast<int>(ins.size());
  std::string text = std::format("size {} {} {}\n", mx, my, mz);
  for (int k = 0; k < inputs; ++k)
    text += std::format("input IN{} repeater {} {} {} {}\n", k + 1, ins[k].x,
                        ins[k].y, ins[k].z,
                        to_string(module_input_facing(size, ins[k])));
  for (std::size_t o = 0; o < outs.size(); ++o)
    text += std::format("output OUT{} dust {} {} {}\n", o + 1, outs[o].x,
                        outs[o].y, outs[o].z);
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
  // Clean faces: above the floor, only the ports on the west and east faces
  // and on a north or south face with a port.
  auto is_port = [&](Vec3 p) {
    return std::find(ins.begin(), ins.end(), p) != ins.end() ||
           std::find(outs.begin(), outs.end(), p) != outs.end();
  };
  for (int y = 1; y < my; ++y)
    for (int z = 0; z < mz; ++z) {
      if (!is_port({0, y, z}))
        text += std::format("empty 0 {} {}\n", y, z);
      if (!is_port({mx - 1, y, z}))
        text += std::format("empty {} {} {}\n", mx - 1, y, z);
    }
  auto on_face = [&](int face) {
    return
        [face, mx](Vec3 p) { return p.x > 0 && p.x < mx - 1 && p.z == face; };
  };
  for (int face : {0, mz - 1}) {
    const bool used = std::any_of(ins.begin(), ins.end(), on_face(face)) ||
                      std::any_of(outs.begin(), outs.end(), on_face(face));
    if (!used)
      continue;
    for (int y = 1; y < my; ++y)
      for (int x = 1; x < mx - 1; ++x)
        if (!is_port({x, y, face}))
          text += std::format("empty {} {} {}\n", x, y, face);
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

Dir module_input_facing(Vec3 size, Vec3 in) {
  if (in.x == 0)
    return Dir::East;
  if (in.z == 0)
    return Dir::South;
  if (in.z == size.z - 1)
    return Dir::North;
  throw std::invalid_argument("a module input is not on the west, north or "
                              "south face");
}

Dir module_output_facing(Vec3 size, Vec3 out) {
  if (out.x == size.x - 1)
    return Dir::East;
  if (out.z == 0)
    return Dir::North;
  if (out.z == size.z - 1)
    return Dir::South;
  throw std::invalid_argument("a module output is not on the east, north or "
                              "south face");
}

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
                                               int outputs, bool sides) {
  if (inputs < 2)
    throw std::invalid_argument("a gate module has at least two inputs");
  // Two deep, a north input would feed the south one.
  const bool side_ports = sides && size.z >= 3;
  std::vector<Vec3> places;
  std::vector<Vec3> out_places;
  for (int z = 0; z < size.z; ++z) {
    places.push_back({0, 1, z});
    out_places.push_back({size.x - 1, 1, z});
  }
  if (side_ports)
    for (int face : {0, size.z - 1})
      for (int x = 1; x < size.x - 1; ++x) {
        places.push_back({x, 1, face});
        out_places.push_back({x, 1, face});
      }
  // A west input in a corner row feeds the side input next to it.
  auto feeds = [&](Vec3 a, Vec3 b) {
    if (a.x == 0 && b.x > 0)
      return a + offset(Dir::East) == b;
    if (b.x == 0 && a.x > 0)
      return b + offset(Dir::East) == a;
    return false;
  };
  std::vector<GateArrangement> arrangements;
  std::vector<Vec3> ins;
  std::vector<Vec3> outs;
  auto fits = [&](Vec3 out) {
    for (Vec3 in : ins)
      if (in == out || in + offset(module_input_facing(size, in)) == out)
        return false;
    for (Vec3 other : outs) {
      const Vec3 d = other - out;
      if (std::abs(d.x) + std::abs(d.y) + std::abs(d.z) <= 1)
        return false; // the same cell, or dust that would join
    }
    return true;
  };
  std::function<void(std::size_t)> pick_outputs = [&](std::size_t from) {
    if (static_cast<int>(outs.size()) == outputs) {
      arrangements.push_back({size, ins, outs});
      return;
    }
    for (std::size_t i = side_ports ? 0 : from; i < out_places.size(); ++i) {
      if (!fits(out_places[i]))
        continue;
      outs.push_back(out_places[i]);
      pick_outputs(i + 1);
      outs.pop_back();
    }
  };
  std::vector<std::size_t> picked;
  std::function<void()> pick = [&] {
    if (static_cast<int>(picked.size()) == inputs) {
      ins.clear();
      for (std::size_t i : picked)
        ins.push_back(places[i]);
      pick_outputs(0);
      return;
    }
    const std::size_t from =
        inputs > 2 && !picked.empty() ? picked.back() + 1 : 0;
    for (std::size_t i = from; i < places.size(); ++i) {
      if (std::find(picked.begin(), picked.end(), i) != picked.end() ||
          std::any_of(picked.begin(), picked.end(), [&](std::size_t j) {
            return feeds(places[i], places[j]);
          }))
        continue;
      picked.push_back(i);
      pick();
      picked.pop_back();
    }
  };
  pick();
  return arrangements;
}

std::optional<GateModule>
build_gate_in(const std::vector<std::uint8_t> &functions,
              const GateArrangement &arrangement,
              const GateLibraryOptions &options) {
  if (arrangement.outs.size() != functions.size())
    throw std::invalid_argument("one output per function");
  const Vec3 size = arrangement.size;
  if (arrangement.ins.size() > kMaxModuleInputs)
    throw std::invalid_argument("a module has at most three inputs");
  Spec spec = parse_spec(gate_spec_text(functions, size, arrangement.ins,
                                        arrangement.outs, options.horizon));
  if (options.palette)
    spec.palette = *options.palette;
  SolverOptions solver = options.solver;
  solver.time_limit_seconds = options.time_limit_seconds;
  solver.stop_at_first = false; // the fewest blocks found in the time
  solver.cancel = options.cancel;
  SolveReport report = solve(spec, solver);
  if (!report.solution)
    return std::nullopt;
  return GateModule{functions, report.solution->layout, arrangement.ins,
                    arrangement.outs};
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
      module.ins != arrangement.ins || module.outs != arrangement.outs)
    return false;
  return true;
}

GateArrangement arrangement_of(const GateModule &module) {
  return GateArrangement{module.layout.size(), module.ins, module.outs};
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
      {key, static_cast<int>(arrangement.ins.size()), functions});
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
      std::vector<Vec3> outs;
      for (int row : rows)
        outs.push_back({size.x - 1, 1, row});
      Spec spec = parse_spec(gate_spec_text(functions, size,
                                            {{0, 1, 0}, {0, 1, size.z - 1}},
                                            outs, options.horizon));
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

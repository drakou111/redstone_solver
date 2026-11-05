#include "redstone/compose.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace rs {

namespace {

constexpr SignalRange kOff{0, 0};
constexpr SignalRange kOn{1, kMaxSignal};
constexpr SignalRange kInputOn{kMaxSignal, kMaxSignal};

using Cells = std::map<std::array<int, 3>, Block>;
std::array<int, 3> key(Vec3 p) { return {p.x, p.y, p.z}; }

std::string port_lines(const std::vector<Vec3> &inputs,
                       const std::vector<Vec3> &outputs) {
  std::string text;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    text += std::format("input IN{} repeater {} {} {} east\n", i + 1,
                        inputs[i].x, inputs[i].y, inputs[i].z);
  for (std::size_t i = 0; i < outputs.size(); ++i)
    text += std::format("output OUT{} dust {} {} {}\n", i + 1, outputs[i].x,
                        outputs[i].y, outputs[i].z);
  return text;
}

// Rows of `outputs` (per input vector over `inputs` on/off inputs).
std::vector<TruthRow>
truth_rows(int inputs,
           const std::function<std::vector<bool>(std::vector<bool>)> &f) {
  std::vector<TruthRow> rows;
  for (int v = 0; v < (1 << inputs); ++v) {
    std::vector<bool> in;
    TruthRow row;
    for (int i = 0; i < inputs; ++i) {
      in.push_back(((v >> i) & 1) != 0);
      row.inputs.push_back(in.back() ? kInputOn : kOff);
    }
    for (bool on : f(in))
      row.outputs.push_back(on ? kOn : kOff);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string range_text(SignalRange r) {
  return r.min == r.max ? std::format("ss{}", r.min)
                        : std::format("ss{}..{}", r.min, r.max);
}

// A spec of the given box and ports with `rows`, settling within `horizon`.
Spec base_spec(Vec3 size, const std::vector<Vec3> &inputs,
               const std::vector<Vec3> &outputs,
               const std::vector<TruthRow> &rows, int horizon) {
  std::string text =
      std::format("size {} {} {}\n{}timing none\nhorizon {}\n", size.x, size.y,
                  size.z, port_lines(inputs, outputs), horizon);
  for (const TruthRow &row : rows) {
    text += "row ";
    for (std::size_t i = 0; i < row.inputs.size(); ++i)
      text += std::format("{}IN{}({})", i ? "," : "", i + 1,
                          range_text(row.inputs[i]));
    text += " ->";
    for (std::size_t o = 0; o < row.outputs.size(); ++o)
      text += std::format("{}OUT{}({})", o ? "," : " ", o + 1,
                          range_text(row.outputs[o]));
    text += "\n";
  }
  return parse_spec(text);
}

// Blocks of a solved layout, its ports turned into the plain blocks they
// are (a repeater input, dust output), moved by `offset`.
void add_layout(Cells &cells, const World &layout, Vec3 offset) {
  for (int i = 0; i < layout.volume(); ++i) {
    Vec3 p = layout.position_of(i);
    Block block = layout.at(p);
    if (block.type == BlockType::Air)
      continue;
    if (block.type == BlockType::Input)
      block = Block::repeater(block.facing);
    else if (block.type == BlockType::Output)
      block = Block::dust();
    cells[key(p + offset)] = block;
  }
}

// Pins `cells` (except where the spec's ports are) and leaves every other
// cell outside `free` empty.
void pin(Spec &spec, const Cells &cells,
         const std::function<bool(Vec3)> &free) {
  World base = spec.base_world();
  auto is_port = [&](Vec3 p) {
    BlockType t = base.at(p).type;
    return t == BlockType::Input || t == BlockType::Output;
  };
  for (int i = 0; i < base.volume(); ++i) {
    Vec3 p = base.position_of(i);
    if (is_port(p))
      continue;
    if (auto it = cells.find(key(p)); it != cells.end())
      spec.fixed.emplace_back(p, it->second);
    else if (!free(p))
      spec.forbidden.push_back(p);
  }
}

} // namespace

std::optional<ComposeResult>
compose_full_adder(const ComposeOptions &options,
                   const ProgressCallback &progress) {
  const auto [mx, my, mz] = options.module_size;
  if (mx < 2 || my < 2 || mz < 3 || options.and_row >= options.xor_row ||
      options.xor_row >= mz || options.gap < 0)
    throw std::invalid_argument("module size or output rows do not fit");
  auto say = [&](const std::string &message) {
    if (progress)
      progress(message);
  };
  auto run = [&](const Spec &spec, double seconds,
                 const std::string &step) -> std::optional<World> {
    if (options.cancel && options.cancel->load())
      return std::nullopt;
    SolverOptions solver = options.solver;
    solver.time_limit_seconds = seconds;
    solver.stop_at_first = true;
    solver.cancel = options.cancel;
    SolveReport report = solve(spec, solver);
    say(std::format("{}: {} ({:.1f}s)", step,
                    report.solution ? std::string_view("found")
                                    : to_string(report.status),
                    report.seconds));
    if (!report.solution)
      return std::nullopt;
    return report.solution->layout;
  };

  // The half adder module.
  const Vec3 m_in1{0, 1, 0}, m_in2{0, 1, mz - 1};
  const Vec3 m_xor{mx - 1, 1, options.xor_row};
  const Vec3 m_and{mx - 1, 1, options.and_row};
  Spec module = base_spec(options.module_size, {m_in1, m_in2}, {m_xor, m_and},
                          truth_rows(2,
                                     [](std::vector<bool> in) {
                                       return std::vector<bool>{in[0] != in[1],
                                                                in[0] && in[1]};
                                     }),
                          options.horizon);
  if (options.clean_faces)
    for (int x : {0, mx - 1})
      for (int y = 1; y < my; ++y)
        for (int z = 0; z < mz; ++z) {
          Vec3 p{x, y, z};
          if (p != m_in1 && p != m_in2 && p != m_xor && p != m_and)
            module.forbidden.push_back(p);
        }
  std::optional<World> module_layout =
      run(module, options.module_seconds, "half adder module");
  if (!module_layout)
    return std::nullopt;

  // Geometry: HA2 right after HA1 (plus the gap), its first input in line
  // with HA1's XOR output; the third input enters one row below HA1.
  const int x2 = mx + options.gap;
  const int z2 = options.xor_row;
  const int z_c = mz;
  const Vec3 size{x2 + mx, my, std::max(z2 + mz, z_c + 1)};
  const std::vector<Vec3> inputs{{0, 1, 0}, {0, 1, mz - 1}, {0, 1, z_c}};
  const Vec3 out1{x2 + mx - 1, 1, z2 + options.xor_row};
  const Vec3 out2{x2 + mx - 1, 1, z2 + options.and_row};
  Cells modules;
  add_layout(modules, *module_layout, {0, 0, 0});
  add_layout(modules, *module_layout, {x2, 0, z2});

  // Stage 1: route the third input; only the sum is asked for.
  Spec stage1 = base_spec(size, inputs, {out1},
                          truth_rows(3,
                                     [](std::vector<bool> in) {
                                       return std::vector<bool>{
                                           (in[0] != in[1]) != in[2]};
                                     }),
                          options.horizon);
  pin(stage1, modules, [&](Vec3 p) {
    return p.y == 0 || (p.z >= mz && p.x < x2) || (p.x >= mx && p.x < x2);
  });
  std::optional<World> layout1 =
      run(stage1, options.stage_seconds, "stage 1 (third input)");
  if (!layout1)
    return std::nullopt;

  // Stage 2: route HA1's AND output north of HA2 into the carry.
  auto full_adder = [](std::vector<bool> in) {
    int count = static_cast<int>(in[0]) + static_cast<int>(in[1]) +
                static_cast<int>(in[2]);
    return std::vector<bool>{count % 2 == 1, count >= 2};
  };
  Spec stage2 = base_spec(size, inputs, {out1, out2}, truth_rows(3, full_adder),
                          options.horizon);
  Cells stage1_cells;
  add_layout(stage1_cells, *layout1, {0, 0, 0});
  pin(stage2, stage1_cells,
      [&](Vec3 p) { return p.y == 0 || (p.z < z2 && p.x >= mx - 1); });
  std::optional<World> layout2 =
      run(stage2, options.stage_seconds, "stage 2 (carry)");
  if (!layout2)
    return std::nullopt;

  // The result: the plain full adder spec with the circuit as its sketch.
  Spec result = base_spec(size, inputs, {out1, out2}, truth_rows(3, full_adder),
                          options.horizon);
  for (int i = 0; i < layout2->volume(); ++i) {
    Vec3 p = layout2->position_of(i);
    const Block &block = layout2->at(p);
    if (block.type != BlockType::Air && block.type != BlockType::Input &&
        block.type != BlockType::Output)
      result.hints.emplace_back(p, block);
  }
  return ComposeResult{std::move(result), std::move(*layout2)};
}

} // namespace rs

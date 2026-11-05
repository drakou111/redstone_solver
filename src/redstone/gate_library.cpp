#include "redstone/gate_library.hpp"

#include <algorithm>
#include <format>

#include "redstone/netlist.hpp"

namespace rs {

namespace {

std::string gate_spec_text(std::uint8_t function, Vec3 size, int out_row,
                           int horizon) {
  const auto [mx, my, mz] = size;
  std::string text = std::format("size {} {} {}\n"
                                 "input IN1 repeater 0 1 0 east\n"
                                 "input IN2 repeater 0 1 {} east\n"
                                 "output OUT1 dust {} 1 {}\n"
                                 "timing none\nhorizon {}\n",
                                 mx, my, mz, mz - 1, mx - 1, out_row, horizon);
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b) {
      bool on = ((function >> ((a << 1) | b)) & 1) != 0;
      text += std::format("row IN1(ss{}),IN2(ss{}) -> OUT1({})\n", 15 * a,
                          15 * b, on ? "ss1..15" : "ss0");
    }
  // Clean faces: above the floor, only the ports on the west and east faces.
  for (int y = 1; y < my; ++y)
    for (int z = 0; z < mz; ++z) {
      if (!(y == 1 && (z == 0 || z == mz - 1)))
        text += std::format("empty 0 {} {}\n", y, z);
      if (!(y == 1 && z == out_row))
        text += std::format("empty {} {} {}\n", mx - 1, y, z);
    }
  return text;
}

} // namespace

std::optional<GateModule> build_gate(std::uint8_t function,
                                     const GateLibraryOptions &options,
                                     const ProgressCallback &progress) {
  for (Vec3 size : options.sizes) {
    // Output rows from the middle outwards.
    std::vector<int> rows;
    for (int d = 0; d < size.z; ++d)
      for (int row : {size.z / 2 - d, size.z / 2 + d})
        if (row >= 0 && row < size.z &&
            std::find(rows.begin(), rows.end(), row) == rows.end())
          rows.push_back(row);
    for (int row : rows) {
      if (options.cancel && options.cancel->load())
        return std::nullopt;
      Spec spec =
          parse_spec(gate_spec_text(function, size, row, options.horizon));
      SolverOptions solver = options.solver;
      solver.time_limit_seconds = options.seconds_per_size;
      solver.stop_at_first = true;
      solver.cancel = options.cancel;
      SolveReport report = solve(spec, solver);
      if (progress)
        progress(std::format("{} gate in {}x{}x{}, output row {}: {}",
                             function_name(function), size.x, size.y, size.z,
                             row,
                             report.solution ? std::string_view("found")
                                             : to_string(report.status)));
      if (report.solution)
        return GateModule{function,
                          report.solution->layout,
                          {0, 1, 0},
                          {0, 1, size.z - 1},
                          {size.x - 1, 1, row}};
    }
  }
  return std::nullopt;
}

} // namespace rs

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redstone/solve_job.hpp"
#include "gui/textures.hpp"
#include "gui/views.hpp"
#include "redstone/simulator.hpp"
#include "redstone/solver.hpp"
#include "redstone/spec.hpp"

namespace rs::gui {

// Editable form of a port; kind 0 is strong/dust, 1 is container/component.
struct PortDraft {
  int kind = 0;
  std::array<int, 3> position{};
  int facing = index_of(Dir::East);
};

// Truth table row as the text typed in each cell ("*", "0", "1..15").
struct RowDraft {
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
};

class App {
public:
  // Opens `spec_path` if given (else the AND example) and optionally starts
  // solving right away.
  explicit App(const std::string &spec_path = {}, bool solve_now = false);
  void draw();
  bool is_solving() const { return this->job_ != nullptr; }
  // Brings a result tab to the front on the next frame ("3D view", "Layers",
  // "Blocks", "Timelines", "Export" or "Log").
  void select_tab(std::string name) { this->requested_tab_ = std::move(name); }

private:
  void draw_spec_panel();
  void draw_file_section();
  void draw_ports(std::vector<PortDraft> &ports, bool inputs);
  void draw_truth_table();
  void draw_timing();
  void draw_options();
  void draw_solve_bar();
  void draw_result_area();
  void draw_playground();
  void draw_timelines();

  void poll_job();
  void start_solve();
  Spec build_spec() const;
  void load_spec(const Spec &spec);
  void load_file(const std::string &path);
  void save_file(const std::string &path);
  void reset_simulator();
  void resize_rows();
  void load_textures();
  void draw_texture_controls();
  const TextureLibrary *textures_for_view() const;

  // Spec draft
  std::array<int, 3> size_{4, 3, 3};
  std::vector<PortDraft> inputs_;
  std::vector<PortDraft> outputs_;
  std::vector<RowDraft> rows_;
  int timing_mode_ = 0; // 0 none, 1 deadline, 2 window
  bool redstone_ticks_ = false;
  int earliest_ = 0;
  int latest_ = 4;
  bool hold_previous_ = false;
  int settle_ = 4;
  int horizon_ = 20;
  bool feedback_ = false;
  std::array<bool, 3> tiled_{};
  std::vector<std::pair<BlockType, bool>> palette_;
  std::string path_ = "examples/and_gate.txt";
  std::string spec_message_;

  // Solving
  float time_limit_ = 60.0f;
  int threads_ = 0;
  bool first_only_ = false;
  std::unique_ptr<SolveJob> job_;

  // Results
  std::optional<Spec> result_spec_;
  std::optional<SolveReport> result_;
  std::string result_message_;
  std::vector<std::string> log_;
  std::string export_text_;
  std::optional<Simulator> sim_;
  std::vector<int> sim_inputs_;
  bool auto_settle_ = true;
  Camera camera_;
  int top_layer_ = 64;
  int layer_ = 1;
  std::string requested_tab_;
  TextureLibrary textures_;
  std::string jar_path_;
  std::string texture_message_;
  bool use_textures_ = true;
};

} // namespace rs::gui

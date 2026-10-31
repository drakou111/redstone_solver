#include "gui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "redstone/report.hpp"

namespace rs::gui {

namespace {

constexpr int kMaxVolumeSide = 16;
constexpr int kMaxPorts = 8;
constexpr int kMaxTicks = 200;
constexpr int kSettleTicks = 400;
constexpr float kSpecPanelWidth = 470.0f;
constexpr const char *kFacings = "down\0up\0north\0south\0west\0east\0";
constexpr const char *kExamplesDir = "examples";
const ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
const ImVec4 kOkColor{0.5f, 0.9f, 0.5f, 1.0f};

constexpr const char *kFallbackSpec = R"(size 4 3 3
input IN1 strong 0 1 0 east
input IN2 strong 0 1 2 east
output OUT1 dust 3 1 1
timing 4
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
)";

std::string read_file(const std::string &path) {
  std::ifstream file(std::filesystem::path(path), std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open " + path);
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

std::vector<std::string> example_files() {
  std::vector<std::string> files;
  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator(kExamplesDir, error))
    if (entry.path().extension() == ".txt")
      files.push_back(entry.path().generic_string());
  std::sort(files.begin(), files.end());
  return files;
}

void help_marker(const char *text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", text);
}

} // namespace

App::App(const std::string &spec_path, bool solve_now) {
  if (!spec_path.empty())
    this->path_ = spec_path;
  try {
    this->load_file(this->path_);
  } catch (const std::exception &error) {
    this->load_spec(parse_spec(kFallbackSpec));
    this->spec_message_ = std::format(
        "error: {}; loaded a built-in AND gate instead", error.what());
  }
  if (std::optional<std::filesystem::path> jar =
          TextureLibrary::find_default_jar()) {
    this->jar_path_ = jar->string();
    this->load_textures();
  } else {
    this->texture_message_ = "no Minecraft install found; pick a client jar";
  }
  if (solve_now)
    this->start_solve();
}

void App::load_textures() {
  try {
    this->textures_.load_from_jar(this->jar_path_);
    this->texture_message_ = "textures from " + this->jar_path_;
  } catch (const std::exception &error) {
    this->texture_message_ = std::string("error: ") + error.what();
  }
}

const TextureLibrary *App::textures_for_view() const {
  return this->use_textures_ && this->textures_.loaded() ? &this->textures_
                                                         : nullptr;
}

void App::draw_texture_controls() {
  ImGui::Checkbox("Minecraft textures", &this->use_textures_);
  ImGui::SameLine();
  if (ImGui::SmallButton("jar..."))
    ImGui::OpenPopup("jar");
  if (ImGui::BeginPopup("jar")) {
    ImGui::TextUnformatted("Minecraft client jar (textures are read from your "
                           "own install, never copied):");
    ImGui::SetNextItemWidth(600);
    ImGui::InputText("##jar", &this->jar_path_);
    ImGui::SameLine();
    if (ImGui::Button("Load"))
      this->load_textures();
    if (!this->texture_message_.empty())
      ImGui::TextWrapped("%s", this->texture_message_.c_str());
    ImGui::EndPopup();
  }
}

// --------------------------------------------------------------- spec I/O

void App::load_spec(const Spec &spec) {
  this->size_ = {spec.size.x, spec.size.y, spec.size.z};
  // The draft stores InputKind / OutputKind as their underlying values.
  auto ports = [](const auto &list) {
    std::vector<PortDraft> drafts;
    for (const auto &port : list) {
      PortDraft draft;
      draft.kind = static_cast<int>(port.kind);
      draft.position = {port.position.x, port.position.y, port.position.z};
      draft.facing = index_of(port.facing);
      drafts.push_back(draft);
    }
    return drafts;
  };
  this->inputs_ = ports(spec.inputs);
  this->outputs_ = ports(spec.outputs);

  this->rows_.clear();
  for (const TruthRow &row : spec.rows) {
    RowDraft draft;
    for (SignalRange r : row.inputs)
      draft.inputs.push_back(short_range_text(r));
    for (SignalRange r : row.outputs)
      draft.outputs.push_back(short_range_text(r));
    this->rows_.push_back(std::move(draft));
  }

  const TimingSpec &t = spec.timing;
  this->redstone_ticks_ = false;
  this->timing_mode_ = t.earliest ? 2 : (t.latest ? 1 : 0);
  this->earliest_ = t.earliest.value_or(0);
  this->latest_ = t.latest.value_or(4);
  this->hold_previous_ = t.hold_previous;
  this->settle_ = t.settle_ticks;
  this->horizon_ = t.horizon_ticks;
  this->feedback_ = spec.allow_feedback;
  this->tiled_ = spec.tiled;

  this->palette_.clear();
  for (BlockType type : default_palette())
    this->palette_.emplace_back(type, spec.palette_contains(type));
}

Spec App::build_spec() const {
  Spec spec;
  spec.size = {this->size_[0], this->size_[1], this->size_[2]};
  for (const PortDraft &d : this->inputs_)
    spec.inputs.push_back({static_cast<InputKind>(d.kind),
                           {d.position[0], d.position[1], d.position[2]},
                           static_cast<Dir>(d.facing)});
  for (const PortDraft &d : this->outputs_)
    spec.outputs.push_back({static_cast<OutputKind>(d.kind),
                            {d.position[0], d.position[1], d.position[2]},
                            static_cast<Dir>(d.facing)});

  for (std::size_t r = 0; r < this->rows_.size(); ++r) {
    TruthRow row;
    try {
      for (const std::string &text : this->rows_[r].inputs)
        row.inputs.push_back(parse_signal_range(text));
      for (const std::string &text : this->rows_[r].outputs)
        row.outputs.push_back(parse_signal_range(text));
    } catch (const std::invalid_argument &error) {
      throw std::invalid_argument(
          std::format("truth table row {}: {}", r + 1, error.what()));
    }
    spec.rows.push_back(std::move(row));
  }

  int scale = this->redstone_ticks_ ? kGameTicksPerRedstoneTick : 1;
  if (this->timing_mode_ >= 1)
    spec.timing.latest = this->latest_ * scale;
  if (this->timing_mode_ == 2)
    spec.timing.earliest = this->earliest_ * scale;
  spec.timing.hold_previous = this->timing_mode_ == 2 && this->hold_previous_;
  spec.timing.settle_ticks = this->settle_ * scale;
  spec.timing.horizon_ticks = this->horizon_ * scale;
  spec.allow_feedback = this->feedback_;
  spec.tiled = this->tiled_;
  for (auto [type, enabled] : this->palette_)
    if (enabled)
      spec.palette.push_back(type);
  spec.validate();
  return spec;
}

void App::load_file(const std::string &path) {
  this->load_spec(parse_spec(read_file(path)));
  this->spec_message_ = "loaded " + path;
}

void App::save_file(const std::string &path) {
  std::string text = format_spec(this->build_spec());
  std::ofstream file(std::filesystem::path(path), std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot write " + path);
  file << text;
  this->spec_message_ = "saved " + path;
}

void App::resize_rows() {
  for (RowDraft &row : this->rows_) {
    row.inputs.resize(this->inputs_.size(), "*");
    row.outputs.resize(this->outputs_.size(), "*");
  }
}

// ------------------------------------------------------------------- draw

void App::draw() {
  this->poll_job();
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("Redstone Solver", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  float width = kSpecPanelWidth * ImGui::GetFontSize() / 13.0f;
  ImGui::BeginChild("spec", {width, 0}, ImGuiChildFlags_Borders);
  this->draw_spec_panel();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("result", {0, 0}, ImGuiChildFlags_Borders);
  this->draw_solve_bar();
  ImGui::Separator();
  this->draw_result_area();
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_spec_panel() {
  this->draw_file_section();

  if (ImGui::CollapsingHeader("Volume", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::InputInt3("size x y z", this->size_.data()))
      for (int &side : this->size_)
        side = std::clamp(side, 1, kMaxVolumeSide);
    ImGui::Text("tile along:");
    for (int axis = 0; axis < 3; ++axis) {
      ImGui::SameLine();
      std::string label =
          std::format("{}##tile", static_cast<char>('x' + axis));
      ImGui::Checkbox(label.c_str(), &this->tiled_[axis]);
    }
    help_marker("Copies placed side by side along these axes must not "
                "influence each other.");
  }
  if (ImGui::CollapsingHeader("Inputs", ImGuiTreeNodeFlags_DefaultOpen))
    this->draw_ports(this->inputs_, true);
  if (ImGui::CollapsingHeader("Outputs", ImGuiTreeNodeFlags_DefaultOpen))
    this->draw_ports(this->outputs_, false);
  if (ImGui::CollapsingHeader("Truth table", ImGuiTreeNodeFlags_DefaultOpen))
    this->draw_truth_table();
  if (ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen))
    this->draw_timing();
  if (ImGui::CollapsingHeader("Allowed blocks", ImGuiTreeNodeFlags_DefaultOpen))
    this->draw_options();
}

void App::draw_file_section() {
  ImGui::SetNextItemWidth(-160);
  ImGui::InputText("##path", &this->path_);
  ImGui::SameLine();
  if (ImGui::Button("Load")) {
    try {
      this->load_file(this->path_);
    } catch (const std::exception &error) {
      this->spec_message_ = std::string("error: ") + error.what();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    try {
      this->save_file(this->path_);
    } catch (const std::exception &error) {
      this->spec_message_ = std::string("error: ") + error.what();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Examples"))
    ImGui::OpenPopup("examples");
  if (ImGui::BeginPopup("examples")) {
    std::vector<std::string> files = example_files();
    if (files.empty())
      ImGui::TextDisabled("no examples folder next to the program");
    for (const std::string &file : files) {
      if (ImGui::Selectable(file.c_str())) {
        this->path_ = file;
        try {
          this->load_file(file);
        } catch (const std::exception &error) {
          this->spec_message_ = std::string("error: ") + error.what();
        }
      }
    }
    ImGui::EndPopup();
  }
  if (!this->spec_message_.empty()) {
    bool error = this->spec_message_.starts_with("error");
    ImGui::PushStyleColor(ImGuiCol_Text, error ? kErrorColor : kOkColor);
    ImGui::TextWrapped("%s", this->spec_message_.c_str());
    ImGui::PopStyleColor();
  }
}

void App::draw_ports(std::vector<PortDraft> &ports, bool inputs) {
  const char *prefix = inputs ? "IN" : "OUT";
  // Combo items follow the enum values.
  const char *kinds = inputs ? "comparator\0container\0repeater\0dust\0"
                             : "dust\0repeater\0comparator\0";
  int remove = -1;
  for (std::size_t i = 0; i < ports.size(); ++i) {
    PortDraft &port = ports[i];
    ImGui::PushID(static_cast<int>(i) + (inputs ? 0 : 1000));
    ImGui::Text("%s%zu", prefix, i + 1);
    ImGui::SameLine(50);
    ImGui::SetNextItemWidth(95);
    ImGui::Combo("##kind", &port.kind, kinds);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ImGui::InputInt3("##pos", port.position.data());
    bool directional =
        inputs ? (static_cast<InputKind>(port.kind) == InputKind::Comparator ||
                  static_cast<InputKind>(port.kind) == InputKind::Repeater)
               : static_cast<OutputKind>(port.kind) != OutputKind::Dust;
    if (directional) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70);
      ImGui::Combo("##facing", &port.facing, kFacings);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x"))
      remove = static_cast<int>(i);
    ImGui::PopID();
  }
  if (remove >= 0) {
    ports.erase(ports.begin() + remove);
    for (RowDraft &row : this->rows_) {
      auto &cells = inputs ? row.inputs : row.outputs;
      if (remove < static_cast<int>(cells.size()))
        cells.erase(cells.begin() + remove);
    }
  }
  if (ports.size() < kMaxPorts &&
      ImGui::SmallButton(inputs ? "+ input" : "+ output")) {
    ports.push_back(PortDraft{});
    this->resize_rows();
  }
  ImGui::SameLine();
  ImGui::TextDisabled(inputs ? "each behaves like that block driven to the "
                               "input strength"
                             : "the output is what that block would receive");
}

void App::draw_truth_table() {
  ImGui::TextDisabled("cells: * = anything, 0, 5, 1..15");
  int columns =
      static_cast<int>(this->inputs_.size() + this->outputs_.size()) + 1;
  int remove = -1;
  if (ImGui::BeginTable("rows", columns,
                        ImGuiTableFlags_Borders |
                            ImGuiTableFlags_SizingFixedFit)) {
    for (std::size_t i = 0; i < this->inputs_.size(); ++i)
      ImGui::TableSetupColumn(std::format("IN{}", i + 1).c_str());
    for (std::size_t o = 0; o < this->outputs_.size(); ++o)
      ImGui::TableSetupColumn(std::format("OUT{}", o + 1).c_str());
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (std::size_t r = 0; r < this->rows_.size(); ++r) {
      RowDraft &row = this->rows_[r];
      ImGui::TableNextRow();
      ImGui::PushID(static_cast<int>(r));
      auto cell = [](std::string &text, int id) {
        ImGui::TableNextColumn();
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(55);
        ImGui::InputText("##cell", &text);
        ImGui::PopID();
      };
      for (std::size_t i = 0; i < row.inputs.size(); ++i)
        cell(row.inputs[i], static_cast<int>(i));
      for (std::size_t o = 0; o < row.outputs.size(); ++o)
        cell(row.outputs[o], 100 + static_cast<int>(o));
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x"))
        remove = static_cast<int>(r);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (remove >= 0)
    this->rows_.erase(this->rows_.begin() + remove);
  if (ImGui::SmallButton("+ row")) {
    this->rows_.push_back(RowDraft{});
    this->resize_rows();
  }
}

void App::draw_timing() {
  ImGui::RadioButton("no deadline", &this->timing_mode_, 0);
  ImGui::SameLine();
  ImGui::RadioButton("deadline", &this->timing_mode_, 1);
  ImGui::SameLine();
  ImGui::RadioButton("window", &this->timing_mode_, 2);

  int unit = this->redstone_ticks_ ? 1 : 0;
  ImGui::SetNextItemWidth(160);
  if (ImGui::Combo("unit", &unit, "game ticks (gt)\0redstone ticks (rt)\0")) {
    bool to_redstone = unit == 1;
    if (to_redstone != this->redstone_ticks_) {
      // Keep the same real durations when switching units.
      auto convert = [&](int &value) {
        value = to_redstone ? (value + 1) / kGameTicksPerRedstoneTick
                            : value * kGameTicksPerRedstoneTick;
      };
      for (int *value :
           {&this->earliest_, &this->latest_, &this->settle_, &this->horizon_})
        convert(*value);
      this->redstone_ticks_ = to_redstone;
    }
  }

  auto ticks = [](const char *label, int &value) {
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt(label, &value))
      value = std::clamp(value, 0, kMaxTicks);
  };
  if (this->timing_mode_ == 2)
    ticks("earliest", this->earliest_);
  if (this->timing_mode_ >= 1)
    ticks("latest (outputs correct from here)", this->latest_);
  if (this->timing_mode_ == 2) {
    ImGui::Checkbox("hold previous outputs until earliest",
                    &this->hold_previous_);
    help_marker("Off: outputs may do anything before the deadline.\n"
                "On: outputs keep their old value until 'earliest'.");
  }
  if (this->timing_mode_ >= 1)
    ticks("settle (extra time to come to rest)", this->settle_);
  else
    ticks("horizon (time to come to rest)", this->horizon_);
}

void App::draw_options() {
  ImGui::TextDisabled("untick a block to keep the solver from using it");
  ImGui::SameLine();
  if (ImGui::SmallButton("all"))
    for (auto &entry : this->palette_)
      entry.second = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("none"))
    for (auto &entry : this->palette_)
      entry.second = false;
  int column = 0;
  for (auto &[type, enabled] : this->palette_) {
    if (column++ % 3 != 0)
      ImGui::SameLine(10 +
                      (column - 1) % 3 * 140.0f * ImGui::GetFontSize() / 13.0f);
    ImGui::Checkbox(std::string(palette_name(type)).c_str(), &enabled);
  }
  ImGui::Checkbox("allow feedback loops and repeater locks", &this->feedback_);
  help_marker("Loops and locked repeaters act as memory. Truth tables do not "
              "need them, and allowing them makes solving much harder.");
}

// ------------------------------------------------------------------ solve

void App::start_solve() {
  try {
    Spec spec = this->build_spec();
    SolverOptions options;
    options.time_limit_seconds = this->time_limit_;
    options.threads = this->threads_;
    options.stop_at_first = this->first_only_;
    this->job_ = std::make_unique<SolveJob>(spec, options);
    this->result_message_.clear();
    this->spec_message_.clear();
  } catch (const std::exception &error) {
    this->spec_message_ = std::string("error: ") + error.what();
  }
}

void App::poll_job() {
  if (!this->job_)
    return;
  this->log_ = this->job_->log();
  if (!this->job_->is_done())
    return;

  if (auto error = this->job_->error()) {
    this->result_message_ = "error: " + *error;
  } else if (auto report = this->job_->report()) {
    this->result_message_ = std::format(
        "{} after {:.1f}s", to_string(report->status), report->seconds);
    if (report->solution) {
      this->result_ = std::move(report);
      this->result_spec_ = this->job_->spec();
      this->sim_inputs_.assign(this->result_spec_->inputs.size(), 0);
      this->export_text_ = setblock_commands(this->result_->solution->layout);
      this->top_layer_ = this->result_spec_->size.y - 1;
      this->layer_ = std::min(this->layer_, this->top_layer_);
      this->reset_simulator();
      this->result_message_ +=
          std::format(", {} blocks", this->result_->solution->block_count);
    }
  }
  this->job_.reset();
}

void App::draw_solve_bar() {
  bool running = this->job_ != nullptr;
  ImGui::BeginDisabled(running);
  ImGui::SetNextItemWidth(160);
  ImGui::SliderFloat("time limit (s)", &this->time_limit_, 5.0f, 600.0f, "%.0f",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine();
  ImGui::Checkbox("stop at first circuit", &this->first_only_);
  help_marker("Faster: stops at the first circuit that passes the simulator, "
              "then removes unneeded blocks.\nOff: keeps searching for the "
              "fewest blocks until the time limit.");
  ImGui::SameLine();
  if (ImGui::Button("Solve", {90, 0}))
    this->start_solve();
  ImGui::EndDisabled();

  if (running) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      this->job_->cancel();
    ImGui::SameLine();
    ImGui::Text("solving... %.0fs", this->job_->seconds());
    if (!this->log_.empty())
      ImGui::TextDisabled("%s", this->log_.back().c_str());
  } else if (!this->result_message_.empty()) {
    bool good = this->result_message_.starts_with("optimal") ||
                this->result_message_.starts_with("feasible");
    ImGui::PushStyleColor(ImGuiCol_Text, good ? kOkColor : kErrorColor);
    ImGui::TextWrapped("%s", this->result_message_.c_str());
    ImGui::PopStyleColor();
  }
}

// ---------------------------------------------------------------- results

void App::reset_simulator() {
  if (!this->result_)
    return;
  this->sim_.emplace(this->result_->solution->layout);
  this->sim_->run_until_quiet(kSettleTicks);
  for (std::size_t i = 0; i < this->sim_inputs_.size(); ++i)
    this->sim_->set_input(static_cast<int>(i), this->sim_inputs_[i]);
  this->sim_->run_until_quiet(kSettleTicks);
}

void App::draw_playground() {
  ImGui::Text("Simulator  t = %lld",
              static_cast<long long>(this->sim_->time()));
  ImGui::SameLine();
  if (ImGui::Button("Tick"))
    this->sim_->tick();
  ImGui::SameLine();
  if (ImGui::Button("Tick x10"))
    for (int i = 0; i < 10; ++i)
      this->sim_->tick();
  ImGui::SameLine();
  if (ImGui::Button("Settle"))
    this->sim_->run_until_quiet(kSettleTicks);
  ImGui::SameLine();
  if (ImGui::Button("Reset"))
    this->reset_simulator();
  ImGui::SameLine();
  ImGui::Checkbox("settle after input change", &this->auto_settle_);
  if (this->sim_->has_pending_ticks()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(ticks pending)");
  }

  for (std::size_t i = 0; i < this->sim_inputs_.size(); ++i) {
    ImGui::SetNextItemWidth(160);
    std::string label = std::format("IN{}", i + 1);
    if (ImGui::SliderInt(label.c_str(), &this->sim_inputs_[i], 0, kMaxSignal)) {
      this->sim_->set_input(static_cast<int>(i), this->sim_inputs_[i]);
      if (this->auto_settle_)
        this->sim_->run_until_quiet(kSettleTicks);
    }
    ImGui::SameLine();
  }
  for (std::size_t o = 0; o < this->result_spec_->outputs.size(); ++o) {
    ImGui::SameLine();
    ImGui::Text("  OUT%zu = %d", o + 1,
                this->sim_->output(static_cast<int>(o)));
  }
}

void App::draw_timelines() {
  const EncodedSolution &solution = *this->result_->solution;
  if (solution.traces.empty()) {
    ImGui::TextWrapped(
        "The model needed no transitions for this circuit (the simulator "
        "checked all of them). Use the simulator above to step through "
        "input changes tick by tick.");
    return;
  }
  ImGui::TextDisabled("Model output per game tick after the input change:");
  for (const TransitionTrace &trace : solution.traces) {
    std::string line = std::format(
        "{} -> {}:", to_string(solution.steady_inputs[trace.transition.from]),
        to_string(solution.steady_inputs[trace.transition.to]));
    for (std::size_t o = 0; o < this->result_spec_->outputs.size(); ++o) {
      line += std::format("  OUT{}", o + 1);
      for (const std::vector<int> &tick : trace.outputs)
        line += std::format(" {}", tick[o]);
    }
    ImGui::TextUnformatted(line.c_str());
  }
}

void App::draw_result_area() {
  const World *world = nullptr;
  std::optional<World> preview;
  if (this->result_) {
    world = &this->result_->solution->layout;
    this->draw_playground();
    ImGui::Separator();
  } else {
    try {
      preview = this->build_spec().base_world();
      world = &*preview;
      ImGui::TextDisabled(
          "Preview of the ports. Press Solve to find a circuit.");
    } catch (const std::exception &error) {
      ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
      ImGui::TextWrapped("Spec problem: %s", error.what());
      ImGui::PopStyleColor();
    }
  }
  const Simulator *sim = this->result_ ? &*this->sim_ : nullptr;

  if (!ImGui::BeginTabBar("views"))
    return;
  auto tab_flags = [&](const char *name) {
    return this->requested_tab_ == name ? ImGuiTabItemFlags_SetSelected
                                        : ImGuiTabItemFlags_None;
  };
  if (world && ImGui::BeginTabItem("3D view", nullptr, tab_flags("3D view"))) {
    int top = world->size().y - 1;
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("show layers up to y", &this->top_layer_, 0, top);
    ImGui::SameLine();
    this->draw_texture_controls();
    ImGui::SameLine();
    ImGui::TextDisabled("drag to rotate, wheel to zoom");
    draw_world_3d("world3d", *world, sim, this->textures_for_view(),
                  this->camera_, std::min(this->top_layer_, top),
                  ImGui::GetContentRegionAvail());
    ImGui::EndTabItem();
  }
  if (world && ImGui::BeginTabItem("Layers", nullptr, tab_flags("Layers"))) {
    int top = world->size().y - 1;
    this->layer_ = std::clamp(this->layer_, 0, top);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("layer y", &this->layer_, 0, top);
    ImGui::SameLine();
    ImGui::TextDisabled("north at the top, east to the right; hover a cell");
    draw_layer("layer", *world, sim, this->textures_for_view(), this->layer_);
    ImGui::EndTabItem();
  }
  if (this->result_ &&
      ImGui::BeginTabItem("Blocks", nullptr, tab_flags("Blocks"))) {
    ImGui::TextUnformatted(
        list_blocks(this->result_->solution->layout).c_str());
    ImGui::EndTabItem();
  }
  if (this->result_ &&
      ImGui::BeginTabItem("Timelines", nullptr, tab_flags("Timelines"))) {
    this->draw_timelines();
    ImGui::EndTabItem();
  }
  if (this->result_ &&
      ImGui::BeginTabItem("Export", nullptr, tab_flags("Export"))) {
    if (ImGui::Button("Copy to clipboard"))
      ImGui::SetClipboardText(this->export_text_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(
        "relative /setblock commands, origin at the lowest corner");
    ImGui::InputTextMultiline("##export", &this->export_text_, {-1, -1},
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Log", nullptr, tab_flags("Log"))) {
    for (const std::string &line : this->log_)
      ImGui::TextWrapped("%s", line.c_str());
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
  if (this->result_ || this->requested_tab_ == "3D view" ||
      this->requested_tab_ == "Layers" || this->requested_tab_ == "Log")
    this->requested_tab_.clear();
}

} // namespace rs::gui

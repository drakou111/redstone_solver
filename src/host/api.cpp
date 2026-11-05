#include "host/api.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "host/minecraft_assets.hpp"
#include "redstone/compose.hpp"
#include "redstone/layout_check.hpp"
#include "redstone/report.hpp"
#include "redstone/shrink.hpp"

namespace rs::host {

namespace {

constexpr int kSettleTicks = 400;
constexpr int kMaxTicksPerRequest = 10'000;
constexpr const char *kDefaultExample = "and_gate.txt";

std::string read_text(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot open " + path.string());
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

std::string string_field(const Json &body, const char *name) {
  if (!body.contains(name) || !body.at(name).is_string())
    throw std::invalid_argument(std::format("'{}' must be a string", name));
  return body.at(name).get<std::string>();
}

int int_field(const Json &body, const char *name) {
  if (!body.contains(name) || !body.at(name).is_number_integer())
    throw std::invalid_argument(std::format("'{}' must be an integer", name));
  return body.at(name).get<int>();
}

// Number of consecutive input (or output) channels starting at 0.
int count_ports(const World &world, bool inputs) {
  int count = 0;
  while (inputs ? world.find_input(count).has_value()
                : world.find_output(count).has_value())
    ++count;
  return count;
}

} // namespace

Api::Api(std::filesystem::path examples_dir)
    : examples_dir_(std::move(examples_dir)) {}

Json Api::handle(std::string_view route, const Json &body) {
  std::lock_guard lock(this->mutex_);
  if (route == "session")
    return this->session();
  if (route == "spec/load")
    return this->load_spec(body);
  if (route == "spec/save")
    return this->save_spec(body);
  if (route == "spec/check_sketch")
    return this->check_sketch(body);
  if (route == "spec/check")
    return this->check_spec(body);
  if (route == "compose/start")
    return this->start_compose(body);
  if (route == "shrink/start")
    return this->start_shrink(body);
  if (route == "solve/start")
    return this->start_solve(body);
  if (route == "solve/status")
    return this->solve_status();
  if (route == "solve/cancel") {
    if (this->job_)
      this->job_->cancel();
    return Json::object();
  }
  if (route == "sim/reset")
    return this->reset_simulator(body);
  if (route == "sim/input")
    return this->simulator_input(body);
  if (route == "sim/tick")
    return this->simulator_tick(body);
  if (route == "sim/settle")
    return this->simulator_settle();
  if (route == "sim/state")
    return this->simulator_state();
  if (route == "verify")
    return this->verify(body);
  if (route == "export")
    return this->export_commands(body);
  if (route == "textures")
    return this->textures(body);
  throw std::invalid_argument(std::format("unknown request '{}'", route));
}

Json Api::session() const {
  Json examples = Json::array();
  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator(this->examples_dir_, error))
    if (entry.path().extension() == ".txt")
      examples.push_back(entry.path().string());
  std::sort(examples.begin(), examples.end());
  std::optional<std::filesystem::path> jar = find_minecraft_jar();
  return {{"examples", examples},
          {"defaultPath", (this->examples_dir_ / kDefaultExample).string()},
          {"jar", jar ? Json(jar->string()) : Json(nullptr)},
          {"palette", [] {
             Json names = Json::array();
             for (BlockType type : default_palette())
               names.push_back(palette_name(type));
             return names;
           }()}};
}

Json Api::load_spec(const Json &body) const {
  std::string path = string_field(body, "path");
  Spec spec = parse_spec(read_text(path));
  return {{"spec", to_json(spec)}, {"path", path}};
}

Json Api::save_spec(const Json &body) const {
  std::string path = string_field(body, "path");
  Spec spec = spec_from_json(body.at("spec"));
  std::ofstream file(std::filesystem::path(path), std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot write " + path);
  file << format_spec(spec);
  return {{"path", path}};
}

Json Api::check_spec(const Json &body) const {
  Spec spec = spec_from_json(body.at("spec"));
  return {{"world", to_json(spec.base_world())}};
}

namespace {

// "Only as drawn": verify the layout at the identity placement alone.
VerificationOptions verification_from(const Json &settings) {
  VerificationOptions options;
  if (settings.value("onePlacement", false))
    options.placements = {Placement{}};
  return options;
}

} // namespace

Json Api::check_sketch(const Json &body) const {
  Spec spec = spec_from_json(body.at("spec"));
  LayoutCheck check =
      check_layout(layout_from_hints(spec), spec, verification_from(body));
  return {{"passes", check.passes()}, {"report", format_check(check, spec)}};
}

Json Api::start_solve(const Json &body) {
  if (this->job_ && !this->job_->is_done())
    throw std::invalid_argument("a solve is already running");
  Spec spec = spec_from_json(body.at("spec"));
  SolverOptions options;
  const Json &settings = body.value("options", Json::object());
  options.time_limit_seconds = settings.value("timeLimit", 60.0);
  options.stop_at_first = settings.value("first", false);
  options.sketch_step_seconds = settings.value("sketchTime", 3.0);
  options.verification = verification_from(settings);
  if (options.sketch_step_seconds <= 0)
    throw std::invalid_argument("the sketch step time must be positive");
  if (options.time_limit_seconds <= 0)
    throw std::invalid_argument("the time limit must be positive");
  this->job_ = std::make_unique<SolveJob>(std::move(spec), options);
  return Json::object();
}

// Shrinks a solved layout: the job ends with the smaller spec (ports where
// the layout has them, the layout as a sketch) and its verified layout.
Json Api::start_shrink(const Json &body) {
  if (this->job_ && !this->job_->is_done())
    throw std::invalid_argument("a solve is already running");
  Spec spec = spec_from_json(body.at("spec"));
  World layout = world_from_json(body.at("world"));
  const Json &settings = body.value("options", Json::object());
  ShrinkOptions options;
  options.time_limit_seconds = settings.value("timeLimit", 600.0);
  options.cut_seconds = settings.value("cutTime", 120.0);
  options.solver.sketch_step_seconds = settings.value("sketchTime", 3.0);
  options.solver.verification = verification_from(settings);
  if (options.time_limit_seconds <= 0 || options.cut_seconds <= 0)
    throw std::invalid_argument("time limits must be positive");
  this->job_ = std::make_unique<SolveJob>(
      spec, [spec, layout, options](const ProgressCallback &progress,
                                    std::atomic<bool> *cancel) {
        auto start = std::chrono::steady_clock::now();
        ShrinkOptions o = options;
        o.cancel = cancel;
        ShrinkResult shrunk = shrink(spec, layout, o, progress);
        int blocks = 0;
        for (int i = 0; i < shrunk.layout.volume(); ++i) {
          BlockType type = shrunk.layout.at(shrunk.layout.position_of(i)).type;
          if (type != BlockType::Air && type != BlockType::Input &&
              type != BlockType::Output)
            ++blocks;
        }
        SolveReport report;
        report.status = SolveStatus::Feasible;
        report.solution = EncodedSolution{shrunk.layout, blocks, {}, {}, {}};
        report.seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        return JobResult{std::move(report), std::move(shrunk.spec)};
      });
  return Json::object();
}

// Composes the spec's on/off table from gate modules; the job ends with the
// composed spec (its own box and ports, the table's rows, the circuit as a
// sketch).
Json Api::start_compose(const Json &body) {
  if (this->job_ && !this->job_->is_done())
    throw std::invalid_argument("a solve is already running");
  const Json &settings = body.value("options", Json::object());
  NetlistComposeOptions options;
  options.stage_seconds = settings.value("timeLimit", 120.0);
  options.gates.seconds_per_size = options.stage_seconds;
  options.solver.verification = verification_from(settings);
  if (options.stage_seconds <= 0)
    throw std::invalid_argument("the time limit must be positive");
  Spec spec = spec_from_json(body.at("spec"));
  // Answered right away: a table that needs exact strengths cannot be
  // composed from on/off gates.
  boolean_table(spec);
  this->job_ = std::make_unique<SolveJob>(
      spec, [spec, options](const ProgressCallback &progress,
                            std::atomic<bool> *cancel) {
        auto start = std::chrono::steady_clock::now();
        NetlistComposeOptions o = options;
        o.cancel = cancel;
        std::optional<ComposeResult> composed = compose_spec(spec, o, progress);
        SolveReport report;
        report.seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        if (!composed) {
          report.status =
              cancel->load() ? SolveStatus::Cancelled : SolveStatus::OutOfTime;
          return JobResult{std::move(report), std::nullopt};
        }
        report.status = SolveStatus::Feasible;
        report.solution =
            EncodedSolution{composed->layout,
                            static_cast<int>(composed->spec.hints.size()),
                            {},
                            {},
                            {}};
        return JobResult{std::move(report), std::move(composed->spec)};
      });
  return Json::object();
}

Json Api::solve_status() const {
  if (!this->job_)
    return {{"running", false}, {"log", Json::array()}};
  Json status{{"running", !this->job_->is_done()},
              {"seconds", this->job_->seconds()},
              {"log", this->job_->log()}};
  if (auto error = this->job_->error())
    status["error"] = *error;
  if (auto report = this->job_->report())
    status["result"] = to_json(*report);
  if (auto spec = this->job_->result_spec())
    status["spec"] = to_json(*spec);
  return status;
}

Simulator &Api::simulator() {
  if (!this->sim_)
    throw std::invalid_argument("no circuit loaded in the simulator");
  return *this->sim_;
}

const Simulator &Api::simulator() const {
  if (!this->sim_)
    throw std::invalid_argument("no circuit loaded in the simulator");
  return *this->sim_;
}

Json Api::reset_simulator(const Json &body) {
  World world = world_from_json(body.at("world"));
  this->sim_.emplace(world);
  this->sim_inputs_ = count_ports(world, true);
  this->sim_outputs_ = count_ports(world, false);
  this->sim_->run_until_quiet(kSettleTicks);
  if (body.contains("inputs")) {
    const Json &inputs = body.at("inputs");
    for (int i = 0;
         i < this->sim_inputs_ && i < static_cast<int>(inputs.size()); ++i)
      this->sim_->set_input(i, inputs[i].get<int>());
    this->sim_->run_until_quiet(kSettleTicks);
  }
  return this->simulator_state();
}

Json Api::simulator_input(const Json &body) {
  Simulator &sim = this->simulator();
  sim.set_input(int_field(body, "channel"), int_field(body, "value"));
  if (body.value("settle", false))
    sim.run_until_quiet(kSettleTicks);
  return this->simulator_state();
}

Json Api::simulator_tick(const Json &body) {
  int count = std::clamp(body.value("count", 1), 1, kMaxTicksPerRequest);
  Simulator &sim = this->simulator();
  for (int i = 0; i < count; ++i)
    sim.tick();
  return this->simulator_state();
}

Json Api::simulator_settle() {
  this->simulator().run_until_quiet(kSettleTicks);
  return this->simulator_state();
}

Json Api::simulator_state() const {
  return simulation_json(this->simulator(), this->sim_inputs_,
                         this->sim_outputs_);
}

Json Api::verify(const Json &body) const {
  Spec spec = spec_from_json(body.at("spec"));
  World world = world_from_json(body.at("world"));
  std::optional<VerificationFailure> failure = verify_layout(
      world, spec, spec.verification_vectors(), verification_from(body));
  if (!failure)
    return {{"ok", true}};
  return {{"ok", false}, {"failure", to_json(*failure)}};
}

Json Api::export_commands(const Json &body) const {
  return {{"text", setblock_commands(world_from_json(body.at("world")))}};
}

Json Api::textures(const Json &body) {
  std::optional<std::filesystem::path> jar;
  if (body.contains("jar") && body.at("jar").is_string())
    jar = body.at("jar").get<std::string>();
  else
    jar = find_minecraft_jar();
  if (!jar)
    throw std::invalid_argument("no Minecraft install found; choose a jar");
  try {
    return {{"jar", jar->string()}, {"textures", load_block_textures(*jar)}};
  } catch (const std::runtime_error &error) {
    throw std::invalid_argument(error.what());
  }
}

} // namespace rs::host

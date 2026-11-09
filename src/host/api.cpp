#include "host/api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "host/catalog_file.hpp"
#include "host/minecraft_assets.hpp"
#include "redstone/compose.hpp"
#include "redstone/layout_check.hpp"
#include "redstone/report.hpp"
#include "redstone/shrink.hpp"

namespace rs::host {

namespace {

// Gate modules are 2 high and a wire crosses another 2 layers up.
constexpr int kCrossingHeight = 4;
constexpr const char *kCatalogFileName = "gate_catalog.json";

// Search the box: extra searches, each with its seed (they need memory and
// a core or two each).
constexpr int kThreadsPerExtraSearch = 3;
constexpr int kMaxExtraSearches = 7;

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

Api::Api(std::filesystem::path examples_dir,
         std::optional<std::filesystem::path> library_dir)
    : examples_dir_(std::move(examples_dir)) {
  if (library_dir) {
    this->library_ = std::make_unique<CircuitLibrary>(*library_dir);
    this->catalog_file_ = *library_dir / kCatalogFileName;
    load_catalog(*this->catalog_file_, this->catalog_);
  }
}

namespace {

// What makes two Search-the-box requests the same: the spec without its
// sketch (a sketch only changes where the search starts) and how it is
// checked.
std::string solve_key(const Spec &spec, const SolverOptions &options) {
  Json json = to_json(spec);
  json.erase("hints");
  return std::format("solve|{}|placements {}", json.dump(),
                     options.verification.placements.size());
}

// What a sketch from another request needs to match: the box, ports,
// blocks and fixed cells, whatever the truth table and timing.
std::string solve_shape(const Spec &spec) {
  Json json = to_json(spec);
  for (const char *field : {"hints", "rows", "timing"})
    json.erase(field);
  return "shape|" + json.dump();
}

// Compose places its own ports: the same on/off table, height limit,
// blocks and checking give the same request.
std::string compose_key(const Spec &spec,
                        const NetlistComposeOptions &options) {
  BoolTable table = boolean_table(spec);
  std::string key = std::format("compose|inputs {}|", table.inputs);
  for (const auto &column : table.values) {
    for (const std::optional<bool> &value : column)
      key += !value ? '-' : *value ? '1' : '0';
    key += '|';
  }
  key += std::format("height {}|placements {}|blocks", options.max_height,
                     options.solver.verification.placements.size());
  for (BlockType type : spec.palette)
    key += std::format(" {}", palette_name(type));
  return key;
}

// A job that ends at once with a circuit from the library.
JobWork from_library(const CircuitLibrary::Entry &entry, bool with_spec) {
  return [entry, with_spec](const JobContext &job) {
    const ProgressCallback &progress = job.progress;
    if (progress)
      progress(std::format("from the circuit library: solved before, {} "
                           "blocks",
                           entry.blocks));
    SolveReport report;
    report.status = SolveStatus::Feasible;
    report.solution = EncodedSolution{
        world_from_json(entry.layout), entry.blocks, {}, {}, {}};
    std::optional<Spec> spec;
    if (with_spec)
      spec = spec_from_json(entry.spec);
    return JobResult{std::move(report), std::move(spec)};
  };
}

// The job's work, then its circuit (if any) offered to the library.
JobWork offering(CircuitLibrary *library, std::string key, Json request_spec,
                 std::string shape, JobWork work) {
  return [library, key = std::move(key), request_spec = std::move(request_spec),
          shape = std::move(shape),
          work = std::move(work)](const JobContext &job) {
    const ProgressCallback &progress = job.progress;
    JobResult result = work(job);
    if (library && result.report.solution) {
      const World &layout = result.report.solution->layout;
      Vec3 size = layout.size();
      CircuitLibrary::Entry entry{
          result.spec ? to_json(*result.spec) : request_spec, to_json(layout),
          result.report.solution->block_count, size.x * size.y * size.z, shape};
      if (library->offer(key, entry) && progress)
        progress("kept in the circuit library");
    }
    return result;
  };
}

} // namespace

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
  if (route == "catalog/build")
    return this->start_catalog(body);
  if (route == "solve/status")
    return this->solve_status(body);
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
  if (route == "sim/use")
    return this->simulator_use(body);
  if (route == "result/analyze")
    return this->analyze_result(body);
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

// "Only as drawn" (the default): verify at the identity placement alone.
VerificationOptions verification_from(const Json &settings) {
  VerificationOptions options;
  if (settings.value("onePlacement", true))
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
  // One search alone leaves most cores idle, and how long it takes varies a
  // lot with its seed: several run side by side.
  options.extra_searches = settings.value(
      "extraSearches",
      std::clamp(static_cast<int>(std::thread::hardware_concurrency()) /
                         kThreadsPerExtraSearch -
                     1,
                 0, kMaxExtraSearches));
  if (options.sketch_step_seconds <= 0)
    throw std::invalid_argument("the sketch step time must be positive");
  if (options.time_limit_seconds <= 0)
    throw std::invalid_argument("the time limit must be positive");
  const bool use_library = this->library_ && settings.value("library", true);
  const std::string key = solve_key(spec, options);
  if (use_library)
    if (std::optional<CircuitLibrary::Entry> entry =
            this->library_->find(key)) {
      this->job_ =
          std::make_unique<SolveJob>(spec, from_library(*entry, false));
      return Json{{"fromLibrary", true}};
    }
  // Not asked before: a kept circuit with the same box and ports (another
  // truth table) is a sketch to start from, when the request has none.
  Spec search = spec;
  bool retrieved = false;
  if (use_library && search.hints.empty())
    if (std::optional<CircuitLibrary::Entry> near =
            this->library_->smallest_of_shape(solve_shape(spec))) {
      World layout = world_from_json(near->layout);
      if (layout.size() == search.size) {
        for (int i = 0; i < layout.volume(); ++i) {
          Vec3 p = layout.position_of(i);
          const Block &block = layout.at(p);
          if (block.type != BlockType::Air && block.type != BlockType::Input &&
              block.type != BlockType::Output)
            search.hints.emplace_back(p, block);
        }
        retrieved = !search.hints.empty();
      }
    }
  JobWork work = [spec, search, options, retrieved](const JobContext &job) {
    if (retrieved && job.progress)
      job.progress("starting from a similar circuit in the library (a sketch)");
    SolverOptions o = options;
    o.cancel = job.cancel;
    o.preview = job.preview;
    SolveReport report = solve(search, o, job.progress);
    // Ports the solver could move end where the circuit has them.
    std::optional<Spec> placed;
    const bool free_ports =
        std::any_of(spec.inputs.begin(), spec.inputs.end(),
                    [](const InputPort &p) { return p.is_free(); }) ||
        std::any_of(spec.outputs.begin(), spec.outputs.end(),
                    [](const OutputPort &p) { return p.is_free(); });
    if (free_ports && report.solution) {
      placed = spec_with_layout(spec, report.solution->layout);
      placed->hints.clear();
    }
    return JobResult{std::move(report), std::move(placed)};
  };
  this->job_ = std::make_unique<SolveJob>(
      spec, offering(this->library_.get(), key, to_json(spec),
                     solve_shape(spec), std::move(work)));
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
  // A build of an on/off table is also kept for Build from gates requests of
  // that table in a box this high: a known design answers them at once.
  std::optional<std::string> key;
  if (this->library_) {
    NetlistComposeOptions compose;
    compose.max_height = spec.size.y;
    compose.solver.verification = options.solver.verification;
    try {
      key = compose_key(spec, compose);
    } catch (const std::invalid_argument &) {
      // Exact strengths matter: not something Build from gates is asked.
    }
  }
  CircuitLibrary *library = this->library_.get();
  this->job_ = std::make_unique<SolveJob>(
      spec, [spec, layout, options, key, library](const JobContext &job) {
        auto start = std::chrono::steady_clock::now();
        ShrinkOptions o = options;
        o.cancel = job.cancel;
        o.preview = job.preview;
        ShrinkResult shrunk = shrink(spec, layout, o, job.progress);
        if (key && library &&
            !verify_layout(shrunk.layout, shrunk.spec,
                           shrunk.spec.verification_vectors(),
                           options.solver.verification)) {
          Vec3 size = shrunk.layout.size();
          CircuitLibrary::Entry entry{
              to_json(shrunk.spec), to_json(shrunk.layout),
              layout_blocks(shrunk.layout), size.x * size.y * size.z, ""};
          if (library->offer(*key, entry) && job.progress)
            job.progress("kept in the circuit library for Build from gates");
        }
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

// Builds catalog modules: for each function list and box size, every
// arrangement of the ports not in the catalog yet, solved for the fewest
// blocks (several at once). A list is 2-input truth tables, or
// {"inputs": 3, "functions": [...]}: 3-input modules take their input rows
// in increasing order, so every other order of the inputs (another truth
// table) is built too. The catalog file is saved after each function list,
// so a stopped build resumes where it was.
Json Api::start_catalog(const Json &body) {
  if (this->job_ && !this->job_->is_done())
    throw std::invalid_argument("a solve is already running");
  if (!this->catalog_file_)
    throw std::invalid_argument("the gate catalog needs --library");
  struct FunctionList {
    int inputs = 2;
    std::vector<std::uint8_t> functions;
  };
  std::vector<FunctionList> function_lists;
  for (const Json &entry : body.at("functions")) {
    const bool plain = entry.is_array();
    const int inputs = plain ? 2 : entry.at("inputs").get<int>();
    if (inputs != 2 && inputs != 3)
      throw std::invalid_argument("a catalog module has 2 or 3 inputs");
    std::vector<std::uint8_t> functions;
    for (const Json &f : plain ? entry : entry.at("functions")) {
      const int value = f.get<int>();
      if (value < 0 || value >= (1 << (1 << inputs)))
        throw std::invalid_argument(
            std::format("{} is no {}-input truth table", value, inputs));
      functions.push_back(static_cast<std::uint8_t>(value));
    }
    if (functions.empty())
      throw std::invalid_argument("a function list is empty");
    if (inputs == 2) {
      function_lists.push_back({inputs, std::move(functions)});
      continue;
    }
    std::vector<int> order{0, 1, 2};
    std::set<std::vector<std::uint8_t>> orders;
    do {
      std::vector<std::uint8_t> permuted;
      for (std::uint8_t f : functions)
        permuted.push_back(permute_function(f, inputs, order));
      if (orders.insert(permuted).second)
        function_lists.push_back({inputs, std::move(permuted)});
    } while (std::next_permutation(order.begin(), order.end()));
  }
  std::vector<Vec3> sizes;
  for (const Json &size : body.at("sizes"))
    sizes.push_back(
        {size.at(0).get<int>(), size.at(1).get<int>(), size.at(2).get<int>()});
  GateLibraryOptions gates;
  gates.time_limit_seconds = body.value("seconds", 20.0);
  if (gates.time_limit_seconds <= 0)
    throw std::invalid_argument("the time per arrangement must be positive");
  gates.solver.verification = verification_from(body);
  std::vector<BlockType> palette;
  for (const Json &name : body.at("palette")) {
    std::optional<BlockType> type =
        parse_palette_block(name.get<std::string>());
    if (!type)
      throw std::invalid_argument("unknown block " + name.get<std::string>());
    palette.push_back(*type);
  }
  gates.palette = palette;
  const std::string key = catalog_key(gates.palette, gates.solver.verification);
  GateCatalog *catalog = &this->catalog_;
  const std::filesystem::path file = *this->catalog_file_;
  Spec spec;
  this->job_ = std::make_unique<SolveJob>(spec, [function_lists, sizes, gates,
                                                 key, catalog,
                                                 file](const JobContext &job) {
    auto start = std::chrono::steady_clock::now();
    const int workers =
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    for (const auto &[inputs, functions] : function_lists) {
      std::string names;
      for (std::uint8_t f : functions)
        names +=
            (names.empty() ? "" : "+") +
            (inputs == 2 ? function_name(f) : std::format("3-input {}", f));
      for (Vec3 size : sizes) {
        if (size.z < inputs)
          continue;
        std::vector<GateArrangement> todo;
        for (const GateArrangement &a : gate_arrangements(
                 size, inputs, static_cast<int>(functions.size())))
          if (!catalog->has(key, functions, a))
            todo.push_back(a);
        std::atomic<std::size_t> next = 0;
        std::atomic<int> built = 0;
        auto worker = [&] {
          for (std::size_t i = next++; i < todo.size() && !job.cancel->load();
               i = next++) {
            GateLibraryOptions o = gates;
            o.cancel = job.cancel;
            if (std::optional<GateModule> module =
                    build_gate_in(functions, todo[i], o)) {
              catalog->add(key, *module);
              ++built;
            }
          }
        };
        std::vector<std::future<void>> threads;
        for (int w = 0; w < workers; ++w)
          threads.push_back(std::async(std::launch::async, worker));
        for (auto &thread : threads)
          thread.wait();
        if (job.progress)
          job.progress(std::format("{} in {}x{}x{}: {} of {} new "
                                   "arrangements built",
                                   names, size.x, size.y, size.z, built.load(),
                                   todo.size()));
        if (job.cancel->load())
          break;
      }
      save_catalog(file, *catalog);
      if (job.cancel->load())
        break;
    }
    SolveReport report;
    report.status =
        job.cancel->load() ? SolveStatus::Cancelled : SolveStatus::Feasible;
    report.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    return JobResult{std::move(report), std::nullopt};
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
  options.gates.time_limit_seconds = options.stage_seconds;
  options.max_height = settings.value("maxHeight", 0);
  options.flush_beam = settings.value("flushBeam", options.flush_beam);
  options.flush_router = settings.value("router", options.flush_router);
  options.gate_cache = &this->gate_cache_;
  options.catalog = &this->catalog_;
  // With a shrink time, every variant is built and shrunk; the best is kept.
  // totalTime: one budget for everything (build passes, then the shrink when
  // "shrink" is set); the step limits only shape the first pass.
  const double total_seconds = settings.value("totalTime", 0.0);
  const bool shrink_until_deadline = settings.value("shrink", false);
  if (total_seconds < 0)
    throw std::invalid_argument("the total time must be positive");
  const double shrink_seconds = shrink_until_deadline && total_seconds > 0
                                    ? total_seconds
                                    : settings.value("shrinkTimeLimit", 0.0);
  const double cut_seconds = settings.value("cutTime", 120.0);
  if (shrink_seconds < 0 || cut_seconds <= 0)
    throw std::invalid_argument("shrink times must be positive");
  // Fails here, before the job starts, when no gate fits the height.
  gates_within_height(options.gates, options.max_height);
  options.solver.verification = verification_from(settings);
  if (options.stage_seconds <= 0)
    throw std::invalid_argument("the time limit must be positive");
  Spec spec = spec_from_json(body.at("spec"));
  // Answered right away: a table that needs exact strengths cannot be
  // composed from on/off gates.
  boolean_table(spec);
  const bool use_library = this->library_ && settings.value("library", true);
  const std::string key = compose_key(spec, options);
  std::optional<CircuitLibrary::Entry> known;
  if (this->library_)
    known = this->library_->find(key);
  if (use_library && known) {
    this->job_ = std::make_unique<SolveJob>(spec, from_library(*known, true));
    return Json{{"fromLibrary", true}};
  }
  // Searching again: only a circuit smaller than the library's is news.
  if (known)
    options.beat = world_from_json(known->layout).size();
  JobWork work = [spec, options, shrink_seconds, cut_seconds, total_seconds,
                  known](const JobContext &job) {
    const ProgressCallback &progress = job.progress;
    std::atomic<bool> *cancel = job.cancel;
    auto start = std::chrono::steady_clock::now();
    NetlistComposeOptions o = options;
    o.cancel = cancel;
    o.preview = job.preview;
    if (total_seconds > 0)
      o.deadline =
          start +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(total_seconds));
    std::optional<ComposeResult> composed;
    if (shrink_seconds > 0) {
      ShrinkOptions s;
      s.time_limit_seconds = shrink_seconds;
      s.cut_seconds = cut_seconds;
      s.solver.verification = options.solver.verification;
      s.preview = job.preview;
      composed = compose_and_shrink(spec, o, s, progress);
    } else {
      composed = compose_spec(spec, o, progress);
    }
    SolveReport report;
    report.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    if (!composed) {
      // Wires cross two layers above each other: a low box may be why.
      if (!cancel->load() && progress && o.max_height > 0 &&
          o.max_height < kCrossingHeight)
        progress(std::format("tip: wires that must cross need a box at least "
                             "{} high (this one is {})",
                             kCrossingHeight, o.max_height));
      report.status =
          cancel->load() ? SolveStatus::Cancelled : SolveStatus::OutOfTime;
      return JobResult{std::move(report), std::nullopt};
    }
    // Nothing smaller than the circuit found before: that one stays.
    if (known) {
      Vec3 size = composed->layout.size();
      Vec3 before = world_from_json(known->layout).size();
      if (size.x * size.y * size.z >= before.x * before.y * before.z) {
        if (progress)
          progress(std::format("nothing smaller than the {}x{}x{} circuit "
                               "found before; returning that one",
                               before.x, before.y, before.z));
        JobResult kept = from_library(*known, true)(job);
        kept.report.seconds = report.seconds;
        return kept;
      }
    }
    report.status = SolveStatus::Feasible;
    report.solution =
        EncodedSolution{composed->layout,
                        static_cast<int>(composed->spec.hints.size()),
                        {},
                        {},
                        {}};
    return JobResult{std::move(report), std::move(composed->spec)};
  };
  this->job_ = std::make_unique<SolveJob>(
      spec,
      offering(this->library_.get(), key, to_json(spec), "", std::move(work)));
  return Json::object();
}

Json Api::solve_status(const Json &body) const {
  if (!this->job_)
    return {{"running", false}, {"log", Json::array()}};
  Json status{{"running", !this->job_->is_done()},
              {"seconds", this->job_->seconds()},
              {"log", this->job_->log()}};
  // The circuit so far, when newer than the one the caller has.
  int version = this->job_->preview_version();
  status["previewVersion"] = version;
  if (version > body.value("previewSince", 0))
    if (std::optional<World> preview = this->job_->preview())
      status["preview"] = to_json(*preview);
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
  // A live world (settle: false) evolves tick by tick, as after an edit in
  // the game; otherwise it starts at rest.
  if (body.value("settle", true))
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

// A player's right-click on a lever or button.
Json Api::simulator_use(const Json &body) {
  const Json &at = body.at("position");
  if (!at.is_array() || at.size() != 3)
    throw std::invalid_argument("position must be [x, y, z]");
  this->simulator().use({at[0].get<int>(), at[1].get<int>(), at[2].get<int>()});
  return this->simulator_state();
}

// What the results card shows about a circuit: whether it passes, and per
// output the latest tick at which it still changed after any input change.
Json Api::analyze_result(const Json &body) const {
  Spec spec = spec_from_json(body.at("spec"));
  World world = world_from_json(body.at("world"));
  LayoutCheck check = check_layout(world, spec, verification_from(body));
  std::vector<int> settle(spec.outputs.size(), 0);
  for (const SwitchCheck &change : check.switches)
    for (std::size_t t = 1; t < change.timeline.size(); ++t)
      for (std::size_t o = 0; o < settle.size(); ++o)
        if (change.timeline[t][o] != change.timeline[t - 1][o])
          settle[o] = std::max(settle[o], static_cast<int>(t));
  return {{"passes", check.passes()}, {"settleTicks", settle}};
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

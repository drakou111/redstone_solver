// Experiment: the first solver round's model as CNF, solved by CP-SAT and by
// an external SAT solver (CaDiCaL), on the exact same clauses.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "redstone/encoder.hpp"
#include "redstone/spec.hpp"
#include "redstone/verifier.hpp"

namespace {

namespace sat = operations_research::sat;
using Clock = std::chrono::steady_clock;

// CaDiCaL 3 has no wall-clock limit on Windows; GNU timeout provides one.
constexpr std::string_view kTimeoutTool =
    "C:/Program Files/Git/usr/bin/timeout.exe";

constexpr std::string_view kUsage =
    "redstone_satbench <cadical.exe> <work dir> <time limit s> <spec>...\n";

std::string read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open " + path.string());
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string check(const rs::CircuitEncoder &encoder,
                  const sat::CpSolverResponse &response,
                  const rs::Spec &spec) {
  rs::EncodedSolution solution = encoder.decode(response);
  auto failure = rs::verify_layout(solution.layout, spec,
                                   spec.verification_vectors());
  return std::format("{} blocks, {}", solution.block_count,
                     failure ? "rejected by verifier" : "verified");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    std::cerr << kUsage;
    return 2;
  }
  std::filesystem::path cadical = argv[1];
  std::filesystem::path work = argv[2];
  double limit = std::stod(argv[3]);
  std::filesystem::create_directories(work);

  for (int a = 4; a < argc; ++a) {
    std::filesystem::path spec_path = argv[a];
    rs::Spec spec = rs::parse_spec(read_file(spec_path));
    rs::ModelPlan plan;
    for (const rs::InputVector &inputs : spec.representative_vectors())
      plan.steady.push_back({inputs, spec.expected_outputs(inputs)});

    Clock::time_point build_start = Clock::now();
    rs::CircuitEncoder encoder(spec, plan, {.minimize_blocks = false});
    sat::CpModelProto proto = encoder.model().Build();
    double build_seconds = seconds_since(build_start);
    for (const auto &constraint : proto.constraints())
      if (!constraint.has_bool_or())
        throw std::logic_error("model has a non-clause constraint");

    const auto &clauses = encoder.clauses();
    std::filesystem::path cnf = work / (spec_path.stem().string() + ".cnf");
    {
      std::ofstream out(cnf);
      out << std::format("p cnf {} {}\n", proto.variables_size(),
                         clauses.size());
      for (const auto &clause : clauses) {
        for (int lit : clause)
          out << lit << ' ';
        out << "0\n";
      }
    }
    std::cout << std::format("{}: {} vars, {} clauses, built in {:.2f}s\n",
                             spec_path.stem().string(), proto.variables_size(),
                             clauses.size(), build_seconds);

    // CP-SAT, as the solver runs it (all cores, stop at first solution).
    sat::SatParameters parameters;
    parameters.set_max_time_in_seconds(limit);
    parameters.set_num_workers(
        static_cast<int>(std::thread::hardware_concurrency()));
    parameters.set_stop_after_first_solution(true);
    Clock::time_point cp_start = Clock::now();
    sat::CpSolverResponse cp_response =
        sat::SolveWithParameters(proto, parameters);
    double cp_seconds = seconds_since(cp_start);
    std::string cp_result =
        cp_response.status() == sat::CpSolverStatus::FEASIBLE ||
                cp_response.status() == sat::CpSolverStatus::OPTIMAL
            ? check(encoder, cp_response, spec)
            : sat::CpSolverStatus_Name(cp_response.status());
    std::cout << std::format("  cp-sat   {:7.2f}s  {}\n", cp_seconds,
                             cp_result);

    // CaDiCaL on the same clauses.
    std::filesystem::path model_file =
        work / (spec_path.stem().string() + ".model");
    std::string command = std::format(
        "\"\"{}\" {} \"{}\" -q \"{}\" > \"{}\"\"", kTimeoutTool,
        static_cast<int>(limit), cadical.string(), cnf.string(),
        model_file.string());
    Clock::time_point sat_start = Clock::now();
    int code = std::system(command.c_str());
    double sat_seconds = seconds_since(sat_start);
    std::string sat_result = std::format("exit {}", code);
    if (code == 10) {
      sat::CpSolverResponse response;
      response.set_status(sat::CpSolverStatus::FEASIBLE);
      response.mutable_solution()->Resize(proto.variables_size(), 0);
      std::istringstream in(read_file(model_file));
      std::string token;
      while (in >> token) {
        if (token == "v" || token == "s" || token == "SATISFIABLE")
          continue;
        int lit = std::stoi(token);
        if (lit > 0)
          response.set_solution(lit - 1, 1);
      }
      sat_result = check(encoder, response, spec);
    } else if (code == 20) {
      sat_result = "INFEASIBLE";
    }
    std::cout << std::format("  cadical  {:7.2f}s  {}\n", sat_seconds,
                             sat_result)
              << std::flush;
  }
  return 0;
}

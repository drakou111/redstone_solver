#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"

// Last, so Catch2's CHECK replaces the one abseil (via OR-Tools) defines.
#include <catch2/catch_test_macros.hpp>

namespace sat = operations_research::sat;

// Guards the toolchain: OR-Tools links, loads its DLLs and solves.
TEST_CASE("CP-SAT finds the optimum of a small model") {
  sat::CpModelBuilder model;
  sat::IntVar x = model.NewIntVar({0, 15});
  sat::IntVar y = model.NewIntVar({0, 15});
  model.AddEquality(x + y, 20);
  model.AddGreaterThan(x, y);
  model.Minimize(x);

  sat::CpSolverResponse response = sat::Solve(model.Build());
  REQUIRE(response.status() == sat::CpSolverStatus::OPTIMAL);
  CHECK(sat::SolutionIntegerValue(response, x) == 11);
  CHECK(sat::SolutionIntegerValue(response, y) == 9);
}

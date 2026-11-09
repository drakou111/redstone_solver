#include <cstdlib>

#include "redstone/gate_library.hpp"
#include "redstone/netlist.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

TEST_CASE("the gate library builds clean-faced modules for every 2-input "
          "function") {
  GateLibraryOptions options;
  options.time_limit_seconds = 30.0;
  for (std::uint8_t function : two_input_functions()) {
    INFO(function_name(function));
    std::optional<GateModule> gate = build_gate({function}, options);
    REQUIRE(gate);
    const World &w = gate->layout;
    Vec3 size = w.size();
    CHECK(w.at(gate->in1).type == BlockType::Input);
    CHECK(w.at(gate->in2).type == BlockType::Input);
    REQUIRE(gate->outs.size() == 1);
    CHECK(w.at(gate->outs[0]).type == BlockType::Output);
    // Faces: nothing but the ports above the floor.
    for (int y = 1; y < size.y; ++y)
      for (int z = 0; z < size.z; ++z) {
        Vec3 west{0, y, z}, east{size.x - 1, y, z};
        if (west != gate->in1 && west != gate->in2)
          CHECK(w.at(west).type == BlockType::Air);
        if (east != gate->outs[0])
          CHECK(w.at(east).type == BlockType::Air);
      }
  }
}

TEST_CASE("the gate library builds a half adder as one two-output module") {
  GateLibraryOptions options;
  options.time_limit_seconds = 30.0;
  std::optional<GateModule> ha = build_gate({0b0110, 0b1000}, options);
  REQUIRE(ha);
  REQUIRE(ha->outs.size() == 2);
  CHECK(std::abs(ha->outs[0].z - ha->outs[1].z) >= 2);
  CHECK(ha->layout.at(ha->outs[1]).type == BlockType::Output);
}

TEST_CASE("gate arrangements put inputs in two rows and outputs apart") {
  std::vector<GateArrangement> one = gate_arrangements({4, 2, 3}, 1);
  CHECK(one.size() == 6 * 3); // ordered input pairs, then any output row
  for (const GateArrangement &a : one)
    CHECK(a.in1_z != a.in2_z);
  for (const GateArrangement &a : gate_arrangements({4, 2, 3}, 2)) {
    REQUIRE(a.out_rows.size() == 2);
    CHECK(a.out_rows[1] - a.out_rows[0] >= 2);
  }
}

TEST_CASE("a catalog keeps each arrangement's module with the fewest blocks") {
  GateLibraryOptions options;
  options.time_limit_seconds = 30.0;
  const std::uint8_t kAnd = 0b1000;
  GateArrangement arrangement{{5, 2, 3}, 0, 2, {1}};
  std::optional<GateModule> module =
      build_gate_in({kAnd}, arrangement, options);
  REQUIRE(module);
  CHECK(module->in1 == Vec3{0, 1, 0});
  CHECK(module->in2 == Vec3{0, 1, 2});
  REQUIRE(module->outs.size() == 1);
  CHECK(module->outs[0] == Vec3{4, 1, 1});

  GateCatalog catalog;
  const std::string key = catalog_key(std::nullopt, {});
  CHECK(catalog.add(key, *module));
  CHECK(catalog.has(key, {kAnd}, arrangement));
  CHECK_FALSE(catalog.add(key, *module)); // not fewer blocks
  CHECK(catalog.variants(key, {kAnd}, 2).size() == 1);
  CHECK(catalog.variants(key, {kAnd}, 1).empty()); // too tall
  CHECK(catalog.variants(key, {0b0110}, 2).empty());
}

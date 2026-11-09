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
    REQUIRE(gate->ins.size() == 2);
    CHECK(w.at(gate->ins[0]).type == BlockType::Input);
    CHECK(w.at(gate->ins[1]).type == BlockType::Input);
    REQUIRE(gate->outs.size() == 1);
    CHECK(w.at(gate->outs[0]).type == BlockType::Output);
    // Faces: nothing but the ports above the floor.
    for (int y = 1; y < size.y; ++y)
      for (int z = 0; z < size.z; ++z) {
        Vec3 west{0, y, z}, east{size.x - 1, y, z};
        if (west != gate->ins[0] && west != gate->ins[1])
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
  std::vector<GateArrangement> one = gate_arrangements({4, 2, 3}, 2, 1);
  CHECK(one.size() == 6 * 3); // ordered input pairs, then any output row
  for (const GateArrangement &a : one) {
    REQUIRE(a.in_rows.size() == 2);
    CHECK(a.in_rows[0] != a.in_rows[1]);
  }
  for (const GateArrangement &a : gate_arrangements({4, 2, 3}, 2, 2)) {
    REQUIRE(a.out_rows.size() == 2);
    CHECK(a.out_rows[1] - a.out_rows[0] >= 2);
  }
}

TEST_CASE("a catalog keeps each arrangement's module with the fewest blocks") {
  GateLibraryOptions options;
  options.time_limit_seconds = 30.0;
  const std::uint8_t kAnd = 0b1000;
  GateArrangement arrangement{{5, 2, 3}, {0, 2}, {1}};
  std::optional<GateModule> module =
      build_gate_in({kAnd}, arrangement, options);
  REQUIRE(module);
  CHECK(module->ins == std::vector<Vec3>{{0, 1, 0}, {0, 1, 2}});
  REQUIRE(module->outs.size() == 1);
  CHECK(module->outs[0] == Vec3{4, 1, 1});

  GateCatalog catalog;
  const std::string key = catalog_key(std::nullopt, {});
  CHECK(catalog.add(key, *module));
  CHECK(catalog.has(key, {kAnd}, arrangement));
  CHECK_FALSE(catalog.add(key, *module)); // not fewer blocks
  CHECK(catalog.variants(key, 2, {kAnd}, 2).size() == 1);
  CHECK(catalog.variants(key, 2, {kAnd}, 1).empty()); // too tall
  CHECK(catalog.variants(key, 2, {0b0110}, 2).empty());
  CHECK(catalog.variants(key, 3, {kAnd}, 2).empty()); // another input count
  CHECK(catalog.has_any(key, 2, {kAnd}));
  CHECK_FALSE(catalog.has_any(key, 3, {kAnd}));
}

TEST_CASE("truth tables change with the order of their inputs") {
  const std::uint8_t kANotB = 0b0100; // a = 1, b = 0
  const std::uint8_t kBNotA = 0b0010;
  CHECK(permute_function(kANotB, 2, {1, 0}) == kBNotA);
  CHECK(permute_function(kANotB, 2, {0, 1}) == kANotB);
  const std::uint8_t kXor3 = 0x96;
  CHECK(permute_function(kXor3, 3, {2, 0, 1}) == kXor3);
  // c AND (a OR b): true for a b c = 011, 101, 111.
  const std::uint8_t kCAndAOrB = 0b10101000;
  // With c first: c a b = 101, 110, 111.
  CHECK(permute_function(kCAndAOrB, 3, {2, 0, 1}) == 0b11100000);
  CHECK(depends_on_all(kXor3, 3));
  CHECK_FALSE(depends_on_all(0b11110000, 3)); // just the first input
  CHECK(depends_on_all(kANotB, 2));
}

TEST_CASE("3-input arrangements keep the input rows in order") {
  std::vector<GateArrangement> three = gate_arrangements({5, 2, 4}, 3, 1);
  CHECK(three.size() == 4 * 4); // rows {0,1,2} {0,1,3} {0,2,3} {1,2,3}
  for (const GateArrangement &a : three) {
    REQUIRE(a.in_rows.size() == 3);
    CHECK(a.in_rows[0] < a.in_rows[1]);
    CHECK(a.in_rows[1] < a.in_rows[2]);
  }
}

TEST_CASE("a 3-input module is built in a given arrangement") {
  GateLibraryOptions options;
  options.time_limit_seconds = 30.0;
  const std::uint8_t kOr3 = 0xFE;
  GateArrangement arrangement{{4, 2, 5}, {0, 2, 4}, {2}};
  std::optional<GateModule> module =
      build_gate_in({kOr3}, arrangement, options);
  REQUIRE(module);
  CHECK(module->ins == std::vector<Vec3>{{0, 1, 0}, {0, 1, 2}, {0, 1, 4}});
  GateCatalog catalog;
  const std::string key = catalog_key(std::nullopt, {});
  CHECK(catalog.add(key, *module));
  CHECK(catalog.has(key, {kOr3}, arrangement));
  CHECK(catalog.variants(key, 3, {kOr3}, 2).size() == 1);
}

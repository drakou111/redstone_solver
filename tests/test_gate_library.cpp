#include "redstone/gate_library.hpp"
#include "redstone/netlist.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

TEST_CASE("the gate library builds clean-faced modules for every 2-input "
          "function") {
  GateLibraryOptions options;
  options.seconds_per_size = 30.0;
  for (std::uint8_t function : two_input_functions()) {
    INFO(function_name(function));
    std::optional<GateModule> gate = build_gate(function, options);
    REQUIRE(gate);
    const World &w = gate->layout;
    Vec3 size = w.size();
    CHECK(w.at(gate->in1).type == BlockType::Input);
    CHECK(w.at(gate->in2).type == BlockType::Input);
    CHECK(w.at(gate->out).type == BlockType::Output);
    // Faces: nothing but the ports above the floor.
    for (int y = 1; y < size.y; ++y)
      for (int z = 0; z < size.z; ++z) {
        Vec3 west{0, y, z}, east{size.x - 1, y, z};
        if (west != gate->in1 && west != gate->in2)
          CHECK(w.at(west).type == BlockType::Air);
        if (east != gate->out)
          CHECK(w.at(east).type == BlockType::Air);
      }
  }
}

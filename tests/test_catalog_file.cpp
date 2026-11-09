#include <filesystem>
#include <fstream>

#include "host/catalog_file.hpp"
#include "host/json_io.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

GateModule module_of(std::vector<Vec3> ins) {
  World layout({4, 2, 5});
  for (Vec3 in : ins)
    layout.set(in, Block::solid());
  return {{0xFE}, layout, std::move(ins), {{3, 1, 2}}};
}

} // namespace

TEST_CASE("the gate catalog file keeps modules of three inputs") {
  const std::filesystem::path file =
      std::filesystem::temp_directory_path() / "redstone_catalog_test.json";
  GateCatalog catalog;
  catalog.add("k", module_of({{0, 1, 0}, {0, 1, 2}, {0, 1, 4}}));
  rs::host::save_catalog(file, catalog);

  GateCatalog loaded;
  rs::host::load_catalog(file, loaded);
  std::filesystem::remove(file);
  std::vector<GateModule> modules = loaded.variants("k", 3, {0xFE}, 2);
  REQUIRE(modules.size() == 1);
  CHECK(modules[0].ins == std::vector<Vec3>{{0, 1, 0}, {0, 1, 2}, {0, 1, 4}});
  CHECK(modules[0].outs == std::vector<Vec3>{{3, 1, 2}});
}

TEST_CASE("the gate catalog file reads modules written with in1 and in2") {
  const std::filesystem::path file =
      std::filesystem::temp_directory_path() / "redstone_catalog_old.json";
  rs::host::Json entry = {{"key", "k"},
                          {"functions", {8}},
                          {"layout", rs::host::to_json(module_of({}).layout)},
                          {"in1", {0, 1, 0}},
                          {"in2", {0, 1, 2}},
                          {"outs", {{3, 1, 1}}}};
  {
    std::ofstream out(file);
    out << rs::host::Json::array({entry}).dump();
  }
  GateCatalog loaded;
  rs::host::load_catalog(file, loaded);
  std::filesystem::remove(file);
  std::vector<GateModule> modules = loaded.variants("k", 2, {8}, 2);
  REQUIRE(modules.size() == 1);
  CHECK(modules[0].ins == std::vector<Vec3>{{0, 1, 0}, {0, 1, 2}});
}

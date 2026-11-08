#include <filesystem>

#include "host/library.hpp"

#include <catch2/catch_test_macros.hpp>

using rs::host::CircuitLibrary;
using rs::host::Json;

namespace {

CircuitLibrary::Entry entry(int blocks, int volume) {
  return {Json{{"name", "spec"}}, Json{{"blocks", blocks}}, blocks, volume};
}

std::filesystem::path fresh_directory() {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "redstone_library_test";
  std::filesystem::remove_all(dir);
  return dir;
}

} // namespace

TEST_CASE("the circuit library keeps the smallest circuit per request") {
  std::filesystem::path dir = fresh_directory();
  CircuitLibrary library(dir);
  CHECK_FALSE(library.find("a"));

  CHECK(library.offer("a", entry(30, 60)));
  CHECK_FALSE(library.offer("a", entry(40, 60))); // not smaller
  CHECK(library.offer("a", entry(20, 60)));       // fewer blocks, same cells
  CHECK_FALSE(library.offer("a", entry(10, 80))); // more cells
  CHECK(library.offer("b", entry(5, 10)));

  // A new library on the same directory sees what the old one kept.
  CircuitLibrary reopened(dir);
  REQUIRE(reopened.find("a"));
  CHECK(reopened.find("a")->blocks == 20);
  CHECK(reopened.find("a")->layout.at("blocks") == 20);
  CHECK(reopened.find("b")->volume == 10);
  CHECK_FALSE(reopened.find("c"));
  std::filesystem::remove_all(dir);
}

TEST_CASE("the circuit library finds the smallest circuit of a shape") {
  std::filesystem::path dir = fresh_directory();
  CircuitLibrary library(dir);
  CircuitLibrary::Entry big = entry(9, 40);
  big.shape = "box A";
  CircuitLibrary::Entry small = entry(7, 30);
  small.shape = "box A";
  CircuitLibrary::Entry other = entry(1, 5);
  other.shape = "box B";
  library.offer("and", big);
  library.offer("or", small);
  library.offer("xor", other);

  REQUIRE(library.smallest_of_shape("box A"));
  CHECK(library.smallest_of_shape("box A")->blocks == 7);
  CHECK(library.smallest_of_shape("box B")->volume == 5);
  CHECK_FALSE(library.smallest_of_shape("box C"));
  std::filesystem::remove_all(dir);
}

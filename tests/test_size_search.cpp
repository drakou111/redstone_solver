#include <string>

#include "redstone/size_search.hpp"
#include "redstone/verifier.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace rs;

namespace {

constexpr double kTimeLimitSeconds = 60.0;

// A NOT gate drawn far wider than it needs to be.
constexpr const char *kWideNot = R"(
size 6 2 1
input IN1 repeater 0 1 0 east
output OUT1 dust 5 1 0
horizon 12
row IN1(ss15) -> OUT1(ss0)
row IN1(ss0) -> OUT1(ss1..15)
)";

} // namespace

TEST_CASE("a box for a size keeps its ports free along their sides") {
  Spec spec = parse_spec(kWideNot);
  std::optional<Spec> box = spec_for_size(spec, {3, 3, 2});
  REQUIRE(box);
  CHECK(box->size == Vec3{3, 3, 2});
  REQUIRE(box->inputs[0].is_free());
  for (Vec3 p : box->inputs[0].region)
    CHECK(p.x == 0);
  for (Vec3 p : box->outputs[0].region)
    CHECK(p.x == 2);
  CHECK(box->inputs[0].region.size() == 4);    // two heights above the floor
  CHECK_FALSE(spec_for_size(spec, {3, 1, 2})); // nothing to stand on
}

TEST_CASE("the size search finds a smaller box than the circuit to beat") {
  Spec spec = parse_spec(kWideNot);
  SizeSearchOptions options;
  options.time_limit_seconds = kTimeLimitSeconds;
  int previews = 0;
  options.preview = [&](const World &) { ++previews; };
  std::optional<SizeSearchResult> found =
      search_sizes(spec, spec.size, options);
  REQUIRE(found);
  Vec3 size = found->layout.size();
  CHECK(size.x * size.y * size.z < spec.size.x * spec.size.y * spec.size.z);
  CHECK(previews > 0);
  CHECK_FALSE(verify_layout(found->layout, found->spec,
                            found->spec.verification_vectors()));
}

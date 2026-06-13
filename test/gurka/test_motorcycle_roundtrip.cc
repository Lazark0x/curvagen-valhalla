#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <set>

using namespace valhalla;

// A rectangular loop around A:
//   A----B
//   |    |
//   C----D
// A native round trip from A at a target distance must ride the rectangle once
// (no edge twice) and return to A. Grid = 1000 m/char, so AB=DC=5 km, BD=CA=2 km
// and the perimeter is 14 km.
class MotorcycleRoundTrip : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B
      |    |
      C----D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        {"BD", {{"highway", "secondary"}}},
        {"DC", {{"highway", "secondary"}}},
        {"CA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip");
  }
};
gurka::map MotorcycleRoundTrip::map = {};

TEST_F(MotorcycleRoundTrip, ReturnsAClosedLoopNearTarget) {
  // locations = [start, start]; roundtrip sub-message carries target + count.
  auto result =
      gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                       {{"/roundtrip/target_distance", "14000"},
                        {"/roundtrip/num_candidates", "1"},
                        {"/costing_options/motorcycle/reuse_penalty", "1.0"}});

  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "no loop candidate returned";

  // The loop visits all four rectangle edges exactly once and returns to A.
  EXPECT_EQ(paths[0].size(), 4u) << "loop should be 4 distinct edges (no road twice)";
  std::set<std::string> uniq(paths[0].begin(), paths[0].end());
  EXPECT_EQ(uniq.size(), 4u) << "an edge was ridden twice (leash failed)";

  // Distance within ~±14% of the 14 km perimeter target.
  gurka::assert::raw::expect_path_length(result, 14.0, 2.0);
}

TEST_F(MotorcycleRoundTrip, ReturnsRequestedCandidateCount) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "2"}});
  const auto paths = gurka::detail::get_paths(result);
  EXPECT_LE(paths.size(), 2u) << "must not exceed requested candidate count";
  EXPECT_GE(paths.size(), 1u);
}

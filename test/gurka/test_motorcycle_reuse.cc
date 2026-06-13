#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

// Two paths between A and D:
//   Top:    A - B - D  (2 edges, shortest)
//   Bottom: A - C - E - D  (3 edges, longer)
// A Round Trip A -> D -> A with no leash reuses the top path both ways; with the
// reuse leash the return leg is forced onto the bottom path (no road ridden twice).
class MotorcycleReuse : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B----D
      |         |
      C---------E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BD", {{"highway", "secondary"}}},
        {"AC", {{"highway", "secondary"}}}, {"CE", {{"highway", "secondary"}}},
        {"ED", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_reuse");
  }
};
gurka::map MotorcycleReuse::map = {};

TEST_F(MotorcycleReuse, NoPenalty_ReusesShortestPathBothWays) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D", "A"}, "motorcycle",
                                 {{"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  // out: A-B-D, back: D-B-A (same roads, reversed)
  gurka::assert::raw::expect_path(result, {"AB", "BD", "BD", "AB"});
}

TEST_F(MotorcycleReuse, Penalty_ReturnsByTheOtherPath) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D", "A"}, "motorcycle",
                                 {{"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  // out: A-B-D (shortest), back forced onto the bottom path D-E-C-A
  gurka::assert::raw::expect_path(result, {"AB", "BD", "ED", "CE", "AC"});
}

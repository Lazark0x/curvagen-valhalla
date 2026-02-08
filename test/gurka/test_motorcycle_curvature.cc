#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

class MotorcycleCurvature : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // Two paths from A to D:
    //   Top path:    A - B - D  (shorter, set to straight / curvature 15)
    //   Bottom path: A - C - E - D  (longer, set to curvy / curvature 0)
    const std::string ascii_map = R"(
      A----B----D
      |         |
      C---------E
    )";

    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        {"BD", {{"highway", "secondary"}}},
        {"AC", {{"highway", "secondary"}}},
        {"CE", {{"highway", "secondary"}}},
        {"ED", {{"highway", "secondary"}}},
    };

    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_curvature");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));

    std::vector<baldr::GraphId> straight_edges;
    straight_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "B")));
    straight_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "A")));
    straight_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "D")));
    straight_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "B")));

    std::vector<baldr::GraphId> curvy_edges;
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "C")));
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "A")));
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "E")));
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "E", "C")));
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "E", "D")));
    curvy_edges.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "E")));

    test::customize_edges(map.config,
                          [&straight_edges, &curvy_edges](const baldr::GraphId& edgeid,
                                                         baldr::DirectedEdge& edge) {
                            if (std::find(straight_edges.begin(), straight_edges.end(), edgeid) !=
                                straight_edges.end()) {
                              edge.set_curvature(15);
                            } else if (std::find(curvy_edges.begin(), curvy_edges.end(), edgeid) !=
                                       curvy_edges.end()) {
                              edge.set_curvature(0);
                            }
                          });
  }
};

gurka::map MotorcycleCurvature::map = {};

TEST_F(MotorcycleCurvature, NoPreference_TakesShortestPath) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"prefer_curvature", "0.0"}});
  gurka::assert::raw::expect_path(result, {"AB", "BD"});
}

TEST_F(MotorcycleCurvature, MaxPreference_TakesCurvyPath) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"prefer_curvature", "1.0"}});
  gurka::assert::raw::expect_path(result, {"AC", "CE", "ED"});
}

TEST_F(MotorcycleCurvature, ModeratePreference_ChangesCost) {
  auto result_none = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                      {{"prefer_curvature", "0.0"}});
  auto result_mod = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                     {{"prefer_curvature", "0.5"}});

  auto cost_none =
      result_none.trip().routes(0).legs(0).node().rbegin()->cost().elapsed_cost().cost();
  auto cost_mod =
      result_mod.trip().routes(0).legs(0).node().rbegin()->cost().elapsed_cost().cost();

  EXPECT_NE(cost_none, cost_mod);
}

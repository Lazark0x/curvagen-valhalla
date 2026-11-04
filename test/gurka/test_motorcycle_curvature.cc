#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

using namespace valhalla;

class MotorcycleCurvature : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // Two paths from A to D (new metric: 0 = straight, 15 = maximally twisty):
    //   Top path:    A - B - D  (shorter, set to straight / curvature 0)
    //   Bottom path: A - C - E - D  (longer, set to curvy / curvature 15)
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
                              edge.set_curvature(0);
                            } else if (std::find(curvy_edges.begin(), curvy_edges.end(), edgeid) !=
                                       curvy_edges.end()) {
                              edge.set_curvature(15);
                            }
                          });
  }
};

gurka::map MotorcycleCurvature::map = {};

TEST_F(MotorcycleCurvature, NoPreference_TakesShortestPath) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"/costing_options/motorcycle/prefer_curvature", "0.0"}});
  gurka::assert::raw::expect_path(result, {"AB", "BD"});
}

TEST_F(MotorcycleCurvature, MaxPreference_TakesCurvyPath) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"/costing_options/motorcycle/prefer_curvature", "1.0"}});
  gurka::assert::raw::expect_path(result, {"AC", "CE", "ED"});
}

TEST_F(MotorcycleCurvature, ModeratePreference_ChangesCost) {
  auto result_none = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                      {{"/costing_options/motorcycle/prefer_curvature", "0.0"}});
  auto result_mod = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                     {{"/costing_options/motorcycle/prefer_curvature", "0.5"}});

  auto cost_none =
      result_none.trip().routes(0).legs(0).node().rbegin()->cost().elapsed_cost().cost();
  auto cost_mod =
      result_mod.trip().routes(0).legs(0).node().rbegin()->cost().elapsed_cost().cost();

  EXPECT_NE(cost_none, cost_mod);
}

// (b2) curvature rescale: the fork's discounts must not make the fixed, seconds-denominated
// transition costs (turn costs, maneuver_penalty, the continuity penalty) heavier against
// the roads than upstream's costing makes them. Measured in transition units, a maximally
// curvy edge never costs less per second of travel than upstream charges for the same
// road, which here is the cheapest road upstream prices (density 0, paved, no class
// penalty: factor 0.85). That is the factor >= 1 property, relative to upstream's floor.
//
// The transition unit is read off the route itself: the same path is costed with
// maneuver_penalty 0 and 100, and the turn's transition cost moves by 100 * scale. Before
// (b2) the scale was 1 and a curvature-15 edge cost 0.25 per second at c1.0 against
// upstream's 0.85.
TEST(MotorcycleCurvatureRescale, CurvyEdgeNeverBelowUpstreamFloorInTransitionUnits) {
  const std::string ascii_map = R"(
    A----B
         |
         C----D
  )";
  const gurka::ways ways = {
      {"AB", {{"highway", "tertiary"}}},
      {"BC", {{"highway", "tertiary"}}},
      {"CD", {{"highway", "tertiary"}}},
  };
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_curvature_rescale");
  test::customize_edges(map.config, [](const baldr::GraphId&, baldr::DirectedEdge& edge) {
    edge.set_curvature(baldr::kMaxCurvatureFactor);
  });

  // crates/domain/src/costing.rs, motorways allowed: the orchestrator's map from the
  // rider's curviness c to the costing options. fork = false turns the fork's own
  // discounts (prefer_curvature, curviness_continuity, prefer_elevation) off.
  auto options = [](float c, int maneuver_penalty, bool fork) {
    auto r2 = [](float x) { return std::to_string(std::round(x * 100.0f) / 100.0f); };
    const std::string m = "/costing_options/motorcycle/";
    return std::unordered_map<std::string, std::string>{
        {m + "use_highways", r2(std::max(0.1f, 1.0f - 0.7f * c))},
        {m + "use_trails", r2(std::min(0.8f, 0.8f * c))},
        {m + "use_tolls", "0.5"},
        {m + "top_speed", std::to_string(static_cast<int>(120.0f - 40.0f * c))},
        {m + "maneuver_penalty", std::to_string(maneuver_penalty)},
        {m + "prefer_curvature", fork ? r2(c) : "0"},
        {m + "curviness_continuity", fork ? r2(c) : "0"},
        {m + "reuse_penalty", "0.8"},
        {m + "prefer_elevation", fork ? "0.3" : "0"},
    };
  };
  auto route = [&](float c, int maneuver_penalty, bool fork) {
    auto api = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                options(c, maneuver_penalty, fork));
    gurka::assert::raw::expect_path(api, {"AB", "BC", "CD"});
    return api;
  };
  // Cost per second of travel of the edge leaving node i.
  auto edge_factor = [](const valhalla::Api& api, int i) {
    const auto& nodes = api.trip().routes(0).legs(0).node();
    const auto& now = nodes.Get(i).cost();
    const auto& next = nodes.Get(i + 1).cost().elapsed_cost();
    const double cost = next.cost() - now.elapsed_cost().cost() - now.transition_cost().cost();
    const double secs =
        next.seconds() - now.elapsed_cost().seconds() - now.transition_cost().seconds();
    EXPECT_GT(secs, 0.0);
    return cost / secs;
  };

  for (float c : {0.5f, 0.7f, 1.0f}) {
    const auto base = route(c, 0, true);
    const auto pen = route(c, 100, true);
    const auto upstream = route(c, 0, false);
    ASSERT_EQ(base.trip().routes(0).legs(0).node_size(), 4);
    // Node 1 is the turn at B onto a differently named road (gurka names each way after
    // its key), so maneuver_penalty applies there.
    const double scale = (pen.trip().routes(0).legs(0).node(1).cost().transition_cost().cost() -
                          base.trip().routes(0).legs(0).node(1).cost().transition_cost().cost()) /
                         100.0;
    ASSERT_GT(scale, 0.0) << "c=" << c;
    for (int i = 0; i < 3; ++i) {
      const double floor = edge_factor(upstream, i);
      EXPECT_GE(edge_factor(base, i) / scale, floor * (1.0 - 1e-3))
          << "c=" << c << " edge " << i << ": cost/s " << edge_factor(base, i)
          << ", transition scale " << scale << ", upstream floor " << floor;
    }
  }
}

#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

// Two equal-length ways A->D (a symmetric diamond):
//   Top:    A - B - D  (straight)
//   Bottom: A - C - D  (continuous curvy)
// Equal length + equal turns isolate the curviness preference: with
// prefer_curvature + curviness_continuity the router stays on the curvy corridor.
TEST(MotorcycleContinuity, PrefersContinuousCurvy) {
  const std::string ascii_map = R"(
    B
  A   D
    C
  )";
  const gurka::ways ways = {
      {"AB", {{"highway", "tertiary"}}}, {"BD", {{"highway", "tertiary"}}},
      {"AC", {{"highway", "tertiary"}}}, {"CD", {{"highway", "tertiary"}}},
  };
  auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_continuity");
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  // Force the bottom corridor (AC, CD) curvy and the top (AB, BD) straight.
  std::vector<baldr::GraphId> curvy, straight;
  for (auto p : {std::pair{"A", "C"}, {"C", "D"}})
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, p.first, p.second)));
  for (auto p : {std::pair{"A", "B"}, {"B", "D"}})
    straight.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, p.first, p.second)));
  test::customize_edges(map.config, [&](const baldr::GraphId& id, baldr::DirectedEdge& e) {
    if (std::find(curvy.begin(), curvy.end(), id) != curvy.end())
      e.set_curvature(14);
    else if (std::find(straight.begin(), straight.end(), id) != straight.end())
      e.set_curvature(0);
  });
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"/costing_options/motorcycle/prefer_curvature", "1.0"},
                                  {"/costing_options/motorcycle/curviness_continuity", "1.0"}});
  gurka::assert::raw::expect_path(result, {"AC", "CD"});
}

#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

// Two equal-length ways A->D (a symmetric diamond):
//   Top:    A - B - D  (flat)
//   Bottom: A - C - D  (graded / hilly)
// Equal length isolates the grade preference: with prefer_elevation the router
// takes the graded route purely because graded edges are discounted.
TEST(MotorcycleElevation, PrefersGradedRoute) {
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
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_elevation");
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  std::vector<baldr::GraphId> graded;
  for (auto p : {std::pair{"A", "C"}, {"C", "D"}})
    graded.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, p.first, p.second)));
  test::customize_edges(map.config, [&](const baldr::GraphId& id, baldr::DirectedEdge& e) {
    if (std::find(graded.begin(), graded.end(), id) != graded.end())
      e.set_weighted_grade(12); // steep
    else
      e.set_weighted_grade(6); // flat
  });
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "motorcycle",
                                 {{"/costing_options/motorcycle/prefer_elevation", "1.0"}});
  gurka::assert::raw::expect_path(result, {"AC", "CD"});
}

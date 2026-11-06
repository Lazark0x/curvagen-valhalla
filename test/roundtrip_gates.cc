#include "thor/roundtrip_gates.h"

#include <gtest/gtest.h>

using namespace valhalla::thor;
using namespace valhalla::thor::roundtrip;

namespace {

// A hand-made trace: grid points with the along-trace metres at each.
GridTrace trace_of(std::vector<std::pair<int64_t, int64_t>> pts, std::vector<double> cum) {
  GridTrace t;
  t.pts = std::move(pts);
  t.cum = std::move(cum);
  return t;
}

std::vector<PathInfo> leg_at(const std::vector<float>& path_distances) {
  std::vector<PathInfo> leg;
  for (float pd : path_distances)
    leg.emplace_back(valhalla::sif::TravelMode::kDrive, valhalla::sif::Cost{},
                     valhalla::baldr::GraphId{}, 0, pd);
  return leg;
}

} // namespace

TEST(RoundTripGates, GridTraceSnapsToTheGridAndDropsRepeats) {
  GridTrace t;
  t.add({20.000001, 45.0});
  t.add({20.000002, 45.0}); // the same grid cell
  t.add({20.0, 45.001});
  ASSERT_EQ(t.pts.size(), 2u);
  EXPECT_EQ(t.pts[0], std::make_pair(int64_t{4500000}, int64_t{2000000}));
  EXPECT_EQ(t.pts[1], std::make_pair(int64_t{4500100}, int64_t{2000000}));
  ASSERT_EQ(t.cum.size(), 2u);
  EXPECT_NEAR(t.cum[1], 111.2, 0.5); // 0.001 deg of latitude
}

TEST(RoundTripGates, MirrorStubIsTheRiddenOutLegOfAPalindrome) {
  // out A-B-C, back C-B-A, then on to D: a 20 m stub ridden both ways
  const GridTrace t =
      trace_of({{0, 0}, {0, 1}, {0, 2}, {0, 1}, {0, 0}, {5, 5}}, {0, 10, 20, 30, 40, 90});
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(t), 20.0);
}

TEST(RoundTripGates, MirrorStubTakesTheLongestOfSeveral) {
  const GridTrace t =
      trace_of({{0, 0}, {0, 1}, {0, 0}, {7, 7}, {8, 8}, {9, 9}, {8, 8}, {7, 7}, {3, 3}},
               {0, 5, 10, 100, 150, 225, 300, 350, 500});
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(t), 125.0);
}

TEST(RoundTripGates, SeamStubCountsOnlyAPalindromeNearTheSeam) {
  // a 20 m mirror at points 0-4, then 6 points of plain riding
  const GridTrace t =
      trace_of({{0, 0}, {0, 1}, {0, 2}, {0, 1}, {0, 0}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9}},
               {0, 10, 20, 30, 40, 90, 140, 190, 240, 290});
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(t, 2), 20.0); // seam at the apex
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(t, 6), 20.0); // within the slack of 2
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(t, 7), 0.0);  // past it
}

TEST(RoundTripGates, NoStubWithoutThreePoints) {
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(trace_of({{0, 0}, {0, 1}}, {0, 10})), 0.0);
  EXPECT_DOUBLE_EQ(longest_mirror_stub_m(GridTrace{}), 0.0);
}

TEST(RoundTripGates, SeamWindowKeepsAboutWindowMetresEachSide) {
  // forward edges 100, 500, 600, 800 m; return edges 300, 600, 600, 500 m
  const auto fwd = leg_at({100, 600, 1200, 2000});
  const auto ret = leg_at({300, 900, 1500, 2000});
  const SeamWindow w = seam_window(fwd, ret, 1000.0);
  EXPECT_EQ(w.fwd_begin, 2u); // 800 + 600 >= 1000
  EXPECT_EQ(w.ret_end, 3u);   // 300 + 600 + 600 >= 1000

  const SeamWindow whole = seam_window(fwd, ret, 0.0);
  EXPECT_EQ(whole.fwd_begin, 0u);
  EXPECT_EQ(whole.ret_end, 4u);
}

TEST(RoundTripGates, SeamRejectsAlwaysRefillGeometryRejectsOnlyWithinBudget) {
  EXPECT_EQ(gate_action({}, 0, 2), GateAction::kServe);
  EXPECT_EQ(gate_action({true, false, false}, 5, 2), GateAction::kStashSeam);
  EXPECT_EQ(gate_action({true, true, true}, 5, 2), GateAction::kStashSeam);
  EXPECT_EQ(gate_action({false, true, false}, 1, 2), GateAction::kStashGeometry);
  EXPECT_EQ(gate_action({false, false, true}, 2, 2), GateAction::kKeepGated);
  EXPECT_EQ(gate_action({false, false, true}, 0, 0), GateAction::kKeepGated);
}

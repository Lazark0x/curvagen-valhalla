#include "thor/roundtrip_score.h"

#include <gtest/gtest.h>

#include <map>

using namespace valhalla::thor::roundtrip;

namespace {

constexpr double kExemption = 1500.0;

LoopEdge road(uint64_t canon,
              double seg_m,
              double from_start_m,
              double curvature = 0,
              double length_m = 0,
              bool resolved = true) {
  LoopEdge e;
  e.canon = canon;
  e.seg_m = seg_m;
  e.from_start_m = from_start_m;
  e.curvature = curvature;
  e.length_m = length_m;
  e.resolved = resolved;
  return e;
}

// road 1 and road 9 are twin carriageways; nothing else has a twin
TwinsFn twins_1_9() {
  return [](uint64_t canon, std::vector<uint64_t>& out) {
    if (canon == 1)
      out.push_back(9);
    if (canon == 9)
      out.push_back(1);
  };
}

} // namespace

TEST(RoundTripScore, BuiltOverlapIsTheReturnRidingTheForwardRoads) {
  const std::vector<LoopEdge> fwd = {road(1, 1000, 1000, 15, 1000), road(2, 1000, 2000, 0, 1000)};
  const std::vector<LoopEdge> ret = {road(3, 100, 4900, 0, 100), road(2, 200, 4700, 0, 200),
                                     road(3, 300, 4400, 0, 300), road(4, 400, 1000, 0, 400)};
  // road 1 sits inside the Start Exemption, so it is no overlap target
  const BuiltMeasure plain = measure_built(fwd, ret, {}, false, kExemption);
  EXPECT_DOUBLE_EQ(plain.overlap_m, 200.0);
  // the return riding road 3 twice counts too
  const BuiltMeasure rvr = measure_built(fwd, ret, {}, true, kExemption);
  EXPECT_DOUBLE_EQ(rvr.overlap_m, 500.0);
  // curvature 15 over 1000 of 3000 m, on the 0..15 scale
  EXPECT_FLOAT_EQ(plain.curviness, 1000.0f / 3000.0f);
}

TEST(RoundTripScore, BuiltOverlapSeesTheForwardRoadsTwin) {
  const std::vector<LoopEdge> fwd = {road(1, 2000, 2000, 0, 2000)};
  const std::vector<LoopEdge> ret = {road(9, 700, 2000, 0, 700)};
  EXPECT_DOUBLE_EQ(measure_built(fwd, ret, twins_1_9(), false, kExemption).overlap_m, 700.0);
  EXPECT_DOUBLE_EQ(measure_built(fwd, ret, {}, false, kExemption).overlap_m, 0.0);
}

TEST(RoundTripScore, BuiltMeasureSkipsUnresolvedEdges) {
  const std::vector<LoopEdge> fwd = {road(1, 2000, 2000, 15, 2000),
                                     road(2, 500, 2500, 15, 500, false)};
  const std::vector<LoopEdge> ret = {road(2, 500, 2000, 0, 2000, false), road(5, 100, 1900, 0, 2000)};
  const BuiltMeasure m = measure_built(fwd, ret, {}, true, kExemption);
  EXPECT_DOUBLE_EQ(m.overlap_m, 0.0);
  EXPECT_FLOAT_EQ(m.curviness, 0.5f); // 15 over 2000 of 4000 m
}

TEST(RoundTripScore, PairMeasureKeysTheRoadsBeyondTheExemption) {
  // tree: 1000 m exempt stem (road 7), then road 1; other: road 9 (road 1's twin), road 5
  const std::vector<LoopEdge> tree = {road(7, 1000, 1000, 15), road(1, 2000, 3000, 0)};
  const std::vector<LoopEdge> other = {road(5, 1400, 1400, 0), road(9, 1600, 3000, 0)};
  const PairMeasure m = measure_pair(tree, other, twins_1_9(), kExemption);
  EXPECT_EQ(m.keys.ridden, (std::unordered_map<uint64_t, double>{{1, 2000}, {9, 1600}}));
  EXPECT_EQ(m.keys.fwd_ridden, (std::unordered_map<uint64_t, double>{{1, 2000}}));
  EXPECT_EQ(m.keys.twins, (std::unordered_set<uint64_t>{1, 9}));
  EXPECT_DOUBLE_EQ(m.keys.fwd_total, 3000.0);
  EXPECT_DOUBLE_EQ(m.keys.total, 6000.0);
  EXPECT_DOUBLE_EQ(m.overlap_m, 1600.0); // the return home on road 1's twin
  EXPECT_FLOAT_EQ(m.curviness, 1000.0f / 6000.0f);
}

TEST(RoundTripScore, PairLengthCountsUnresolvedArcsButCurvinessDoesNot) {
  const std::vector<LoopEdge> tree = {road(1, 2000, 2000, 15), road(2, 2000, 4000, 15, 0, false)};
  const std::vector<LoopEdge> other = {road(3, 4000, 4000, 0)};
  const PairMeasure m = measure_pair(tree, other, {}, kExemption);
  EXPECT_DOUBLE_EQ(m.keys.fwd_total, 4000.0);
  EXPECT_DOUBLE_EQ(m.keys.total, 8000.0);
  EXPECT_FLOAT_EQ(m.curviness, 0.25f);
  EXPECT_EQ(m.keys.ridden.count(2), 0u);
}

TEST(RoundTripScore, PairReturnRidingItselfIsOverlap) {
  const std::vector<LoopEdge> tree = {road(1, 3000, 3000)};
  const std::vector<LoopEdge> other = {road(4, 500, 2000), road(4, 300, 2300), road(6, 200, 2500)};
  EXPECT_DOUBLE_EQ(measure_pair(tree, other, {}, kExemption).overlap_m, 300.0);
}

TEST(RoundTripScore, SharedFractionsReadRoadsAndTwins) {
  LoopKeys a;
  a.ridden = {{1, 300}, {2, 200}, {3, 500}};
  a.total = 2000;
  a.fwd_ridden = {{1, 300}, {3, 500}};
  a.fwd_total = 1000;
  LoopKeys b;
  b.ridden = {{1, 100}};
  b.twins = {3};
  b.total = 100;
  EXPECT_DOUBLE_EQ(shared_fraction(a, b), 0.4);
  EXPECT_DOUBLE_EQ(fwd_shared_fraction(a, b), 0.8);
  EXPECT_DOUBLE_EQ(shared_fraction(LoopKeys{}, b), 0.0);
  EXPECT_DOUBLE_EQ(fwd_shared_fraction(LoopKeys{}, b), 0.0);
}

TEST(RoundTripScore, AShortLoopInsideALongOneIsANearCopyEitherWay) {
  LoopKeys long_loop;
  long_loop.ridden = {{1, 1000}, {2, 1000}, {3, 1000}, {4, 1000}};
  long_loop.total = 4000;
  LoopKeys short_loop;
  short_loop.ridden = {{1, 1000}};
  short_loop.twins = {9};
  short_loop.total = 1000;
  EXPECT_TRUE(near_copies(short_loop, long_loop, 0.6));
  EXPECT_TRUE(near_copies(long_loop, short_loop, 0.6));
  LoopKeys apart;
  apart.ridden = {{5, 1000}, {1, 100}};
  apart.total = 1100;
  EXPECT_FALSE(near_copies(apart, long_loop, 0.6));
  EXPECT_FALSE(near_copies(long_loop, apart, 0.6));
}

TEST(RoundTripScore, BuiltKeysSumMetresPerRoadBeyondTheExemption) {
  const std::vector<LoopEdge> fwd = {road(7, 1000, 1000), road(1, 1500, 2500), road(2, 500, 3000)};
  const std::vector<LoopEdge> ret = {road(1, 400, 2600), road(8, 1200, 1200)};
  std::unordered_map<uint64_t, double> ridden, twin_m;
  const double total = built_keys(fwd, ret, 3000, 1600, twins_1_9(), kExemption, ridden, twin_m);
  EXPECT_DOUBLE_EQ(total, 4600.0);
  EXPECT_EQ(ridden, (std::unordered_map<uint64_t, double>{{1, 1900}, {2, 500}}));
  EXPECT_EQ(twin_m, (std::unordered_map<uint64_t, double>{{9, 1900}}));
}

TEST(RoundTripScore, ScoresDiscountCurvinessByOverlapAndDistanceError) {
  EXPECT_DOUBLE_EQ(distance_error(120000, 100000), 0.2);
  EXPECT_DOUBLE_EQ(distance_error(80000, 100000), 0.2);
  EXPECT_DOUBLE_EQ(distance_error(80000, 0), 0.0);
  EXPECT_DOUBLE_EQ(pair_score(0.6f, 0.2, 1.0), static_cast<double>(0.6f) / 1.2);
  // a quarter of the loop self-overlapping at Wo = 4 halves the score
  EXPECT_DOUBLE_EQ(built_score(0.5f, 250, 1000, 0.0, 4.0, 1.0), 0.25);
  EXPECT_DOUBLE_EQ(built_score(0.5f, 0, 0, 0.25, 4.0, 1.0), 0.4);
}

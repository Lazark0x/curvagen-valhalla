#include "baldr/graphconstants.h"
#include "midgard/pointll.h"
#include "mjolnir/util.h"

#include <gtest/gtest.h>

#include <cmath>

using valhalla::baldr::kCurvatureMetricVersion;
using valhalla::midgard::PointLL;
using valhalla::mjolnir::compute_curvature;

// A quarter-circle arc sampled coarsely (4 pts) vs finely (40 pts) must yield the
// same bucket (sampling invariance); a straight line must yield 0.
TEST(CurvatureMetric, SamplingInvariantAndStraightIsZero) {
  auto arc = [](int n) {
    std::vector<PointLL> v;
    for (int i = 0; i <= n; ++i) {
      double t = (M_PI / 2.0) * i / n; // quarter circle, ~0.01 deg radius
      v.emplace_back(20.0 + 0.01 * std::cos(t), 45.0 + 0.01 * std::sin(t));
    }
    return v;
  };
  uint32_t coarse = compute_curvature(arc(4));
  uint32_t fine = compute_curvature(arc(40));
  EXPECT_GT(coarse, 0u);
  EXPECT_NEAR(static_cast<int>(coarse), static_cast<int>(fine), 1); // same bucket +-1

  std::vector<PointLL> straight = {{20.0, 45.0}, {20.0, 45.01}, {20.0, 45.02}};
  EXPECT_EQ(compute_curvature(straight), 0u);

  // Two-point edges have no curvature.
  std::vector<PointLL> two = {{20.0, 45.0}, {20.0, 45.01}};
  EXPECT_EQ(compute_curvature(two), 0u);
}

// A right angle between two equal legs on the equator turns 90 degrees over 2 legs. The buckets
// below are worked by hand from the scale of metric version 1: deg/km / 350 * 15, rounded.
TEST(CurvatureMetric, BucketsPinnedToTheStampedVersion) {
  static_assert(kCurvatureMetricVersion == 1,
                "the buckets below belong to metric 1: re-derive them for the new version");
  auto right_angle = [](double leg_km) {
    const double leg_deg = leg_km / 111.195;
    return std::vector<PointLL>{{0.0, 0.0}, {leg_deg, 0.0}, {leg_deg, leg_deg}};
  };
  const char* bump = "compute_curvature moved: bump kCurvatureMetricVersion, then re-pin";
  EXPECT_EQ(compute_curvature(right_angle(1.0)), 2u) << bump;  //  45 deg/km -> 1.93
  EXPECT_EQ(compute_curvature(right_angle(0.5)), 4u) << bump;  //  90 deg/km -> 3.86
  EXPECT_EQ(compute_curvature(right_angle(0.2)), 10u) << bump; // 225 deg/km -> 9.64
  EXPECT_EQ(compute_curvature(right_angle(0.1)), 15u) << bump; // 450 deg/km -> capped
}

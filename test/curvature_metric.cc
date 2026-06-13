#include "mjolnir/util.h"
#include "midgard/pointll.h"

#include <cmath>
#include <gtest/gtest.h>

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

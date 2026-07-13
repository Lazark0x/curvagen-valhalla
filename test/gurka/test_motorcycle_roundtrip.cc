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

TEST_F(MotorcycleRoundTrip, ClampsExtremeCandidateCounts) {
  // The parse-side clamp (ADR-0036 T9) bounds num_candidates to 1..16 —
  // each candidate pays a return-leg bidirectional A*.
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "100000"}});
  const auto paths = gurka::detail::get_paths(result);
  EXPECT_GE(paths.size(), 1u);
  EXPECT_LE(paths.size(), 16u) << "num_candidates clamp (1..16) missing";

  auto zero = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                               {{"/roundtrip/target_distance", "14000"},
                                {"/roundtrip/num_candidates", "0"}});
  EXPECT_GE(gurka::detail::get_paths(zero).size(), 1u) << "0 must clamp to 1";
}

// A one-way dead-end spur: the expansion rides A->S legally (7 km, in the
// harvest band at target 14 km), but S has no auto-accessible outbound edge —
// the only way out is against the one-way. correlate_node() therefore yields a
// Location with ZERO path edges, and stock bidir A* reads edges(0) unchecked at
// entry (#44: null Rep* chain -> SIGSEGV, worker death). The sink candidate
// must fail alone; the rectangle loop must still be served.
class MotorcycleRoundTripOneWaySink : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      S------A----B
             |    |
             C----D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        {"BD", {{"highway", "secondary"}}},
        {"DC", {{"highway", "secondary"}}},
        {"CA", {{"highway", "secondary"}}},
        // one-way INTO the dead end: reachable outbound, unroutable back.
        {"AS", {{"highway", "secondary"}, {"oneway", "yes"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_oneway_sink");
  }
};
gurka::map MotorcycleRoundTripOneWaySink::map = {};

TEST_F(MotorcycleRoundTripOneWaySink, SinkTurnaroundSkippedNotFatal) {
  // Both in-band turnarounds get chosen (S ~270°, D ~111° — separate sectors):
  // the sink S must be skipped as a per-candidate failure, not kill the worker.
  auto result =
      gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                       {{"/roundtrip/target_distance", "14000"},
                        {"/roundtrip/num_candidates", "4"},
                        {"/costing_options/motorcycle/reuse_penalty", "1.0"}});

  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "sink candidate killed the whole round trip";

  // No served loop may contain the spur — its return leg cannot exist.
  for (const auto& path : paths)
    for (const auto& edge : path)
      EXPECT_NE(edge, "AS") << "a loop was built through the one-way sink";
}

// A long stem into a tight ladder of cells: many in-band turnaround nodes
// packed ~500 m apart — closer than the min-separation eps (0.1 x target/2 =
// 700 m at 14 km). Node-dedup keeps them all, bearings collapse into one or
// two sectors, and without the separation guard the curviness-only backfill
// returns geometrically identical loops (the measured Zlatibor 5/12 case).
class MotorcycleRoundTripCluster : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A------------B-C
                   | |
                   D-E
                   | |
                   F-G
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"BD", {{"highway", "secondary"}}}, {"CE", {{"highway", "secondary"}}},
        {"DE", {{"highway", "secondary"}}}, {"DF", {{"highway", "secondary"}}},
        {"EG", {{"highway", "secondary"}}}, {"FG", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 500);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_cluster");
  }
};
gurka::map MotorcycleRoundTripCluster::map = {};

TEST_F(MotorcycleRoundTripCluster, CandidatesAreGeometricallyDistinct) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "8"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  std::set<std::vector<std::string>> uniq(paths.begin(), paths.end());
  EXPECT_EQ(uniq.size(), paths.size())
      << "duplicate loop geometry — the turnaround separation guard failed";
}

#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <map>
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

// A curvy dead-end spur mid-corridor: the expansion legally U-turns at the dead end
// (the costing allows U-turns at deadend nodes), so the settled tree contains a bounced
// chain A->B->X->B whose doubled spur curvature outranks every clean candidate:
//   bounce X->B at 9 km (in band for target 18 km), curviness 0.444
//   clean  C->D at 9 km, curviness 0.222
// Without harvest bounce rejection (ADR-0037 turnaround hardening) the bounced chain is
// harvested and the forward leg ships an out-and-back spike on BX. The D-E-F-A road
// gives the clean candidate a return leg home.
class MotorcycleRoundTripBounce : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B-C-D
      |    |   |
      |    X   |
      F--------E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"BX", {{"highway", "secondary"}}},
        {"AF", {{"highway", "secondary"}}}, {"FE", {{"highway", "secondary"}}},
        {"ED", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_bounce");

    // The spur and the far corridor section are the curvy prizes; the bounced chain
    // rides the spur twice and outranks the clean C->D chain unless rejected.
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "X")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "X", "B")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "D")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "C")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripBounce::map = {};

TEST_F(MotorcycleRoundTripBounce, BouncedChainNotHarvested) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "18000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  // The bounced chain (curviest, rides the spur twice) must not be served: no loop
  // may enter the dead-end spur at all.
  for (const auto& path : paths)
    for (const auto& edge : path)
      EXPECT_NE(edge, "BX") << "a harvested chain bounced through the dead-end spur";
}

TEST_F(MotorcycleRoundTripBounce, HardExclusionForcesFreshReturn) {
  // With the leash OFF (reuse_penalty 1.0) retracing the 9 km corridor home is cheaper
  // than the 15 km D-E-F-A road — only the hard exclusion (ADR-0037) can force the
  // fresh return. Every edge of the loop must be ridden exactly once.
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "18000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  std::set<std::string> uniq(paths[0].begin(), paths[0].end());
  EXPECT_EQ(uniq.size(), paths[0].size())
      << "an edge was ridden twice — the return leg reused the forward corridor";
}

// A cul-de-sac start: A's only access is the 1 km edge AB — shorter than the Start
// Exemption (ADR-0037 §3), so the return may ride it home even though every other
// forward edge is hard-excluded. The primary-class corridor makes RETRACING strictly
// cheaper than the fresh secondary road, so if the exemption were missing (whole
// forward leg excluded -> no route -> Fallback Loop retry with no exclusion at all)
// the return would retrace the corridor — visible as BC/CD ridden twice.
class MotorcycleRoundTripCulDeSac : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      AB------C
       |      |
       E------D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        {"BC", {{"highway", "primary"}}},
        {"CD", {{"highway", "primary"}}},
        {"ED", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_culdesac");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "D")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "C")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripCulDeSac::map = {};

TEST_F(MotorcycleRoundTripCulDeSac, StartExemptionClosesTheLoop) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "18000"},
                                  {"/roundtrip/num_candidates", "2"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "cul-de-sac start must not fail the request";
  std::map<std::string, int> count;
  for (const auto& edge : paths[0])
    ++count[edge];
  EXPECT_EQ(count["AB"], 2) << "the exempt access road must carry the loop out and home";
  for (const auto& [name, n] : count)
    if (name != "AB")
      EXPECT_EQ(n, 1) << "non-exempt edge " << name
                      << " was reused — exemption missing (fallback retraced the corridor)";
}

TEST_F(MotorcycleRoundTripCulDeSac, HierarchyJunctionTurnaroundNotSkipped) {
  // At this target the only in-band candidate is D reached over the PRIMARY corridor —
  // a level-0 node whose secondary exit (DE) lives on its level-1 twin. correlate_node
  // must surface the whole physical junction's outbound set, or the non-U-turn
  // outbound guard reads D as exitless and the request 442s.
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "junction turnaround was skipped as exitless";
  std::map<std::string, int> count;
  for (const auto& edge : paths[0])
    ++count[edge];
  EXPECT_EQ(count["CD"], 1) << "the U-turn door must stay closed at the junction";
  EXPECT_EQ(count["ED"], 1) << "the return must leave over the level-1 twin's exit";
}

// No fresh road home exists: the corridor is the only connection to A, so the
// hard-excluded pass finds nothing and the ONE soft-leash retry must serve a
// Fallback Loop instead of failing the cell (clean-first, dirty-last-resort).
class MotorcycleRoundTripNoFreshReturn : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A--------B-T
               | |
               C-Y
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BT", {{"highway", "secondary"}}},
        {"TY", {{"highway", "secondary"}}}, {"YC", {{"highway", "secondary"}}},
        {"CB", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_nofresh");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "T")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "T", "B")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripNoFreshReturn::map = {};

TEST_F(MotorcycleRoundTripNoFreshReturn, FallbackLoopServedNotFailed) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "22000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "no-fresh-return cell must fall back, not 442";
  std::map<std::string, int> count;
  for (const auto& edge : paths[0])
    ++count[edge];
  EXPECT_EQ(count["AB"], 2) << "the Fallback Loop must ride the only road home";
}

// The #53 bank-fill death shape: the forward leg arrives at the turnaround over a
// ONE-WAY edge. The arrival edge's opposing direction fails the access filter, so a
// correlation built only from outbound edges (+ their opposings) omits the arrival
// edge — and TripLegBuilder then throws 499 for the forward leg, OUTSIDE the
// per-candidate failure contract: the whole request dies, all candidates lost.
// correlate_node must guarantee the arrival edge as an end-node PathEdge (ADR-0037).
class MotorcycleRoundTripOneWayArrival : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A--B----C
      |       |
      F-------E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        // one-way INTO the turnaround: legal to arrive, impossible to retrace.
        {"BC", {{"highway", "secondary"}, {"oneway", "yes"}}},
        {"CE", {{"highway", "secondary"}}},
        {"FE", {{"highway", "secondary"}}},
        {"AF", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_oneway_arrival");

    // Make the one-way approach the curvy prize so its head node is the top candidate.
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "C")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripOneWayArrival::map = {};

TEST_F(MotorcycleRoundTripOneWayArrival, OneWayArrivalDoesNotKillTheFill) {
  // Pre-fix this THROWS (TripLegBuilder 499 escapes the per-candidate contract).
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "one-way arrival killed the whole request (#53)";
  // The served loop rides the one-way approach out and the F-E road home.
  bool used_oneway = false;
  for (const auto& edge : paths[0])
    used_oneway |= (edge == "BC");
  EXPECT_TRUE(used_oneway) << "turnaround was not taken via the one-way approach";
}

// A curvy two-way dead-end spur whose TIP is the top in-band candidate. With the
// U-turn door dropped, the tip's only outbound edge is gone — the candidate must be
// skipped (a tip turnaround is a forced out-and-back spike), and the clean candidate
// must still be served. Guards both the door drop and the non-U-turn outbound check.
class MotorcycleRoundTripDeadEndTip : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B----T
      |    |
      F----E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BT", {{"highway", "secondary"}}},
        {"AF", {{"highway", "secondary"}}}, {"FE", {{"highway", "secondary"}}},
        {"EB", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_deadend_tip");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "T")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "T", "B")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripDeadEndTip::map = {};

TEST_F(MotorcycleRoundTripDeadEndTip, TipTurnaroundSkippedCleanLoopServed) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "2"},
                                  {"/costing_options/motorcycle/reuse_penalty", "1.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "tip skip must not kill the request";
  // No served loop may enter the dead-end spur: a tip turnaround is a forced spike.
  for (const auto& path : paths)
    for (const auto& edge : path)
      EXPECT_NE(edge, "BT") << "a loop was built to the dead-end tip (out-and-back spike)";
}

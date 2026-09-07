#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

using namespace valhalla;

namespace {
// v4 ships the pair pass and the P1.1 knee ON by default (src/thor/worker.cc, ADR-0041
// §4 — the engine that shipped is the engine that was measured). The suites here pin
// construction stages that run BEFORE the pair pass, on maps small enough that a
// different selection stage serves a different candidate entirely, so each states the
// configuration it is about instead of inheriting whatever the product ships today.
void pin_pre_pair_pass(gurka::map& m) {
  m.config.put("thor.roundtrip_pair_pass", false);
  m.config.put("thor.roundtrip_xcand_penalty", false);
  m.config.put("thor.roundtrip_gate_refill_budget", 0);
  m.config.put("thor.roundtrip_fallback_rungs", true);
}
} // namespace

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
    pin_pre_pair_pass(map);
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
    pin_pre_pair_pass(map);
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
    pin_pre_pair_pass(map);
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

// A 14 km rectangle asked for a 20 km loop, with the reverse arm one-wayed so the far
// corner cannot be reached the long way round: the primary harvest band (8.2–11.8 km)
// holds no viable turnaround — D sits at 7 km, C at 9 km is too close to the start
// (straight-line guard). Pre-flex engines 442 here; the ADR-0037 Distance Flex band
// (0.55–1.18 x target/2) reaches down to D and serves the clean 14 km loop instead.
class MotorcycleRoundTripFlex : public ::testing::Test {
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
        // one-way: rideable D->C (the return), not C->D (the long reverse arm).
        {"DC", {{"highway", "secondary"}, {"oneway", "yes"}}},
        {"CA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_flex");
    pin_pre_pair_pass(map);
  }
};
gurka::map MotorcycleRoundTripFlex::map = {};

TEST_F(MotorcycleRoundTripFlex, FlexServesWhenPrimaryBandIsEmpty) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "empty primary band must fall back to the flex band";
  std::set<std::string> uniq(paths[0].begin(), paths[0].end());
  EXPECT_EQ(uniq.size(), paths[0].size()) << "flex fill must still be a clean loop";
}

// The one-way sink is the CURVIEST candidate and K=1: selection picks it, hardening
// skips it, and without the refill queue (ADR-0037 build-until-full) the request dies
// 442 even though a clean rectangle loop exists right there in the band.
class MotorcycleRoundTripRefill : public ::testing::Test {
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
        {"AS", {{"highway", "secondary"}, {"oneway", "yes"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_refill");
    pin_pre_pair_pass(map);

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "S")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripRefill::map = {};

TEST_F(MotorcycleRoundTripRefill, FailedTopCandidateRefillsFromTheBand) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "sink pick must refill from the queue, not 442";
  for (const auto& edge : paths[0])
    EXPECT_NE(edge, "AS") << "the sink candidate itself must not be served";
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
    pin_pre_pair_pass(map);

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
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  // The bounced chain (curviest, rides the spur twice) must not be served: no loop
  // may enter the dead-end spur at all.
  for (const auto& path : paths)
    for (const auto& edge : path)
      EXPECT_NE(edge, "BX") << "a harvested chain bounced through the dead-end spur";
}

TEST_F(MotorcycleRoundTripBounce, HardExclusionForcesFreshReturn) {
  // With the leash OFF (reuse_penalty 0.0 -> reuse factor 1.0) retracing the 9 km
  // corridor home is cheaper than the 15 km D-E-F-A road — only ADR-0037 return
  // hardening (U-turn door + hard exclusion) can force the fresh return. Every edge of the loop must be ridden exactly once.
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "18000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
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
    pin_pre_pair_pass(map);

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
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
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
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
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
    pin_pre_pair_pass(map);

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

// The curviest candidate T sits above a BRANCHY dead-end cluster (Y forks to W and X,
// both dead ends): its fresh exit survives the walk-back probe (branches look viable),
// no fresh route home exists, and the Fallback return is forced to bounce a dead end —
// a wrapped-bounce spike whose mirror apex sits ~4 km off-seam, far outside the seam
// window. The Defect Gate must reject it via the FULL-leg decode (fallbacks never get
// the window — v3f leaked 25 wrapped bounces through it) and the refill queue must
// serve the clean second candidate D instead (clean-first).
class MotorcycleRoundTripDefectGate : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A---B-----T

        W-Y-X




C



D---E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BT", {{"highway", "secondary"}}},
        {"TY", {{"highway", "secondary"}}}, {"YW", {{"highway", "secondary"}}},
        {"YX", {{"highway", "secondary"}}}, {"AC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DE", {{"highway", "secondary"}}},
        {"EB", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_gate");
    pin_pre_pair_pass(map);

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy15, curvy10;
    curvy15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "T")));
    curvy15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "T", "B")));
    curvy10.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "D")));
    curvy10.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "C")));
    test::customize_edges(map.config, [&curvy15, &curvy10](const baldr::GraphId& edgeid,
                                                           baldr::DirectedEdge& edge) {
      if (std::find(curvy15.begin(), curvy15.end(), edgeid) != curvy15.end())
        edge.set_curvature(15);
      else if (std::find(curvy10.begin(), curvy10.end(), edgeid) != curvy10.end())
        edge.set_curvature(10);
    });
  }
};
gurka::map MotorcycleRoundTripDefectGate::map = {};

TEST_F(MotorcycleRoundTripDefectGate, WrappedBounceRejectedCleanServedInstead) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  bool has_clean_corridor = false;
  for (const auto& edge : paths[0]) {
    EXPECT_NE(edge, "BT") << "the wrapped-bounce loop was served over the clean one";
    has_clean_corridor |= (edge == "CD");
  }
  EXPECT_TRUE(has_clean_corridor) << "the clean refill candidate was not served";
  std::set<std::string> uniq(paths[0].begin(), paths[0].end());
  EXPECT_EQ(uniq.size(), paths[0].size()) << "served loop rides an edge twice";
}

// The primary band holds only the trap candidate T (its loop is a wrapped bounce); the
// clean C-D arm sits in the FLEX band only. The lazy wide-scan (ADR-0037 / #54) must
// fire when the primary queue runs dry and serve the clean flex loop — without it the
// cell falls back to the dirty last resort. Pins clean-first ACROSS bands and the
// widen-on-stall trigger.
class MotorcycleRoundTripLazyFlex : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A---B-----T

        W-Y-X

C


D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BT", {{"highway", "secondary"}}},
        {"TY", {{"highway", "secondary"}}}, {"YW", {{"highway", "secondary"}}},
        {"YX", {{"highway", "secondary"}}}, {"AC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DB", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_lazyflex");
    pin_pre_pair_pass(map);

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
gurka::map MotorcycleRoundTripLazyFlex::map = {};

TEST_F(MotorcycleRoundTripLazyFlex, WideScanFiresOnStallAndServesClean) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  bool rode_flex_arm = false;
  for (const auto& edge : paths[0]) {
    EXPECT_NE(edge, "BT") << "dirty primary loop served — the wide scan never fired";
    rode_flex_arm |= (edge == "CD");
  }
  EXPECT_TRUE(rode_flex_arm) << "the clean flex-band candidate was not served";
}

// Same trap topology with NO clean alternative anywhere: the Defect Gate rejects the
// only buildable loop, and the dirty-last-resort rule must serve it rather than 442
// (better one honest out-and-back than no route).
class MotorcycleRoundTripDirtyLastResort : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A---B-----T

        W-Y-X
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BT", {{"highway", "secondary"}}},
        {"TY", {{"highway", "secondary"}}}, {"YW", {{"highway", "secondary"}}},
        {"YX", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_lastresort");
    pin_pre_pair_pass(map);
  }
};
gurka::map MotorcycleRoundTripDirtyLastResort::map = {};

TEST_F(MotorcycleRoundTripDirtyLastResort, DirtyLoopServedWhenNothingCleanExists) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "a cell with only defective loops must not 442";
  bool rode_corridor = false;
  for (const auto& edge : paths[0])
    rode_corridor |= (edge == "BT");
  EXPECT_TRUE(rode_corridor) << "the last-resort loop should be the corridor loop";
}

// Adjacent turnarounds B and D converge onto a byte-identical loop (the door-drop
// detour, the T2 dedup case). With B and C ranked as the top two picks and K=2, the
// duplicate must be detected AT BUILD TIME so the queue refills the slot with the
// genuinely distinct C loop — a post-build dedup would silently serve 1 of 2.
class MotorcycleRoundTripDedupRefill : public ::testing::Test {
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
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_dedup_refill");
    pin_pre_pair_pass(map);

    // Rank B (1.0) over D (0.99) over C (0.87): the converging pair (B, D) is the
    // top-2 pick. Curvature edits change harvest RANKING only — the request sends no
    // prefer_curvature, so routing costs and the measured tie-breaks stay put.
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy15, curvy14;
    curvy15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "B")));
    curvy15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "A")));
    curvy14.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "D")));
    curvy14.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "D", "B")));
    test::customize_edges(map.config, [&curvy15, &curvy14](const baldr::GraphId& edgeid,
                                                           baldr::DirectedEdge& edge) {
      if (std::find(curvy15.begin(), curvy15.end(), edgeid) != curvy15.end())
        edge.set_curvature(15);
      else if (std::find(curvy14.begin(), curvy14.end(), edgeid) != curvy14.end())
        edge.set_curvature(14);
    });
  }
};
gurka::map MotorcycleRoundTripDedupRefill::map = {};

TEST_F(MotorcycleRoundTripDedupRefill, ConvergedDuplicateRefillsInsteadOfShrinkingFill) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "14000"},
                                  {"/roundtrip/num_candidates", "2"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  EXPECT_EQ(paths.size(), 2u) << "duplicate convergence must refill, not shrink the fill";
  std::set<std::vector<std::string>> uniq(paths.begin(), paths.end());
  EXPECT_EQ(uniq.size(), paths.size()) << "identical alternates served";
}

// The harvest lands INSIDE a curvy dead-end stub (C-P-Q): the chain carries no U-turn,
// so bounce rejection cannot see it, and every loop built from the stub bounces on the
// return. The junction C below the stub sits outside both harvest bands, so no refill
// candidate can serve its loop — only the trap-aware walk-back (ADR-0037) can retreat
// the tip from Q to C and build the clean C-D-A loop. Without it the cell serves the
// Defect-Gate-stashed bounce loop as a last resort.
class MotorcycleRoundTripWalkBack : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A-B-C--P--Q











    D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CP", {{"highway", "secondary"}}}, {"PQ", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_walkback");
    pin_pre_pair_pass(map);

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "P")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "P", "C")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "P", "Q")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "Q", "P")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripWalkBack::map = {};

TEST_F(MotorcycleRoundTripWalkBack, StubTipWalksBackToTheJunction) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  bool rode_corridor = false, rode_ring = false;
  for (const auto& edge : paths[0]) {
    EXPECT_NE(edge, "PQ") << "the loop entered the dead-end stub";
    rode_corridor |= (edge == "BC");
    rode_ring |= (edge == "CD");
  }
  EXPECT_TRUE(rode_corridor && rode_ring)
      << "expected the walked-back C turnaround loop (A-B-C-D-A)";
  std::set<std::string> uniq(paths[0].begin(), paths[0].end());
  EXPECT_EQ(uniq.size(), paths[0].size()) << "an edge was ridden twice (bounce served)";
}

// The two Second Via tests (OverStemLoopRebuiltTwoLobed, NoViaCandidateKeepsTheOneViaLoop)
// were DELETED with the mechanism in proto/v4-p1 (curvagen-valhalla#10): the census
// measured 0 Second Via rebuilds in 320 Belgrade requests and 0.1 % passes-home over
// 6 622 loops, while the audit's G4 pinned the rebuild as a figure-8 through the rider's
// home whose leg C falls back to no exclusion at all.  ADR-0037 keeps the record.
// The only in-band candidate C closes a 30.6 km loop at a 20 km target (its sole way
// home is the long N arc) — err 0.53, far outside tolerance. The ADR-0037 §2 one-shot
// distance correction must re-aim at the compensated distance (~6.5 km): candidate B
// in the same bearing sector closes a 21 km loop over the M arc instead.
class MotorcycleRoundTripDistCorrection : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A-----B---C






   M

     N
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"BM", {{"highway", "secondary"}}}, {"MA", {{"highway", "secondary"}}},
        {"CN", {{"highway", "secondary"}}}, {"NA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_distcorr");
    pin_pre_pair_pass(map);

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "B", "C")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "B")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripDistCorrection::map = {};

TEST_F(MotorcycleRoundTripDistCorrection, OffTargetBuildReAimedOnce) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  bool corrected_arc = false;
  for (const auto& edge : paths[0]) {
    EXPECT_NE(edge, "CN") << "the 53%-off build was served uncorrected";
    corrected_arc |= (edge == "BM");
  }
  EXPECT_TRUE(corrected_arc) << "expected the re-aimed B loop over the M arc";
}

// The same off-target build with NO candidate in the bearing sector: the correction
// finds nothing and the original loop must stand — a distance miss never costs the
// rider the route.
class MotorcycleRoundTripDistCorrectionKeep : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A---------C






     N
    )";
    const gurka::ways ways = {
        {"AC", {{"highway", "secondary"}}},
        {"CN", {{"highway", "secondary"}}},
        {"NA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {},
                            "test/data/motorcycle_roundtrip_distcorr_keep");
    pin_pre_pair_pass(map);

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "C")));
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, "C", "A")));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map MotorcycleRoundTripDistCorrectionKeep::map = {};

TEST_F(MotorcycleRoundTripDistCorrectionKeep, NoReAimCandidateKeepsTheBuild) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "an uncorrectable distance miss must not cost the route";
  bool long_arc = false;
  for (const auto& edge : paths[0])
    long_arc |= (edge == "CN");
  EXPECT_TRUE(long_arc) << "expected the original off-target loop kept";
}

// Progress-graded rejoin (ADR-0037): the return leg should not shadow the forward
// corridor home on the cheapest crossing. From turnaround T two fresh returns exist:
//   R1 crosses corridor node M near the start (edges QM + MX, 22.8 km — shortest)
//   R2 detours wide via G-H (24 km, touches no forward-path junction)
// With the leash off the grading is off (it scales off the leash surcharge) and R1
// wins on length. With the leash on, M sits at 4/16 of the forward leg, so its
// junction edges carry ~1.75x and the return must take the wide detour R2.
class MotorcycleRoundTripRejoin : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
  W-X

A---M------B----T

    Q------R----S

H---G
    )";
    const gurka::ways ways = {
        {"AM", {{"highway", "secondary"}}}, {"MB", {{"highway", "secondary"}}},
        {"BT", {{"highway", "secondary"}}}, {"TS", {{"highway", "secondary"}}},
        {"RS", {{"highway", "secondary"}}}, {"QR", {{"highway", "secondary"}}},
        {"QM", {{"highway", "secondary"}}}, {"MX", {{"highway", "secondary"}}},
        {"XW", {{"highway", "secondary"}}}, {"WA", {{"highway", "secondary"}}},
        {"QG", {{"highway", "secondary"}}}, {"GH", {{"highway", "secondary"}}},
        {"HA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/motorcycle_roundtrip_rejoin");
    pin_pre_pair_pass(map);

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
gurka::map MotorcycleRoundTripRejoin::map = {};

TEST_F(MotorcycleRoundTripRejoin, GradedRejoinSteersOffTheCorridorCrossing) {
  // Target keeps both return variants inside the distance-correction tolerance
  // (R1 ~38.8 km, R2 ~40 km at 38 km => err <= 0.06) so this map isolates the
  // rejoin grading and the correction stays silent.
  const std::unordered_map<std::string, std::string> base = {
      {"/roundtrip/target_distance", "38000"},
      {"/roundtrip/num_candidates", "1"},
  };
  auto count_edge = [](const std::vector<std::string>& path, const std::string& name) {
    return std::count(path.begin(), path.end(), name);
  };

  // Leash off -> grading off -> shortest fresh return crosses the corridor at M.
  auto off = base;
  off["/costing_options/motorcycle/reuse_penalty"] = "0.0";
  auto r_off = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle", off);
  const auto p_off = gurka::detail::get_paths(r_off);
  ASSERT_GE(p_off.size(), 1u);
  EXPECT_GE(count_edge(p_off[0], "QM"), 1) << "ungraded return should take the short crossing";

  // Leash on -> near-start junction edges graded -> the wide detour wins.
  auto on = base;
  on["/costing_options/motorcycle/reuse_penalty"] = "0.5";
  auto r_on = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle", on);
  const auto p_on = gurka::detail::get_paths(r_on);
  ASSERT_GE(p_on.size(), 1u);
  EXPECT_EQ(count_edge(p_on[0], "QM"), 0)
      << "graded return still crossed the corridor at the near-start junction";
  EXPECT_GE(count_edge(p_on[0], "QG"), 1) << "graded return should take the wide detour";
}

TEST_F(MotorcycleRoundTripNoFreshReturn, FallbackLoopServedNotFailed) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "22000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
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
    pin_pre_pair_pass(map);

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
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
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
    pin_pre_pair_pass(map);

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
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "tip skip must not kill the request";
  // No served loop may enter the dead-end spur: a tip turnaround is a forced spike.
  for (const auto& path : paths)
    for (const auto& edge : path)
      EXPECT_NE(edge, "BT") << "a loop was built to the dead-end tip (out-and-back spike)";
}

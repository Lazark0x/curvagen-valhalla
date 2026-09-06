// Rig confirmation for the 2026-09-05 v3 correctness/optimality audit
// (curvagen-orchestrator #44, docs/curvagen/research/2026-09-05-v3-correctness-optimality-audit.md
// § "Needs rig confirmation").  Every test here is DIAGNOSTIC: several of them are
// EXPECTED TO FAIL — the failure IS the confirmation of the audit's predicted defect.
// Nothing in this file is a regression guard; do not "fix" a failure by weakening it.
//
//   G1  RingReversal          F01  predicted FAIL   (undirected edge ridden twice in legs[0])
//   G1b RingReversalArm       F01  predicted FAIL   (reversal chain out-ranks a clean arm)
//   G2  DualCarriagewayRetrace F02 predicted FAIL   (return rides the opposite carriageway)
//   G3  RejoinMixedClass      F04  predicted FAIL   (no rejoin grade on hierarchy-twin exits)
//   G4  SecondViaFigure8      F06  DELETED with the mechanism (proto/v4-p1)
//   G5  LongFirstEdge         F07  predicted FAIL   (>1.5 km access edge forces a Fallback)
//   G6  RestrictedTurnBounce  F08  predicted FAIL   (artificial dead end -> mid-return bounce)
//   G7  ShortcutPin           F05  predicted PASS   (pins the accident: no served shortcut)
//   F03 ReturnHeuristic       F03  magnitude        (bidir A* return vs near-Dijkstra return)
//
// Idiom follows test/gurka/test_motorcycle_roundtrip.cc: locations = [start, start],
// the roundtrip sub-message carries target_distance + num_candidates, curvature is
// injected with test::customize_edges, and every served loop is a 2-leg TripRoute
// (route_action.cc:2159-2172 — a two-lobe Second Via loop splices leg B + leg C into
// legs[1], route_action.cc:1950-1959).

#include "baldr/graphreader.h"
#include "gurka.h"
#include "thor/road_twin_index.h"
#include "midgard/encoded.h"
#include "midgard/pointll.h"
#include "test.h"

#include <boost/format.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace valhalla;

namespace {

// Same join gurka::detail::to_string does (gurka.cc:411-422); it is not declared in
// gurka.h, so we repeat the two lines here.
std::string join_names(const ::google::protobuf::RepeatedPtrField<::valhalla::StreetName>& sn) {
  std::string str;
  for (const auto& n : sn) {
    if (!str.empty())
      str += "/";
    str += n.value();
  }
  return str;
}

// Way names of one leg, in ride order.  gurka::detail::get_paths() flattens every leg
// of a route into one vector; the audit's assertions are per-leg, so we need this.
std::vector<std::string> leg_names(const valhalla::Api& api, int route_idx, int leg_idx) {
  std::vector<std::string> names;
  const auto& leg = api.trip().routes(route_idx).legs(leg_idx);
  for (const auto& node : leg.node())
    if (node.has_edge())
      names.push_back(join_names(node.edge().name()));
  return names;
}

// Graph ids of one leg's directed edges, in ride order.
std::vector<baldr::GraphId> leg_edge_ids(const valhalla::Api& api, int route_idx, int leg_idx) {
  std::vector<baldr::GraphId> ids;
  const auto& leg = api.trip().routes(route_idx).legs(leg_idx);
  for (const auto& node : leg.node())
    if (node.has_edge())
      ids.emplace_back(node.edge().id());
  return ids;
}

std::map<std::string, int> name_counts(const std::vector<std::string>& names) {
  std::map<std::string, int> c;
  for (const auto& n : names)
    ++c[n];
  return c;
}

std::string dump_counts(const std::map<std::string, int>& c) {
  std::string s;
  for (const auto& [n, k] : c)
    s += n + "x" + std::to_string(k) + " ";
  return s;
}

std::string dump_path(const std::vector<std::string>& names) {
  std::string s;
  for (const auto& n : names)
    s += n + " ";
  return s;
}

std::vector<midgard::PointLL> leg_shape(const valhalla::Api& api, int route_idx, int leg_idx) {
  return midgard::decode<std::vector<midgard::PointLL>>(
      api.trip().routes(route_idx).legs(leg_idx).shape());
}

// The whole ride: legs[0] + legs[1] shapes concatenated (the second leg repeats the seam).
std::vector<midgard::PointLL> ride_shape(const valhalla::Api& api, int route_idx) {
  auto pts = leg_shape(api, route_idx, 0);
  const auto ret = leg_shape(api, route_idx, 1);
  pts.insert(pts.end(), ret.begin() + (ret.empty() ? 0 : 1), ret.end());
  return pts;
}

double ride_length_m(const std::vector<midgard::PointLL>& pts) {
  double d = 0;
  for (size_t i = 1; i < pts.size(); ++i)
    d += pts[i - 1].Distance(pts[i]);
  return d;
}

// Longest exact-mirror stub inside one leg's own shape: the metrics.py find_spikes /
// route_action.cc:1443-1455 test, pts[i-1] == pts[i+1] walked outward.
double self_mirror_stub_m(const std::vector<midgard::PointLL>& pts) {
  double worst = 0;
  for (size_t i = 1; i + 1 < pts.size(); ++i) {
    if (pts[i - 1].Distance(pts[i + 1]) > 1.0)
      continue;
    // apex at i; walk outward while the two sides stay coincident
    double stub = 0;
    size_t a = i, b = i;
    while (a > 0 && b + 1 < pts.size() && pts[a - 1].Distance(pts[b + 1]) <= 1.0) {
      stub += pts[a - 1].Distance(pts[a]);
      --a;
      ++b;
    }
    worst = std::max(worst, stub);
  }
  return worst;
}

// Smallest distance from any point of `probe` to the polyline vertices of `pts`.
double min_dist_to(const std::vector<midgard::PointLL>& pts, const midgard::PointLL& probe) {
  double best = 1e12;
  for (const auto& p : pts)
    best = std::min<double>(best, p.Distance(probe));
  return best;
}

// Cumulative-length-parameterised window [lo, hi] of a ride, as a point list.
std::vector<midgard::PointLL>
ride_window(const std::vector<midgard::PointLL>& pts, double lo_frac, double hi_frac) {
  const double total = ride_length_m(pts);
  std::vector<midgard::PointLL> out;
  double run = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    if (i)
      run += pts[i - 1].Distance(pts[i]);
    const double f = total > 0 ? run / total : 0;
    if (f >= lo_frac && f <= hi_frac)
      out.push_back(pts[i]);
  }
  return out;
}

int count_name(const std::vector<std::string>& names, const std::string& want) {
  return static_cast<int>(std::count(names.begin(), names.end(), want));
}

// Length-weighted mean curvature (0..15) of a leg, read back out of the tiles.
double leg_curviness(baldr::GraphReader& reader, const valhalla::Api& api, int r, int l) {
  double curv = 0, len = 0;
  for (const auto& id : leg_edge_ids(api, r, l)) {
    auto tile = reader.GetGraphTile(id);
    if (!tile)
      continue;
    const auto* de = tile->directededge(id);
    curv += static_cast<double>(de->curvature()) * de->length();
    len += de->length();
  }
  return len > 0 ? curv / len : 0.0;
}

} // namespace

// ---------------------------------------------------------------------------------
// G1 (F01) — ring-reversal harvest chains.
//
//   A-----B-C-DE          AB 6 km, BC 2 km, CD 2 km, ring D-E-F-D ~3.4 km,
//             F           fresh return road B-G-A.
//      G
//
// The ONLY way the harvest can label the homeward directed edges C->B and B->A is to
// ride out to D, go round the D-E-F triangle (a LEGAL reversal — not an immediate
// U-turn, so ADR-0037 bounce rejection does not see it) and come back.  Those chains
// have path distance 15.4 km (at C) and 17.4 km (at B), both inside the +/-18 % band of
// target/2 = 17 km, and both clear the 0.3 straight-line filter.  The served forward
// leg therefore rides CD and BC twice each, inside legs[0], where neither the Defect
// Gate's mirror test nor the stem check can see it.
// ---------------------------------------------------------------------------------
class RtAuditRingReversal : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A-----B-C-DE
                 F

         G
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DE", {{"highway", "secondary"}}},
        {"EF", {{"highway", "secondary"}}}, {"FD", {{"highway", "secondary"}}},
        {"BG", {{"highway", "secondary"}}}, {"GA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_ring_reversal");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"C", "D"},
                                                                               {"D", "C"},
                                                                               {"D", "E"},
                                                                               {"E", "D"},
                                                                               {"E", "F"},
                                                                               {"F", "E"},
                                                                               {"F", "D"},
                                                                               {"D", "F"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditRingReversal::map = {};

TEST_F(RtAuditRingReversal, G1_ForwardLegIsNotEdgeSimple) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "34000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  ASSERT_EQ(result.trip().routes(0).legs_size(), 2);

  const auto fwd = leg_names(result, 0, 0);
  const auto ret = leg_names(result, 0, 1);
  const auto counts = name_counts(fwd);
  std::cerr << "[G1] legs[0] = " << dump_path(fwd) << "\n";
  std::cerr << "[G1] legs[1] = " << dump_path(ret) << "\n";
  std::cerr << "[G1] legs[0] counts = " << dump_counts(counts) << "\n";

  // (a) PREDICTED FAIL: no undirected way name twice inside the forward leg.
  for (const auto& [name, n] : counts)
    EXPECT_EQ(n, 1) << "F01 CONFIRMED: forward leg rides way " << name << " " << n
                    << "x — the harvest chain reversed on the ring at D. legs[0] = "
                    << dump_path(fwd);

  // (b) the map still discriminates: the served RIDE reaches the D-E-F ring.  (Before
  //     proto/v4-p1 the ring sat inside legs[0] because the harvest chain reversed on
  //     it; with non-simple chains rejected the ring is crossed once, at the seam.)
  //
  //     proto/v4-p1.1 REPAIR.  The ring loop reaches the ring only on a soft-leash
  //     return that retraces the corridor, and the geometry Defect Gate now rejects
  //     exactly that.  With the gate on the engine refills the single slot with the
  //     clean B-G-A loop — the intended behaviour, and a better ride.  So (b) is now
  //     asserted against the GATE-OFF control, which is what actually pins F01: the
  //     ring is still reachable, the chain that reaches it is still edge-simple.
  map.config.put("thor.roundtrip_geometry_gate", false);
  auto ungated = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                  {{"/roundtrip/target_distance", "34000"},
                                   {"/roundtrip/num_candidates", "1"},
                                   {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  map.config.put("thor.roundtrip_geometry_gate", true);
  ASSERT_GE(ungated.trip().routes_size(), 1) << "gate-off control served no loop";
  const auto ufwd = leg_names(ungated, 0, 0);
  const auto uret = leg_names(ungated, 0, 1);
  std::cerr << "[G1] gate-off control legs[0] = " << dump_path(ufwd) << "\n";
  std::cerr << "[G1] gate-off control legs[1] = " << dump_path(uret) << "\n";
  for (const auto& [name, n] : name_counts(ufwd))
    EXPECT_EQ(n, 1) << "F01 CONFIRMED in the gate-off control: forward leg rides " << name
                    << " " << n << "x; legs[0] = " << dump_path(ufwd);
  const auto ride = [&] {
    std::vector<std::string> v = ufwd;
    v.insert(v.end(), uret.begin(), uret.end());
    return name_counts(v);
  }();
  EXPECT_GE(ride.count("EF") ? ride.at("EF") : 0, 1)
      << "expected the D-E-F ring somewhere in the gate-off control's ride";
  // ...and with the gate on, the slot goes to the clean loop instead.
  EXPECT_EQ(ride.count("BG") ? ride.at("BG") : 0, 0)
      << "gate-off control took the clean arm too — the map no longer discriminates";
  const auto gated_ride = [&] {
    std::vector<std::string> v = fwd;
    v.insert(v.end(), ret.begin(), ret.end());
    return name_counts(v);
  }();
  EXPECT_GE(gated_ride.count("BG") ? gated_ride.at("BG") : 0, 1)
      << "the geometry gate did not refill the slot with the clean B-G-A loop; ride = "
      << dump_path(fwd) << "| " << dump_path(ret);
}

// G1 variant: a clean curvature-10 arm B-H (pd 16 km, in band) competes with the
// reversal chain (pd 15.4 km).  Ranking is curviness_per_km over the WHOLE chain, and
// the retraced curvy kilometres count twice (roundtrip_expansion.cc:168), so the
// reversal chain scores 0.48 against the clean arm's 0.42 and takes the only slot.
class RtAuditRingReversalArm : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A-----B-C-DE
                 F

         G









      I     H
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DE", {{"highway", "secondary"}}},
        {"EF", {{"highway", "secondary"}}}, {"FD", {{"highway", "secondary"}}},
        {"BG", {{"highway", "secondary"}}}, {"GA", {{"highway", "secondary"}}},
        {"BH", {{"highway", "secondary"}}}, {"HI", {{"highway", "secondary"}}},
        {"IA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_ring_reversal_arm");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy15, curvy10;
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"C", "D"},
                                                                               {"D", "C"},
                                                                               {"D", "E"},
                                                                               {"E", "D"},
                                                                               {"E", "F"},
                                                                               {"F", "E"},
                                                                               {"F", "D"},
                                                                               {"D", "F"}})
      curvy15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"B", "H"}, {"H", "B"}})
      curvy10.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config, [&curvy15, &curvy10](const baldr::GraphId& edgeid,
                                                           baldr::DirectedEdge& edge) {
      if (std::find(curvy15.begin(), curvy15.end(), edgeid) != curvy15.end())
        edge.set_curvature(15);
      if (std::find(curvy10.begin(), curvy10.end(), edgeid) != curvy10.end())
        edge.set_curvature(10);
    });
  }
};
gurka::map RtAuditRingReversalArm::map = {};

TEST_F(RtAuditRingReversalArm, G1b_CleanArmLosesToTheReversalChain) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "34000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  const auto fwd = leg_names(result, 0, 0);
  std::cerr << "[G1b] legs[0] = " << dump_path(fwd) << "\n";
  std::cerr << "[G1b] legs[1] = " << dump_path(leg_names(result, 0, 1)) << "\n";

  // PREDICTED FAIL: the clean curvature-10 arm should win the single slot.
  EXPECT_GE(count_name(fwd, "BH"), 1)
      << "F01 CONFIRMED: the ring-reversal chain out-ranked the clean arm B-H "
         "(curviness_per_km counts the retraced curvy km twice). legs[0] = "
      << dump_path(fwd);
}

// ---------------------------------------------------------------------------------
// G2 (F02) — parallel-carriageway retrace.  30 m/char.
//
//   A>>>>>>>>>>>>>>B      one-way east, 3 km, curvature 15
//   D<<<<<<<<<<<<<<C      one-way west, 3 km, curvature 15, 30 m south
//   links B-C (east end) and D-A (west end); fresh two-way road B-E-F-A 600 m south.
//
// route_leg excludes e and GetOpposingEdgeId(e) only (route_action.cc:1625-1635); the
// opposite carriageway is a different OSM way with different nodes, so nothing bars it.
// The return U-turns at B onto the westbound carriageway and rides home 30 m from the
// forward leg — the same physical road, invisible to the seam-window mirror test.
// ---------------------------------------------------------------------------------
class RtAuditDualCarriageway : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // 100 chars @ 30 m = 3 km carriageways; row 1 is 30 m south of row 0.
    const std::string ascii_map = R"(
    A---------------------------------------------------------------------------------------------------B
    D---------------------------------------------------------------------------------------------------C




    F---------------------------------------------------------------------------------------------------E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"CD", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"BC", {{"highway", "primary"}}},
        {"DA", {{"highway", "primary"}}},
        {"BE", {{"highway", "secondary"}}},
        {"EF", {{"highway", "secondary"}}},
        {"FA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 30);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_dual_carriageway");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"A", "B"}, {"C", "D"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditDualCarriageway::map = {};

TEST_F(RtAuditDualCarriageway, G2_ReturnRidesTheOppositeCarriageway) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "6000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.8"},
                                  {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  ASSERT_EQ(result.trip().routes(0).legs_size(), 2);

  const auto fwd = leg_names(result, 0, 0);
  const auto ret = leg_names(result, 0, 1);
  std::cerr << "[G2] legs[0] = " << dump_path(fwd) << "\n";
  std::cerr << "[G2] legs[1] = " << dump_path(ret) << "\n";

  // (a) PREDICTED FAIL: the return should use the fresh southern road, not the twin.
  EXPECT_EQ(count_name(ret, "CD"), 0)
      << "F02 CONFIRMED: the return rode the opposite carriageway CD — the hard "
         "exclusion set never learns that AB and CD are the same physical road. "
         "legs[1] = "
      << dump_path(ret);
  EXPECT_GE(count_name(ret, "EF"), 1)
      << "expected the fresh road E-F on the return; legs[1] = " << dump_path(ret);

  // (b) PREDICTED FAIL: geometric proximity — beyond the 1500 m Start Exemption, no
  //     return vertex may sit within 40 m of a forward vertex.
  const auto fshape = leg_shape(result, 0, 0);
  const auto rshape = leg_shape(result, 0, 1);
  const auto start = map.nodes.at("A");
  // The turnaround node itself belongs to BOTH legs, so a disc around the seam has to
  // come out of this measurement or it can only ever read 0 m (as it did pre-P1, where
  // the audit recorded "0 m — the shared turnaround node").  100 m is one carriageway
  // link; everything the F02 class is about lives far outside it.
  const auto seam = map.nodes.at("B");
  double closest = 1e12;
  for (const auto& rp : rshape) {
    if (rp.Distance(start) < 1500.0 || rp.Distance(seam) < 100.0)
      continue; // Start Exemption disc / seam disc
    closest = std::min(closest, min_dist_to(fshape, rp));
  }
  std::cerr << "[G2] closest return-to-forward vertex beyond the exemption disc = " << closest
            << " m\n";
  EXPECT_GT(closest, 40.0)
      << "F02 CONFIRMED: the return runs " << closest
      << " m from the forward leg beyond the Start Exemption — a parallel retrace the "
         "mirror test cannot see";
}

// ---------------------------------------------------------------------------------
// G3 (F04) — rejoin grades miss hierarchy-twin exits.
//
// The MotorcycleRoundTripRejoin map with the corridor promoted to `primary` (level 0)
// and the near-start crossing QM/MX demoted to `unclassified` (level 2).  The grade
// builder reads only the corridor node's OWN level NodeInfo (route_action.cc:1636-1657),
// so M's level-2 twin — which is where QM and MX live — is never enumerated and the
// crossing is never surcharged.  With the leash on, the all-secondary original passes;
// this class-mixed version is predicted to fail.
// ---------------------------------------------------------------------------------
class RtAuditRejoinMixedClass : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
  W-X

A---M------B----T

    Q------R----S

H---G
    )";
    // Every way carries the same maxspeed so that ROAD CLASS — i.e. hierarchy level —
    // is the only thing that differs between the crossing and the detour.  Without this
    // the unclassified default speed alone would send the return the long way and the
    // test would prove nothing about the rejoin grades.
    const gurka::ways ways = {
        // corridor: PRIMARY (level 0)
        {"AM", {{"highway", "primary"}, {"maxspeed", "80"}}},
        {"MB", {{"highway", "primary"}, {"maxspeed", "80"}}},
        {"BT", {{"highway", "primary"}, {"maxspeed", "80"}}},
        // the near-start crossing: UNCLASSIFIED (level 2) — M's hierarchy twin
        {"QM", {{"highway", "unclassified"}, {"maxspeed", "80"}}},
        {"MX", {{"highway", "unclassified"}, {"maxspeed", "80"}}},
        // the rest as in the original
        {"TS", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"RS", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"QR", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"XW", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"WA", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"QG", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"GH", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"HA", {{"highway", "secondary"}, {"maxspeed", "80"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_rejoin_mixed");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"B", "T"}, {"T", "B"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditRejoinMixedClass::map = {};

TEST_F(RtAuditRejoinMixedClass, G3_GradeMissingOnHierarchyTwinExits) {
  // Control: leash OFF => grading off => the shortest fresh return must cross at M.
  // If this control does not take QM the map is not discriminating and the graded run
  // below proves nothing.
  auto ctl = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                              {{"/roundtrip/target_distance", "38000"},
                               {"/roundtrip/num_candidates", "1"},
                               {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  ASSERT_GE(ctl.trip().routes_size(), 1) << "control served no loop";
  const auto ctl_ret = leg_names(ctl, 0, 1);
  std::cerr << "[G3] CONTROL (leash off) legs[1] = " << dump_path(ctl_ret) << "\n";
  EXPECT_GE(count_name(ctl_ret, "QM"), 1)
      << "MAP NOT DISCRIMINATING: even ungraded, the return avoided the M crossing — the "
         "graded assertion below cannot distinguish a working grade from a map artefact. "
         "control legs[1] = "
      << dump_path(ctl_ret);

  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "38000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.5"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  const auto ret = leg_names(result, 0, 1);
  std::cerr << "[G3] legs[0] = " << dump_path(leg_names(result, 0, 0)) << "\n";
  std::cerr << "[G3] legs[1] = " << dump_path(ret) << "\n";

  // PREDICTED FAIL: the same assertion the all-secondary original passes.
  EXPECT_EQ(count_name(ret, "QM"), 0)
      << "F04 CONFIRMED: the graded return still crossed the corridor at M — QM/MX live "
         "on M's level-2 twin and the rejoin builder enumerates only M's own level. "
         "legs[1] = "
      << dump_path(ret);
  EXPECT_GE(count_name(ret, "QG"), 1)
      << "expected the wide detour Q-G-H-A; legs[1] = " << dump_path(ret);
}

// ---------------------------------------------------------------------------------
// G4 (F06) — DELETED with the Second Via mechanism (proto/v4-p1, curvagen-valhalla#10).
// The census found Second Via structurally unreachable on rider demand (0 rebuilds in
// 320 Belgrade requests) while G4 pinned it as a figure-8 through the rider's home, so
// P1 removes the mechanism rather than fixing it.  ADR-0037 keeps the record.
// ---------------------------------------------------------------------------------
// G5 (F07) — a long first edge forces a Fallback.
//
//   A--B------C     The CulDeSac map with AB stretched to 3 km.  Exclusion granularity
//      |      |     is the whole edge: AB's cumulative forward distance ENDS at 3000 m,
//      E------D     beyond kStartExemptionMeters 1500, so the only access road is barred
//                   whole and the return's destination is unreachable in the primary
// search.  The Fallback then drops EVERY hard exclusion, so the primary corridor may be
// retraced on the soft leash alone.
// ---------------------------------------------------------------------------------
// Both variants are cul-de-sac starts: A's ONLY road is AB, so the return's destination
// side stands or falls with that one edge.  The corridor is A-B-C-D (14 km / 12 km) and
// the single fresh way home is D-E-B-A, deliberately built 4 km LONGER than retracing
// the corridor.  The comparison is therefore pure length, no cost modelling:
//   hard exclusion in force   => the corridor is barred => the long fresh road home
//   Fallback (exclusion gone) => the shorter retrace D-C-B-A wins => corridor ridden 2x
// The two fixtures differ only in the length of the access edge AB (3 km vs 1 km),
// i.e. only in whether AB's cumulative path distance ends beyond kStartExemptionMeters.
static void build_culdesac(gurka::map& m, const std::string& ascii_map, const std::string& dir) {
  const gurka::ways ways = {
      {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
      {"CD", {{"highway", "secondary"}}}, {"DE", {{"highway", "secondary"}}},
      {"EB", {{"highway", "secondary"}}},
  };
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
  // ADR-0037 stage-timing ledger prints "fallbacks=N", which is the only way to observe
  // from outside the engine whether the hard-exclusion pass failed (F12: the fell_back
  // tag never reaches the response).
  m = gurka::buildtiles(layout, ways, {}, {}, dir, {{"thor.roundtrip_stage_timing", "true"}});

  auto reader = test::make_clean_graphreader(m.config.get_child("mjolnir"));
  std::vector<baldr::GraphId> curvy;
  for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"C", "D"},
                                                                             {"D", "C"},
                                                                             {"B", "C"},
                                                                             {"C", "B"}})
    curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
  test::customize_edges(m.config, [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
    if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
      edge.set_curvature(15);
  });
}

// Diagnostic shared by the two G5 fixtures. Returns the whole-ride way counts.
static std::map<std::string, int>
run_culdesac(const gurka::map& m, const std::string& target, const char* tag) {
  auto result = gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", target},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"},
                                  {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  EXPECT_GE(result.trip().routes_size(), 1) << tag << ": cell must not fail the request";
  if (result.trip().routes_size() < 1)
    return {};
  const auto fwd = leg_names(result, 0, 0);
  const auto ret = leg_names(result, 0, 1);
  std::vector<std::string> all = fwd;
  all.insert(all.end(), ret.begin(), ret.end());
  std::cerr << tag << " legs[0] = " << dump_path(fwd) << "\n";
  std::cerr << tag << " legs[1] = " << dump_path(ret) << "\n";
  std::cerr << tag << " whole-ride counts = " << dump_counts(name_counts(all)) << "\n";
  return name_counts(all);
}

// CONTROL: AB = 1 km, entirely inside the 1500 m Start Exemption.
class RtAuditShortFirstEdge : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // A(0,0) B(1,0) C(10,0) D(10,2) E(5,7): AB 1 km, BC 9 km, CD 2 km => corridor 12 km;
    // fresh D-E-B 15.1 km, so the fresh way home is 4 km longer than the retrace.
    build_culdesac(map, R"(
AB--------C

          D




     E
)",
                   "test/data/rt_audit_short_first_edge");
  }
};
gurka::map RtAuditShortFirstEdge::map = {};

TEST_F(RtAuditShortFirstEdge, G5_ControlShortAccessEdgeKeepsHardExclusion) {
  const auto counts = run_culdesac(map, "28000", "[G5-ctl]");
  ASSERT_FALSE(counts.empty());
  EXPECT_EQ(counts.count("AB") ? counts.at("AB") : 0, 2)
      << "the exempt access road must carry the loop out and home";
  for (const auto& [name, n] : counts)
    if (name != "AB")
      EXPECT_EQ(n, 1) << "CONTROL BROKEN: with a 1 km access edge the exemption holds and the "
                         "hard exclusion should have forced the long fresh road home, yet "
                      << name << " was ridden " << n << "x";
}

// PREDICTED FAIL: AB = 3 km, so the access edge's cumulative distance ENDS at 3000 m and
// exclusion granularity (the whole edge) bars the only road home.
class RtAuditLongFirstEdge : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // A(0,0) B(3,0) C(12,0) D(12,2) E(7,7): AB 3 km, BC 9 km, CD 2 km => corridor 14 km;
    // fresh D-E-B 15.1 km (+ AB = 18.1 km), so the retrace is 4 km cheaper the moment the
    // exclusion is dropped.
    build_culdesac(map, R"(
A--B--------C

            D




       E
)",
                   "test/data/rt_audit_long_first_edge");
  }
};
gurka::map RtAuditLongFirstEdge::map = {};

TEST_F(RtAuditLongFirstEdge, G5_LongAccessEdgeForcesACorridorRetrace) {
  const auto counts = run_culdesac(map, "32000", "[G5]");
  ASSERT_FALSE(counts.empty());
  EXPECT_EQ(counts.count("AB") ? counts.at("AB") : 0, 2)
      << "the access road must carry the loop out and home";
  // PREDICTED FAIL: everything else exactly once.
  for (const auto& [name, n] : counts)
    if (name != "AB")
      EXPECT_EQ(n, 1) << "F07 CONFIRMED: " << name << " was ridden " << n
                      << "x — the 3 km access edge ends beyond the 1500 m exemption, is "
                         "barred whole, and the forced Fallback retraced the curvy corridor "
                         "(the control with a 1 km access edge does not)";
}

// ---------------------------------------------------------------------------------
// G6 (F08) — artificial dead ends and the restricted-turn bounce.
//
//   A-----K-----T      Forward A-K-T (curvature 15 on KT).  Fresh return T-P-K, but a
//         U            no_left_turn relation forbids P->K->A.  K's other non-U-turn
//   (P south-east,     exits are the hard-excluded corridor edges KT/KA-as-from-PK, so
//    W far south-west) the return search sees K as a dead end, dives into the 1 km spur
//                      K-U, U-turns there (bidirectional_astar.cc:439-458 marks the
// predecessor deadend), comes back on U->K, and the restriction — which is keyed on the
// from-edge — no longer applies.  The long way round P-W-A is 16.9 km against a 2 km
// bounce, so the bounce wins.  A mid-return mirror is outside the seam window.
// ---------------------------------------------------------------------------------
class RtAuditRestrictedTurnBounce : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // 1000 m/char.  The geometry keeps T the top candidate (as the earlier run showed);
    // the cost margin comes from the SPEED of the long way round, not its length, so
    // that W and P stay where they were:
    //   bounce  P-K 4.5 km + K-U-K 2.0 km + K-A 6.0 km  @ 80 km/h  ~   561 s
    //   long    P-W 7.6 km + W-A 5.8 km                 @ 15 km/h  ~  3227 s
    // a 5.7x margin, so if the artificial dead end at K is exploitable at all the bounce
    // must win.
    const std::string ascii_map = R"(
      A-----K-----T
            U
                P


         W
    )";
    const gurka::ways ways = {
        {"AK", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"KT", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"KU", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"TP", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        {"PK", {{"highway", "secondary"}, {"maxspeed", "80"}}},
        // the long way round is legal but crawling — the bounce must be the cheap option
        {"PW", {{"highway", "secondary"}, {"maxspeed", "15"}}},
        {"WA", {{"highway", "secondary"}, {"maxspeed", "15"}}},
    };
    const gurka::relations relations = {
        {{
             {gurka::way_member, "PK", "from"},
             {gurka::way_member, "AK", "to"},
             {gurka::node_member, "K", "via"},
         },
         {
             {"type", "restriction"},
             {"restriction", "no_left_turn"},
         }},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, relations, "test/data/rt_audit_restricted_bounce");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"K", "T"}, {"T", "K"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditRestrictedTurnBounce::map = {};

TEST_F(RtAuditRestrictedTurnBounce, G6_MidReturnBounceThroughTheSpur) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "24000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  const auto ret = leg_names(result, 0, 1);
  std::cerr << "[G6] legs[0] = " << dump_path(leg_names(result, 0, 0)) << "\n";
  std::cerr << "[G6] legs[1] = " << dump_path(ret) << "\n";

  const double stub = self_mirror_stub_m(leg_shape(result, 0, 1));
  std::cerr << "[G6] longest exact-mirror stub inside legs[1] = " << stub << " m\n";
  std::cerr << "[G6] legs[1] counts = " << dump_counts(name_counts(ret)) << "\n";

  // PREDICTED FAIL (if the bounce is cheaper than the long way round).
  EXPECT_LT(stub, 30.0)
      << "F08 CONFIRMED: the return bounced " << stub
      << " m into the spur K-U — hard exclusion made K an artificial dead end and the "
         "restricted turn was laundered through the U-turn. legs[1] = "
      << dump_path(ret);
  EXPECT_EQ(count_name(ret, "KU"), 0)
      << "the return dived into the dead-end spur; legs[1] = " << dump_path(ret);
}

// G6 disambiguation.  The same map with the long way round DELETED: after the restricted
// turn the spur bounce is the only route home at all.  This separates "the bounce lost on
// cost" from "the dead-end U-turn is simply not available to the return search".
class RtAuditRestrictedTurnNoWayRound : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // proto/v4-p1.1: the map gains a SECOND, clean lobe (A-M-N-A, south-west).  The
    // T lobe is unchanged and still has no way round — the bounce is still the only
    // route home from T — but the cell now has an alternative, which is what the
    // geometry Defect Gate needs to do its job: reject the bouncing loop and refill
    // the slot.  Without an alternative the gate can only stash the loop and serve it
    // as the last resort, and "no route" is not an option the engine may take.
    // G6c below keeps the original degenerate map and pins exactly that contract.
    const std::string ascii_map = R"(
      A-----K-----T
            U
                P



             N





      M
    )";
    const gurka::ways ways = {
        {"AK", {{"highway", "secondary"}}},
        {"KT", {{"highway", "secondary"}}},
        {"KU", {{"highway", "secondary"}}},
        // proto/v4-p1.1: the clean lobe.  A-M is 13 km due south (pd 13 000, inside the
        // +/-18 % band of target/2 = 13 500); M-N-A closes it on fresh road.
        {"AM", {{"highway", "secondary"}}},
        {"MN", {{"highway", "secondary"}}},
        {"NA", {{"highway", "secondary"}}},
        // one-way T->P so the loop CANNOT simply be ridden the other way round:
        // after the restricted P->K->A turn the spur bounce is the only route home.
        {"TP", {{"highway", "secondary"}, {"oneway", "yes"}}},
        {"PK", {{"highway", "secondary"}}},
    };
    const gurka::relations relations = {
        {{
             {gurka::way_member, "PK", "from"},
             {gurka::way_member, "AK", "to"},
             {gurka::node_member, "K", "via"},
         },
         {
             {"type", "restriction"},
             {"restriction", "no_left_turn"},
         }},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, relations, "test/data/rt_audit_restricted_only");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"K", "T"}, {"T", "K"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditRestrictedTurnNoWayRound::map = {};

TEST_F(RtAuditRestrictedTurnNoWayRound, G6b_IsTheDeadEndUTurnAvailableAtAll) {
  valhalla::Api result;
  try {
    result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                              {{"/roundtrip/target_distance", "27000"},
                               {"/roundtrip/num_candidates", "1"},
                               {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  } catch (const std::exception& e) {
    std::cerr << "[G6b] request threw: " << e.what()
              << " => the dead-end U-turn is NOT available to the return search\n";
    return;
  }
  if (result.trip().routes_size() < 1) {
    std::cerr << "[G6b] no loop served => the dead-end U-turn is NOT available\n";
    return;
  }
  const auto ret = leg_names(result, 0, 1);
  std::cerr << "[G6b] legs[0] = " << dump_path(leg_names(result, 0, 0)) << "\n";
  std::cerr << "[G6b] legs[1] = " << dump_path(ret) << "\n";
  const double stub = self_mirror_stub_m(leg_shape(result, 0, 1));
  std::cerr << "[G6b] KU count in legs[1] = " << count_name(ret, "KU")
            << ", longest exact-mirror stub = " << stub << " m\n";
  // proto/v4-p1.1: the geometry Defect Gate decodes the WHOLE return leg, so the
  // mid-return mirror is now visible; the loop is rejected and the slot refilled with
  // the clean A-M-N-A lobe.  Under P1 (seam window only) this served the bounce.
  EXPECT_EQ(count_name(ret, "KU"), 0)
      << "F08 STILL OPEN: the return bounced into the dead-end spur K-U and the loop was "
         "served anyway — a mid-return exact mirror of "
      << stub << " m that the seam-window verdict cannot see. legs[1] = " << dump_path(ret);
  EXPECT_LT(stub, 30.0) << "served return still carries a mid-leg exact mirror of " << stub
                        << " m; legs[1] = " << dump_path(ret);
}

// ---------------------------------------------------------------------------------
// G6c — the last-resort contract.  The ORIGINAL G6b map: one lobe, and after the
// restricted turn the spur bounce is the only route home at all.  The gate rejects the
// loop; there is nothing to refill it with; the engine must still serve it rather than
// return a 442.  This is the boundary the gate must not cross.
// ---------------------------------------------------------------------------------
class RtAuditRestrictedTurnOnlyLoop : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A-----K-----T
            U
                P
    )";
    const gurka::ways ways = {
        {"AK", {{"highway", "secondary"}}},
        {"KT", {{"highway", "secondary"}}},
        {"KU", {{"highway", "secondary"}}},
        {"TP", {{"highway", "secondary"}, {"oneway", "yes"}}},
        {"PK", {{"highway", "secondary"}}},
    };
    const gurka::relations relations = {
        {{
             {gurka::way_member, "PK", "from"},
             {gurka::way_member, "AK", "to"},
             {gurka::node_member, "K", "via"},
         },
         {
             {"type", "restriction"},
             {"restriction", "no_left_turn"},
         }},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, relations, "test/data/rt_audit_restricted_only_loop");
  }
};
gurka::map RtAuditRestrictedTurnOnlyLoop::map = {};

TEST_F(RtAuditRestrictedTurnOnlyLoop, G6c_GateNeverStarvesTheOnlyLoop) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "27000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.0"}});
  ASSERT_GE(result.trip().routes_size(), 1)
      << "P1.1 REGRESSION: the geometry gate rejected the cell's only loop and the "
         "engine served nothing — dirty-last-resort must still fire";
  std::cerr << "[G6c] legs[1] = " << dump_path(leg_names(result, 0, 1)) << "\n";
}

// ---------------------------------------------------------------------------------
// G7 (F05) — shortcut pin.  Long same-class chains on level 0/1 produce shortcut
// edges in the tiles.  Because the round-trip branch never applies hierarchy limits,
// BidirectionalAStar::Init sets ignore_hierarchy_limits_ and shortcuts are never
// expanded (bidirectional_astar.cc:202-205), so the base-id-only hard exclusion set is
// complete BY ACCIDENT.  This test pins the accident: it must PASS today, and it is the
// canary if v4 or an upstream default ever hands round-trip finite hierarchy limits.
// ---------------------------------------------------------------------------------
class RtAuditShortcutPin : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    // The loop A..G-H..N-A is what gets ridden; the dead-end spur G-O-P-Q-R-S is a
    // straight same-name chain whose interior nodes (O,P,Q,R) are contractible, so the
    // TILE SET carries shortcuts even if the closed loop itself yields none
    // (cf. test_shortcut.cc `Shortcuts.LoopWithoutShortcut`).
    const std::string ascii_map = R"(
                        O--P--Q--R--S
      A--B--C--D--E--F--G
      |                 |
      N--M--L--K--J--I--H
    )";
    // Shortcut contraction needs the chained edges to look like ONE road: same class,
    // same speed and the same `name` tag (test/gurka/test_shortcut.cc idiom).  Without
    // the shared name mjolnir cannot contract B..F / I..M and the pin is vacuous.
    // test_shortcut.cc `Shortcuts.CreateValid` recipe: same class, same name and explicit
    // directional maxspeeds, so mjolnir can contract the interior nodes.
    auto road = [](const char* n) {
      return std::map<std::string, std::string>{{"highway", "primary"},
                                                {"name", n},
                                                {"maxspeed:forward", "80"},
                                                {"maxspeed:backward", "80"}};
    };
    const gurka::ways ways = {
        {"AB", road("North Road")},
        {"BC", road("North Road")},
        {"CD", road("North Road")},
        {"DE", road("North Road")},
        {"EF", road("North Road")},
        {"FG", road("North Road")},
        {"GH", road("East Road")},
        {"HI", road("South Road")},
        {"IJ", road("South Road")},
        {"JK", road("South Road")},
        {"KL", road("South Road")},
        {"LM", road("South Road")},
        {"MN", road("South Road")},
        {"NA", road("West Road")},
        // contractible dead-end spur
        {"GO", road("Spur Road")},
        {"OP", road("Spur Road")},
        {"PQ", road("Spur Road")},
        {"QR", road("Spur Road")},
        {"RS", road("Spur Road")},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 500);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_shortcut_pin");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"D", "E"}, {"E", "D"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtAuditShortcutPin::map = {};

TEST_F(RtAuditShortcutPin, G7_NoServedLegEdgeIsAShortcut) {
  baldr::GraphReader reader(map.config.get_child("mjolnir"));

  // The pin is only meaningful if the tiles actually carry shortcuts.
  size_t shortcuts_in_tiles = 0;
  for (const auto& tile_id : reader.GetTileSet()) {
    auto tile = reader.GetGraphTile(tile_id);
    if (!tile)
      continue;
    size_t here = 0;
    for (uint32_t i = 0; i < tile->header()->directededgecount(); ++i)
      if (tile->directededge(i)->is_shortcut())
        ++here;
    std::cerr << "[G7]   tile " << tile_id << " level " << tile_id.level() << ": "
              << tile->header()->directededgecount() << " directed edges, " << here << " shortcuts\n";
    shortcuts_in_tiles += here;
  }
  {
    auto base = std::get<0>(gurka::findEdgeByNodes(reader, map.nodes, "B", "C"));
    auto sc = reader.GetShortcut(base);
    std::cerr << "[G7] GetShortcut(BC) valid = " << sc.is_valid() << "\n";
  }
  std::cerr << "[G7] shortcut directed edges in the tile set = " << shortcuts_in_tiles << "\n";
  EXPECT_GT(shortcuts_in_tiles, 0u) << "no shortcuts were built — the pin would be vacuous";

  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "20000"},
                                  {"/roundtrip/num_candidates", "3"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.8"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";

  size_t served_shortcuts = 0, served_edges = 0;
  for (int r = 0; r < result.trip().routes_size(); ++r) {
    for (int l = 0; l < result.trip().routes(r).legs_size(); ++l) {
      for (const auto& id : leg_edge_ids(result, r, l)) {
        auto tile = reader.GetGraphTile(id);
        if (!tile)
          continue;
        ++served_edges;
        if (tile->directededge(id)->is_shortcut())
          ++served_shortcuts;
      }
    }
  }
  std::cerr << "[G7] served edges = " << served_edges << ", of which shortcuts = " << served_shortcuts
            << " (routes = " << result.trip().routes_size() << ")\n";
  EXPECT_EQ(served_shortcuts, 0u)
      << "F05 BROKEN: a served leg contains a shortcut edge — the base-id-only hard "
         "exclusion set is no longer complete with respect to the graph the return searches";
}

// ---------------------------------------------------------------------------------
// F03 magnitude — the return-leg A* heuristic is not admissible under the fork costing.
//
// AStarCostFactor() returns kSpeedFactor[top_speed_] * min_linear_cost_factor_, i.e.
// pure time at top speed, while the true edge cost is sec * factor with factor as low
// as 0.523 (c0.5) / 0.343 (c0.8).  min_linear_cost_factor_ is the ONE public lever on
// that heuristic: it is the smallest `linear_cost_factors` factor in the request
// (dynamiccost.cc:227-246).  Attaching a tiny factor to a DECOY road in a disconnected
// component drives the heuristic to ~0 without changing any cost on the reachable
// graph, which turns the return-leg bidirectional A* into an (admissible) Dijkstra.
//
// Same request, same costing, twice: stock heuristic vs near-zero heuristic.  Any
// return leg whose cost drops under the exact search is a proven optimality loss.
//
//   A---B---C---D      corridor east (curvature 15 on CD)
//   |    \           the return home may go the straight way (F-A, curvature 0) or the
//   E-F-G-H          curvy-but-longer southern chain; the greedy-toward-home heuristic
//   (Z-Y decoy, disconnected)
// ---------------------------------------------------------------------------------
class RtAuditReturnHeuristic : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A-----B-----C-----D


      E-----F-----G-----H

      J-----K-----L-----M

                              Z-----Y
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}},
        {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}},
        {"DH", {{"highway", "secondary"}}},
        {"HG", {{"highway", "secondary"}}},
        {"GF", {{"highway", "secondary"}}},
        {"FE", {{"highway", "secondary"}}},
        {"EA", {{"highway", "secondary"}}},
        {"HM", {{"highway", "secondary"}}},
        {"ML", {{"highway", "secondary"}}},
        {"LK", {{"highway", "secondary"}}},
        {"KJ", {{"highway", "secondary"}}},
        {"JE", {{"highway", "secondary"}}},
        // disconnected decoy: carries the linear cost factor, can never be ridden.
        {"ZY", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_audit_return_heuristic",
                            {{"service_limits.min_linear_cost_factor", "0.00001"}});

    // The southern chain J-K-L-M is the curvy (discounted) way home; the middle row is
    // the geometrically direct one.
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"C", "D"},
                                                                               {"D", "C"},
                                                                               {"H", "M"},
                                                                               {"M", "H"},
                                                                               {"M", "L"},
                                                                               {"L", "M"},
                                                                               {"L", "K"},
                                                                               {"K", "L"},
                                                                               {"K", "J"},
                                                                               {"J", "K"},
                                                                               {"J", "E"},
                                                                               {"E", "J"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }

  // One round-trip request; `decoy_factor` <= 0 means "stock heuristic".
  static valhalla::Api run(double prefer_curvature, double decoy_factor) {
    std::string decoy;
    if (decoy_factor > 0) {
      std::vector<midgard::PointLL> shape{map.nodes.at("Z"), map.nodes.at("Y")};
      decoy =
          (boost::format(R"("linear_cost_factors":[{"shape":"%s","factor":%s}],)") %
           midgard::encode<std::vector<midgard::PointLL>>(shape, 1e6) % std::to_string(decoy_factor))
              .str();
    }
    const std::string tmpl = R"({"locations":[{"lon":%s,"lat":%s},{"lon":%s,"lat":%s}],)"
                             R"(%s"costing":"motorcycle",)"
                             R"("costing_options":{"motorcycle":{"prefer_curvature":%s,)"
                             R"("reuse_penalty":0.8,"curviness_continuity":%s,)"
                             R"("prefer_elevation":0.3,"top_speed":%s}},)"
                             R"("roundtrip":{"target_distance":24000,"num_candidates":1}})";
    const auto& a = map.nodes.at("A");
    const auto req = (boost::format(tmpl) % std::to_string(a.lng()) % std::to_string(a.lat()) %
                      std::to_string(a.lng()) % std::to_string(a.lat()) % decoy %
                      std::to_string(prefer_curvature) % std::to_string(prefer_curvature) %
                      std::to_string(static_cast<int>(120 - 40 * prefer_curvature)))
                         .str();
    return gurka::do_action(valhalla::Options::route, map, req);
  }
};
gurka::map RtAuditReturnHeuristic::map = {};

TEST_F(RtAuditReturnHeuristic, F03_BidirectionalReturnVsExactReturn) {
  baldr::GraphReader reader(map.config.get_child("mjolnir"));
  bool any_loss = false;

  for (double c : {0.5, 0.8}) {
    valhalla::Api stock, exact;
    try {
      stock = run(c, 0.0);
      exact = run(c, 0.0001); // min_linear_cost_factor_ -> 1e-4 => heuristic ~ 0
    } catch (const std::exception& e) {
      ADD_FAILURE() << "[F03] c" << c << " request threw: " << e.what();
      continue;
    }
    if (stock.trip().routes_size() < 1 || exact.trip().routes_size() < 1) {
      ADD_FAILURE() << "[F03] c" << c << " served no loop (stock " << stock.trip().routes_size()
                    << ", exact " << exact.trip().routes_size() << ")";
      continue;
    }

    // The lever must actually have reached the costing: add_cost_factor_edges()
    // (route_action.cc:387) runs BEFORE the round-trip branch, so a non-empty
    // cost_factor_edges list proves min_linear_cost_factor_ was driven below 1 and
    // AStarCostFactor() therefore returned ~0 for the return leg's bidirectional A*.
    const auto& s_co = stock.options().costings().at(stock.options().costing_type()).options();
    const auto& e_co = exact.options().costings().at(exact.options().costing_type()).options();
    std::cerr << "[F03] c" << c << " cost_factor_edges: stock=" << s_co.cost_factor_edges_size()
              << " exact=" << e_co.cost_factor_edges_size() << "\n";
    EXPECT_EQ(s_co.cost_factor_edges_size(), 0) << "the stock run must carry no cost factors";
    EXPECT_GT(e_co.cost_factor_edges_size(), 0)
        << "LEVER DEAD: linear_cost_factors never reached the costing, so the 'exact' run "
           "used the same heuristic as the stock run and this comparison is meaningless";

    const auto s_ret = leg_names(stock, 0, 1);
    const auto e_ret = leg_names(exact, 0, 1);
    const double s_curv = leg_curviness(reader, stock, 0, 1);
    const double e_curv = leg_curviness(reader, exact, 0, 1);
    const double s_cost =
        stock.trip().routes(0).legs(1).node().rbegin()->cost().elapsed_cost().cost();
    const double e_cost =
        exact.trip().routes(0).legs(1).node().rbegin()->cost().elapsed_cost().cost();

    std::cerr << "[F03] c" << c << " STOCK  return = " << dump_path(s_ret) << " | curviness "
              << s_curv << " | leg cost " << s_cost << "\n";
    std::cerr << "[F03] c" << c << " EXACT  return = " << dump_path(e_ret) << " | curviness "
              << e_curv << " | leg cost " << e_cost << "\n";
    std::cerr << "[F03] c" << c << " delta cost = " << (s_cost - e_cost)
              << " (positive = the bidirectional A* return is more expensive than the "
                 "exact one = optimality loss)\n";

    if (s_ret != e_ret)
      any_loss = true;
    // The exact search cannot be worse; if it is strictly better the heuristic pruned
    // the cheaper return away.
    EXPECT_LE(s_cost, e_cost + 1e-3)
        << "F03 CONFIRMED at c" << c << ": the stock bidirectional A* return costs " << s_cost
        << " where the near-Dijkstra return costs " << e_cost
        << " — the inadmissible heuristic pruned the cheaper (curvier) return. stock = "
        << dump_path(s_ret) << " exact = " << dump_path(e_ret);
  }
  if (any_loss)
    std::cerr << "[F03] the two searches returned DIFFERENT return legs — see above.\n";
}

// =================================================================================
// P1 (proto/v4-p1, curvagen-valhalla#10) — tiered road identity.
//
// G1/G1b/G2/G3 above are the red->green set: with the sidecar and harvest hygiene in
// place they must now PASS.  The three tests below are new and cover what those cannot:
//   P1a  the PARALLEL tier (30-80 m) steers the return off a 50 m parallel street,
//        with a config-off control that proves the map discriminates;
//   P1b  widening the exclusion must not BOX THE RETURN IN — where the twin is the
//        only way home the cell still serves (a Fallback Loop), never a 442;
//   P1c  the ranking puts hard-exclude successes ahead of Fallback Loops.
// =================================================================================

// ---------------------------------------------------------------------------------
// P1a — the parallel tier.  25 m/char.
//
//   A========================================B     corridor, curvature 15, 3 km
//   P----------------------------------------Q     50 m south: the PARALLEL street
//   F----------------------------------------E     200 m south: the fresh way home
//
// Links B-Q / P-A (50 m) and B-E / F-A (200 m).  Returning over P-Q is 300 m shorter
// than returning over E-F, so the ungraded engine takes the street one block over —
// the shape a rider reads as "it brought me back the same way".  With the parallel
// tier on, that street carries the soft leash and the progress-graded rejoin penalty
// and the return takes the genuinely fresh road.
// ---------------------------------------------------------------------------------
class RtP1ParallelTier : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
    A----------------------------------------------------------------------------------------------------------------------B

    P----------------------------------------------------------------------------------------------------------------------Q




    F----------------------------------------------------------------------------------------------------------------------E
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BQ", {{"highway", "secondary"}}},
        {"QP", {{"highway", "secondary"}}}, {"PA", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}}, {"EF", {{"highway", "secondary"}}},
        {"FA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 25);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p1_parallel_tier");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"A", "B"}, {"B", "A"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
  static valhalla::Api run(gurka::map& m, bool parallel_tier) {
    m.config.put("thor.roundtrip_parallel_tier", parallel_tier);
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle",
                            {{"/roundtrip/target_distance", "6000"},
                             {"/roundtrip/num_candidates", "1"},
                             {"/costing_options/motorcycle/reuse_penalty", "0.8"},
                             {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  }
};
gurka::map RtP1ParallelTier::map = {};

TEST_F(RtP1ParallelTier, P1a_ParallelStreetLoosesToTheFreshRoad) {
  // CONTROL: parallel tier off => the shorter parallel street wins.  If it does not,
  // the map is not discriminating and the assertion below proves nothing.
  auto ctl = run(map, false);
  ASSERT_GE(ctl.trip().routes_size(), 1) << "control served no loop";
  const auto ctl_ret = leg_names(ctl, 0, 1);
  std::cerr << "[P1a] CONTROL (parallel tier off) legs[1] = " << dump_path(ctl_ret) << "\n";
  EXPECT_GE(count_name(ctl_ret, "QP"), 1)
      << "MAP NOT DISCRIMINATING: even untiered the return avoided the 50 m parallel. "
         "control legs[1] = "
      << dump_path(ctl_ret);

  auto result = run(map, true);
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  const auto ret = leg_names(result, 0, 1);
  std::cerr << "[P1a] legs[0] = " << dump_path(leg_names(result, 0, 0)) << "\n";
  std::cerr << "[P1a] legs[1] = " << dump_path(ret) << "\n";
  EXPECT_EQ(count_name(ret, "QP"), 0)
      << "the parallel tier did not steer the return off the 50 m parallel street; "
         "legs[1] = "
      << dump_path(ret);
  EXPECT_GE(count_name(ret, "EF"), 1)
      << "expected the fresh southern road on the return; legs[1] = " << dump_path(ret);
}

// ---------------------------------------------------------------------------------
// P1b — widening the exclusion must not box the return in.  30 m/char.
//
//   A>>>>>>>>>>>>>>>>>>>>B     one-way east, 3 km, curvature 15
//   D<<<<<<<<<<<<<<<<<<<<C     one-way west, 3 km, 30 m south — the TWIN
//
// The G2 map with the fresh southern road REMOVED: the twin carriageway is the only
// way home.  P1 hard-excludes it, the primary return search fails, and the Fallback
// must serve the loop anyway.  A 442 here would mean the mechanism starves cells.
// ---------------------------------------------------------------------------------
class RtP1TwinOnlyWayHome : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
    A---------------------------------------------------------------------------------------------------B
    D---------------------------------------------------------------------------------------------------C
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"CD", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"BC", {{"highway", "primary"}}},
        {"DA", {{"highway", "primary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 30);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p1_twin_only_way_home");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> curvy;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"A", "B"}, {"C", "D"}})
      curvy.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config,
                          [&curvy](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (std::find(curvy.begin(), curvy.end(), edgeid) != curvy.end())
                              edge.set_curvature(15);
                          });
  }
};
gurka::map RtP1TwinOnlyWayHome::map = {};

TEST_F(RtP1TwinOnlyWayHome, P1b_TwinExclusionFallsBackInsteadOfFailing) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "6000"},
                                  {"/roundtrip/num_candidates", "1"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.8"},
                                  {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  ASSERT_GE(result.trip().routes_size(), 1)
      << "P1 REGRESSION: hard-excluding the twin left the cell with no loop at all — the "
         "Fallback must still serve one";
  ASSERT_EQ(result.trip().routes(0).legs_size(), 2);
  std::cerr << "[P1b] legs[0] = " << dump_path(leg_names(result, 0, 0)) << "\n";
  std::cerr << "[P1b] legs[1] = " << dump_path(leg_names(result, 0, 1)) << "\n";
}

// ---------------------------------------------------------------------------------
// P1c — clean-first ranking.  1000 m/char.
//
//        C          Two turnarounds, both in the +/-18 % band of target/2 = 8 km:
//                     D (pd 8.66 km) sits inside a BULB hanging off the single access
//   A  B   D          road A-B.  Its return can reach B on fresh road (D-E-B) but A-B
//                     is hard-excluded, so it has no way home => FALLBACK LOOP.  Its
//        E            harvest curviness is 0.65 (the bulb is curvature 15).
//                     Q (pd 8 km) closes over the fresh diagonal Q-A => a HARD-EXCLUDE
//   P   Q             SUCCESS, curviness 0.53 (curvature 8).
//
// v3 sorted on curviness alone and served the Fallback in slot 0.  P1 sorts
// hard-exclude successes first, so slot 0 must be the Q loop.
// ---------------------------------------------------------------------------------
class RtP1CleanFirstRanking : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
     C

A  B   D

     E

P   Q
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DE", {{"highway", "secondary"}}},
        {"EB", {{"highway", "secondary"}}}, {"AP", {{"highway", "secondary"}}},
        {"PQ", {{"highway", "secondary"}}}, {"QA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p1_clean_first");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> c15, c8;
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"B", "C"},
                                                                               {"C", "B"},
                                                                               {"C", "D"},
                                                                               {"D", "C"},
                                                                               {"D", "E"},
                                                                               {"E", "D"},
                                                                               {"E", "B"},
                                                                               {"B", "E"}})
      c15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "P"},
                                                                               {"P", "A"},
                                                                               {"P", "Q"},
                                                                               {"Q", "P"}})
      c8.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config, [&c15, &c8](const baldr::GraphId& edgeid,
                                                  baldr::DirectedEdge& edge) {
      if (std::find(c15.begin(), c15.end(), edgeid) != c15.end())
        edge.set_curvature(15);
      if (std::find(c8.begin(), c8.end(), edgeid) != c8.end())
        edge.set_curvature(8);
    });
  }
};
gurka::map RtP1CleanFirstRanking::map = {};

TEST_F(RtP1CleanFirstRanking, P1c_HardExcludeSuccessOutranksTheFallback) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "16000"},
                                  {"/roundtrip/num_candidates", "2"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.8"}});
  ASSERT_GE(result.trip().routes_size(), 1) << "no loop served";
  for (int r = 0; r < result.trip().routes_size(); ++r)
    std::cerr << "[P1c] slot " << r << " legs[0] = " << dump_path(leg_names(result, r, 0))
              << "| legs[1] = " << dump_path(leg_names(result, r, 1)) << "\n";
  ASSERT_EQ(result.trip().routes_size(), 2)
      << "the ranking assertion needs both loops built";
  // The hard-exclude success returns over the fresh diagonal QA; the Fallback Loop
  // retraces AB.
  const auto slot0 = leg_names(result, 0, 1);
  EXPECT_GE(count_name(slot0, "QA"), 1)
      << "slot 0 is not the hard-exclude success — the Fallback Loop still ranks first. "
         "legs[1] = "
      << dump_path(slot0);
  const auto slot1 = leg_names(result, 1, 1);
  EXPECT_GE(count_name(slot1, "AB"), 1)
      << "slot 1 is not the Fallback Loop; legs[1] = " << dump_path(slot1);
}

// =================================================================================
// P1.1 (proto/v4-p1.1, curvagen-valhalla#12) — switchback-safe twins, built-loop
// ranking, geometry Defect Gate.
//
//   P1.1a  the sidecar must not call a mountain hairpin pair a twin, and must still
//          call a dual carriageway one (the P1 report §7.6 regression, block C);
//   P1.1b  the served order comes from the BUILT loop, not the harvest chain (F20);
//   P1.1c  a return that rides the forward corridor's twins is refilled, not served.
// =================================================================================

namespace {
// The sidecar's view of one edge, for the switchback tests.
std::vector<uint64_t> twins_of(baldr::GraphReader& reader,
                               const gurka::nodelayout& layout,
                               bool switchback_test,
                               const std::string& a,
                               const std::string& b) {
  const auto& idx = valhalla::thor::RoadTwinIndex::get(reader, 30.0, 80.0, true, switchback_test);
  const auto e = std::get<0>(gurka::findEdgeByNodes(reader, layout, a, b));
  valhalla::baldr::graph_tile_ptr tile = reader.GetGraphTile(e);
  const auto* de = tile->directededge(e);
  const auto opp = reader.GetOpposingEdgeId(e);
  std::vector<uint64_t> out;
  idx.append_twins(valhalla::thor::RoadTwinIndex::canonical_id(de, e, opp), out);
  return out;
}
uint64_t canon_of(baldr::GraphReader& reader,
                  const gurka::nodelayout& layout,
                  const std::string& a,
                  const std::string& b) {
  const auto e = std::get<0>(gurka::findEdgeByNodes(reader, layout, a, b));
  valhalla::baldr::graph_tile_ptr tile = reader.GetGraphTile(e);
  const auto* de = tile->directededge(e);
  return valhalla::thor::RoadTwinIndex::canonical_id(de, e, reader.GetOpposingEdgeId(e));
}
uint64_t wayid_of(baldr::GraphReader& reader,
                  const gurka::nodelayout& layout,
                  const std::string& a,
                  const std::string& b) {
  const auto e = std::get<0>(gurka::findEdgeByNodes(reader, layout, a, b));
  valhalla::baldr::graph_tile_ptr tile = reader.GetGraphTile(e);
  return tile->edgeinfo(tile->directededge(e)).wayid();
}
bool has_canon(const std::vector<uint64_t>& tw, uint64_t k) {
  return std::find(tw.begin(), tw.end(), k) != tw.end();
}
} // namespace

// ---------------------------------------------------------------------------------
// P1.1a-1 — the mountain hairpin.  4 m/char, so a row is 4 m.
//
//   A--G--H--M--N--B   ONE OSM way "AGHMNBCIJKLD": arm A-B (400 m, east, four shape
//                  X   points), apex link B-C (8 m), arm C-D (west, drifting away).
//                  C   G/H/M/N and I/J/K/L are shape points, not junctions — without
//                I Y   them each arm decodes to two samples and the cover test cannot
//              J       see the divergence at all.
//            K
//          L
//
//   D
//
// C sits 8 m below B; D sits 32 m below A — so the arms are 8 m apart at the apex and
// 32 m apart at the far end, which is what a hairpin's arms do: they converge on the
// turn.  X and Y are 4 m spurs whose only job is to force graph nodes at B and C so
// the way yields three edges instead of one shape.  Planimetrically A-B and C-D pass
// every P1 test (collinear, ~92 % of samples inside 30 m, span 0.92); only the
// switchback test can separate them from a carriageway.
// ---------------------------------------------------------------------------------
class RtP11Hairpin : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
A                   G                   H                   M                   N                   B
                                                                                                    X
                                                                                                    C
                                                                                I                   Y
                                                            J
                                        K
                    L

D
    )";
    const gurka::ways ways = {
        {"AGHMNBCIJKLD", {{"highway", "secondary"}}},
        {"BX", {{"highway", "service"}}},
        {"CY", {{"highway", "service"}}},
    };
    map = gurka::buildtiles(gurka::detail::map_to_coordinates(ascii_map, 4), ways, {}, {},
                            "test/data/rt_p11_hairpin");
  }
};
gurka::map RtP11Hairpin::map = {};

TEST_F(RtP11Hairpin, P11a_HairpinArmsAreNotTwins) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto& L = map.nodes;
  std::cerr << "[P1.1a] way id A-B = " << wayid_of(*reader, L, "A", "B")
            << ", C-D = " << wayid_of(*reader, L, "C", "D") << "\n";

  // CONTROL — switchback test OFF: P1's geometry calls the two arms twins.  Without
  // this the assertion below could pass on a map that was never a twin at all.
  const auto ctl = twins_of(*reader, L, false, "A", "B");
  std::cerr << "[P1.1a] control (test off): A-B has " << ctl.size() << " twin(s); C-D in = "
            << has_canon(ctl, canon_of(*reader, L, "C", "D")) << "\n";
  EXPECT_TRUE(has_canon(ctl, canon_of(*reader, L, "C", "D")))
      << "MAP NOT DISCRIMINATING: the P1 geometry did not call the hairpin arms twins, so "
         "the switchback test has nothing to remove";

  // TREATMENT — same OSM way and a divergent offset: not a twin.
  const auto tw = twins_of(*reader, L, true, "A", "B");
  std::cerr << "[P1.1a] A-B has " << tw.size() << " twin(s); C-D in = "
            << has_canon(tw, canon_of(*reader, L, "C", "D")) << "\n";
  EXPECT_FALSE(has_canon(tw, canon_of(*reader, L, "C", "D")))
      << "the switchback test did not drop the hairpin pair — block C's is_lollipop and "
         "vlasina-50 km regressions stand";
}

// ---------------------------------------------------------------------------------
// P1.1a-2 — the dual carriageway control.  4 m/char.
//
//   P================================================Q   way "PQ", one-way east
//   S================================================R   way "SR", one-way west, 8 m
//
// Two DIFFERENT OSM ways holding a constant 8 m offset.  The switchback test must
// leave this pair alone, or P1's whole F02 result goes with it.
// ---------------------------------------------------------------------------------
class RtP11Carriageway : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
P                                                                                                   Q

S                                                                                                   R
    )";
    const gurka::ways ways = {
        {"PQ", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"SR", {{"highway", "primary"}, {"oneway", "yes"}}},
    };
    map = gurka::buildtiles(gurka::detail::map_to_coordinates(ascii_map, 4), ways, {}, {},
                            "test/data/rt_p11_carriageway");
  }
};
gurka::map RtP11Carriageway::map = {};

TEST_F(RtP11Carriageway, P11a_DualCarriagewayIsStillATwin) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto& L = map.nodes;
  std::cerr << "[P1.1a] way id P-Q = " << wayid_of(*reader, L, "P", "Q")
            << ", S-R = " << wayid_of(*reader, L, "S", "R") << "\n";
  const auto tw = twins_of(*reader, L, true, "P", "Q");
  std::cerr << "[P1.1a] P-Q has " << tw.size() << " twin(s); S-R in = "
            << has_canon(tw, canon_of(*reader, L, "S", "R")) << "\n";
  EXPECT_TRUE(has_canon(tw, canon_of(*reader, L, "S", "R")))
      << "the switchback test threw the dual carriageway out with the hairpin — F02 is back";
}

// ---------------------------------------------------------------------------------
// P1.1b — the BUILT loop is what gets ranked (F20's full fix).  1000 m/char.
//
//         Q   P            Two loops, BOTH clean hard-exclude successes:
//                            east lobe  A-B-C (pd 8 km; B-C curvature 15, A-B 0),
//         R   A B     C       home over C-D-A (16 km, curvature 0) => a 24 km ride,
//                             50 % over target.  Harvest curviness = 0.75.
//                     D     north-west lobe A-P-Q (pd 8 km, curvature 8), home over
//                             Q-R-A (8 km, curvature 8) => 16 km, on target.
//                             Harvest curviness = 0.53.
//
// v3/P1 rank on the harvest chain, so the east lobe takes slot 0.  P1.1 scores the
// built loop — whole-loop curviness 0.25 against 0.53, plus a 0.50 distance error —
// and the north-west lobe wins.
// ---------------------------------------------------------------------------------
class RtP11BuiltRanking : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(






      Q   P



      R   A B     C





                  D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}}, {"DA", {{"highway", "secondary"}}},
        {"AP", {{"highway", "secondary"}}}, {"PQ", {{"highway", "secondary"}}},
        {"QR", {{"highway", "secondary"}}}, {"RA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p11_built_ranking");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> c15, c8;
    for (const auto& [a, b] :
         std::vector<std::pair<std::string, std::string>>{{"B", "C"}, {"C", "B"}})
      c15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "P"},
                                                                              {"P", "A"},
                                                                              {"P", "Q"},
                                                                              {"Q", "P"},
                                                                              {"Q", "R"},
                                                                              {"R", "Q"},
                                                                              {"R", "A"},
                                                                              {"A", "R"}})
      c8.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config, [&c15, &c8](const baldr::GraphId& edgeid,
                                                  baldr::DirectedEdge& edge) {
      if (std::find(c15.begin(), c15.end(), edgeid) != c15.end())
        edge.set_curvature(15);
      if (std::find(c8.begin(), c8.end(), edgeid) != c8.end())
        edge.set_curvature(8);
    });
  }
  static valhalla::Api run(gurka::map& m, bool built_ranking) {
    m.config.put("thor.roundtrip_built_ranking", built_ranking);
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle",
                            {{"/roundtrip/target_distance", "16000"},
                             {"/roundtrip/num_candidates", "2"},
                             {"/costing_options/motorcycle/reuse_penalty", "0.8"}});
  }
};
gurka::map RtP11BuiltRanking::map = {};

TEST_F(RtP11BuiltRanking, P11b_BuiltLoopScoreReordersTheBank) {
  // CONTROL — P1's ranking (harvest chain): the curvy-forward lobe takes slot 0.
  auto ctl = run(map, false);
  ASSERT_EQ(ctl.trip().routes_size(), 2) << "control did not build both loops";
  for (int r = 0; r < 2; ++r)
    std::cerr << "[P1.1b] control slot " << r << " = " << dump_path(leg_names(ctl, r, 0)) << "| "
              << dump_path(leg_names(ctl, r, 1)) << "\n";
  EXPECT_GE(count_name(leg_names(ctl, 0, 0), "BC"), 1)
      << "MAP NOT DISCRIMINATING: the harvest-chain ranking did not put the curvy-forward "
         "lobe in slot 0; legs[0] = "
      << dump_path(leg_names(ctl, 0, 0));

  // TREATMENT — the built-loop score.
  auto result = run(map, true);
  ASSERT_EQ(result.trip().routes_size(), 2) << "did not build both loops";
  for (int r = 0; r < 2; ++r)
    std::cerr << "[P1.1b] slot " << r << " = " << dump_path(leg_names(result, r, 0)) << "| "
              << dump_path(leg_names(result, r, 1)) << "\n";
  const auto slot0 = leg_names(result, 0, 0);
  EXPECT_EQ(count_name(slot0, "BC"), 0)
      << "slot 0 is still the harvest-chain winner — the built-loop score did not reorder "
         "the bank; legs[0] = "
      << dump_path(slot0);
  // the lobe can be ridden either way round (A-P-Q home over Q-R-A, or the reverse).
  EXPECT_GE(count_name(slot0, "AP") + count_name(slot0, "PQ") + count_name(slot0, "RA") +
                count_name(slot0, "QR"),
            1)
      << "slot 0 is not the on-target north-west lobe; legs[0] = " << dump_path(slot0);
  EXPECT_GE(count_name(leg_names(result, 1, 0), "BC"), 1)
      << "the curvy-forward lobe did not fall to slot 1";
}

// ---------------------------------------------------------------------------------
// P1.1c — the geometry Defect Gate refills a twin-ridden return.  30 m/char.
//
//   A==E==B        top carriageway A>E>B, one-way east, 1.5 km per edge, curvature 15
//   D==F==C        bottom carriageway C>F>D, one-way west, 30 m south — the TWINS
//   ...
//   M==N           a clean second lobe 1.5 km south: A-M-N, home over N-A
//
// The corridor's second edge E-B sits beyond the Start Exemption, so it and its twin
// C-F are barred; the return falls to rung 1 and rides C-F home anyway — 1 500 m of
// twin outside the exemption, the exact shape P1 left in slots 9-11.  With K = 1 the
// gate must reject that loop and refill the slot with the clean lobe.
// ---------------------------------------------------------------------------------
class RtP11GeometryGate : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
                                                                                                    A                                                 E                                                 B
                                                                                                    D                                                 F                                                 C
















































                                                                                                    M                                                 N
    )";
    const gurka::ways ways = {
        {"AE", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"EB", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"CF", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"FD", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"BC", {{"highway", "primary"}}},
        {"DA", {{"highway", "primary"}}},
        {"AM", {{"highway", "secondary"}}},
        {"MN", {{"highway", "secondary"}}},
        {"NA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 30);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p11_geometry_gate");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> c15, c6;
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "E"},
                                                                              {"E", "B"},
                                                                              {"C", "F"},
                                                                              {"F", "D"}})
      c15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "M"},
                                                                              {"M", "A"},
                                                                              {"M", "N"},
                                                                              {"N", "M"},
                                                                              {"N", "A"},
                                                                              {"A", "N"}})
      c6.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config, [&c15, &c6](const baldr::GraphId& edgeid,
                                                  baldr::DirectedEdge& edge) {
      if (std::find(c15.begin(), c15.end(), edgeid) != c15.end())
        edge.set_curvature(15);
      if (std::find(c6.begin(), c6.end(), edgeid) != c6.end())
        edge.set_curvature(6);
    });
  }
  static valhalla::Api run(gurka::map& m, bool gate) {
    m.config.put("thor.roundtrip_geometry_gate", gate);
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle",
                            {{"/roundtrip/target_distance", "6000"},
                             {"/roundtrip/num_candidates", "1"},
                             {"/costing_options/motorcycle/reuse_penalty", "0.8"},
                             {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  }
};
gurka::map RtP11GeometryGate::map = {};

TEST_F(RtP11GeometryGate, P11c_TwinRiddenReturnIsRefilled) {
  // CONTROL — gate off: the twin-riding loop is served, which is what P1 does.
  auto ctl = run(map, false);
  ASSERT_GE(ctl.trip().routes_size(), 1) << "control served no loop";
  const auto ctl_ret = leg_names(ctl, 0, 1);
  std::cerr << "[P1.1c] control (gate off) legs[0] = " << dump_path(leg_names(ctl, 0, 0))
            << "| legs[1] = " << dump_path(ctl_ret) << "\n";
  EXPECT_GE(count_name(ctl_ret, "CF"), 1)
      << "MAP NOT DISCRIMINATING: even ungated the return avoided the twin carriageway; "
         "legs[1] = "
      << dump_path(ctl_ret);

  // TREATMENT — the gate rejects it and the queue refills with the clean lobe.
  auto result = run(map, true);
  ASSERT_GE(result.trip().routes_size(), 1)
      << "P1.1 REGRESSION: the gate rejected the twin loop and nothing was served";
  const auto ret = leg_names(result, 0, 1);
  const auto fwd = leg_names(result, 0, 0);
  std::cerr << "[P1.1c] legs[0] = " << dump_path(fwd) << "| legs[1] = " << dump_path(ret) << "\n";
  EXPECT_EQ(count_name(ret, "CF"), 0)
      << "the geometry gate served a return that rides the forward corridor's twin; "
         "legs[1] = "
      << dump_path(ret);
  EXPECT_GE(count_name(fwd, "AM") + count_name(fwd, "MN") + count_name(ret, "NA"), 1)
      << "the slot was not refilled with the clean M-N lobe; legs[0] = " << dump_path(fwd)
      << "| legs[1] = " << dump_path(ret);
}

// ---------------------------------------------------------------------------------
// P1.1d (§16) — THE REFILL BUDGET IS SCOPED TO THE GEOMETRY GATE.
//
// The sweep (§15.5) measured what a budget SHARED with ADR-0037's seam gate costs:
// once the budget is spent, a seam reject — an exact-mirror U-turn spike, the one
// absolute Gate v1.3 forbids — is kept in the bank instead of refilled, and
// `spike_ge_500m` went 0.00 -> 2.08 % at every bounded budget.  This map pins the fix.
//
//   A ==E==G==B--S            A=E=G=B is a one-way primary carriageway (curvature 15);
//   D ==F=====C               D=F=C is its twin, one-way the other way, 30 m south;
//        ...                  B--S is a 450 m dead-end spur off the far end;
//   M-----N                   A-M-N-A is a clean, straight, uncurvy lobe (curvature 6).
//
// Every corridor turnaround in the harvest band (G at 2 550 m, B at 3 000 m) can only
// get home along the twin carriageway, so each is a GEOMETRY-gate reject; S at 3 450 m
// can only get home by first retracing B--S, so it is a SEAM reject with a 450 m
// exact mirror at the seam.  A-M-N-A is the clean alternative.  With
// `roundtrip_gate_refill_budget = 1` the first geometry reject spends the budget and
// the second is kept, so the budget is provably EXHAUSTED before the spur candidate is
// ever built.  The pin: the spur loop must STILL be refilled away.
//
// On the pre-§16 binary this test fails — the exhausted shared budget keeps the spur
// loop, and a 450 m spike reaches the served bank while a clean loop exists.
// ---------------------------------------------------------------------------------
class RtP11ScopedRefillBudget : public ::testing::Test {
protected:
  static gurka::map map;
  static gurka::map spur_only;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
                                                                                                    A                                                 E                                  G              B              S
                                                                                                    D                                                 F                                                 C
















































                                                                                                    M                                                 N
    )";
    const gurka::ways ways = {
        {"AE", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"EG", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"GB", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"CF", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"FD", {{"highway", "primary"}, {"oneway", "yes"}}},
        {"BC", {{"highway", "primary"}}},
        {"DA", {{"highway", "primary"}}},
        // the dead-end spur: the only way off S is back down S--B, so any loop that
        // turns around at S carries a 450 m exact mirror across the seam.
        {"BS", {{"highway", "primary"}}},
        // the clean lobe
        {"AM", {{"highway", "secondary"}}},
        {"MN", {{"highway", "secondary"}}},
        {"NA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 30);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_p11_scoped_budget");

    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    std::vector<baldr::GraphId> c15, c6;
    // the corridor, its twin and the spur are the curvy roads, so the harvest queue
    // offers them BEFORE the clean lobe and the gate actually has to work.
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "E"},
                                                                              {"E", "G"},
                                                                              {"G", "B"},
                                                                              {"C", "F"},
                                                                              {"F", "D"},
                                                                              {"B", "S"},
                                                                              {"S", "B"}})
      c15.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"A", "M"},
                                                                              {"M", "A"},
                                                                              {"M", "N"},
                                                                              {"N", "M"},
                                                                              {"N", "A"},
                                                                              {"A", "N"}})
      c6.push_back(std::get<0>(gurka::findEdgeByNodes(*reader, layout, a, b)));
    test::customize_edges(map.config, [&c15, &c6](const baldr::GraphId& edgeid,
                                                  baldr::DirectedEdge& edge) {
      if (std::find(c15.begin(), c15.end(), edgeid) != c15.end())
        edge.set_curvature(15);
      if (std::find(c6.begin(), c6.end(), edgeid) != c6.end())
        edge.set_curvature(6);
    });

    // The discrimination proof, on its own tiles: the same 3 000 m + 450 m geometry
    // with NOTHING else in the graph.  Every loop here is an out-and-back mirror, the
    // seam gate rejects them all, and ADR-0037's last resort must still serve one —
    // which is what makes "S is a real seam reject with a >= 30 m stub" a measurement
    // rather than an assumption.
    const std::string spur_ascii = R"(
A                                                                                                   B              S
    )";
    const gurka::ways spur_ways = {
        {"AB", {{"highway", "primary"}}},
        {"BS", {{"highway", "primary"}}},
    };
    const auto spur_layout = gurka::detail::map_to_coordinates(spur_ascii, 30);
    spur_only =
        gurka::buildtiles(spur_layout, spur_ways, {}, {}, "test/data/rt_p11_scoped_spur");
  }

  static valhalla::Api run(gurka::map& m, uint32_t budget, uint32_t k, const char* target) {
    m.config.put("thor.roundtrip_gate_refill_budget", budget);
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle",
                            {{"/roundtrip/target_distance", target},
                             {"/roundtrip/num_candidates", std::to_string(k)},
                             {"/costing_options/motorcycle/reuse_penalty", "0.8"},
                             {"/costing_options/motorcycle/prefer_curvature", "0.5"}});
  }
};
gurka::map RtP11ScopedRefillBudget::map = {};
gurka::map RtP11ScopedRefillBudget::spur_only = {};

TEST_F(RtP11ScopedRefillBudget, P11d_SeamRefillIsNotSpentByTheGeometryBudget) {
  // (0) THE MAP IS DISCRIMINATING: the spur really does produce a >= 30 m seam stub,
  //     and the seam gate really does reject it (it only reaches the bank at all
  //     because it is the last resort on tiles that hold nothing else).
  auto probe = run(spur_only, 1, 1, "6900");
  ASSERT_GE(probe.trip().routes_size(), 1)
      << "REGRESSION: the seam gate starved the only loop on the spur-only tiles";
  const double probe_stub = self_mirror_stub_m(ride_shape(probe, 0));
  std::cerr << "[P1.1d] spur-only probe legs[0] = " << dump_path(leg_names(probe, 0, 0))
            << "| legs[1] = " << dump_path(leg_names(probe, 0, 1)) << ", mirror = " << probe_stub
            << " m\n";
  ASSERT_GE(probe_stub, 30.0) << "MAP NOT DISCRIMINATING: the spur geometry does not produce a "
                                 "seam-gate stub at all, so the treatment below proves nothing";

  // (1) THE TREATMENT.  budget = 1: the first geometry reject spends it, the second is
  //     kept, and the spur's seam reject arrives at an exhausted budget.
  auto result = run(map, 1, 3, "6000");
  ASSERT_GE(result.trip().routes_size(), 1)
      << "P1.1 REGRESSION: nothing was served with the budget scoped";
  const int n = result.trip().routes_size();
  bool twin_ride_served = false, clean_lobe_served = false;
  double worst_stub = 0;
  int spur_hits = 0;
  for (int r = 0; r < n; ++r) {
    const auto fwd = leg_names(result, r, 0);
    const auto ret = leg_names(result, r, 1);
    const double stub = self_mirror_stub_m(ride_shape(result, r));
    worst_stub = std::max(worst_stub, stub);
    spur_hits += count_name(fwd, "BS") + count_name(ret, "BS");
    if (count_name(ret, "CF") + count_name(ret, "FD") >= 1)
      twin_ride_served = true;
    if (count_name(ret, "NA") + count_name(fwd, "AM") >= 1)
      clean_lobe_served = true;
    std::cerr << "[P1.1d] slot " << r << " legs[0] = " << dump_path(fwd) << "| legs[1] = "
              << dump_path(ret) << ", mirror = " << stub << " m\n";
  }

  // the budget is EXHAUSTED — a geometry-gated loop was kept and sorted into the last
  // tier rather than bought out with a replacement build.
  EXPECT_TRUE(twin_ride_served)
      << "MAP NOT DISCRIMINATING: no served return rides the twin carriageway, so the "
         "geometry budget was never exhausted and the pin below is vacuous";
  // ...and a clean loop existed the whole time.
  EXPECT_TRUE(clean_lobe_served) << "the clean A-M-N-A lobe never reached the bank";

  // THE PIN.  ADR-0037's seam refill is not the geometry gate's to spend.
  EXPECT_EQ(spur_hits, 0)
      << "SHARED-BUDGET REGRESSION (§15.5): an exhausted GEOMETRY budget kept a SEAM "
         "reject, and the dead-end spur B--S reached the served bank";
  EXPECT_LT(worst_stub, 30.0)
      << "SHARED-BUDGET REGRESSION (§15.5): the served bank carries a " << worst_stub
      << " m exact-mirror spike while a clean loop exists — the seam gate must refill "
         "unconditionally at every budget";
}

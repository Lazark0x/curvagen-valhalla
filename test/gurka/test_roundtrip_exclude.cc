// roundtrip.exclude_turnarounds (curvagen-valhalla#28): a Shuffle passes the turnarounds
// it already served, and the engine must not serve them again while the cell has
// another loop, must still fill when it has none, and must not change anything for a
// request without the field.

#include "gurka.h"
#include "midgard/encoded.h"
#include "midgard/pointll.h"
#include "test.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace valhalla;

namespace {

// Leg 0 runs start -> turnaround, so its last shape point is the served turnaround.
midgard::PointLL turnaround(const valhalla::Api& api, int route_idx) {
  return midgard::decode<std::vector<midgard::PointLL>>(api.trip().routes(route_idx).legs(0).shape())
      .back();
}

std::string at(const std::string& index, const std::string& field) {
  return "/roundtrip/exclude_turnarounds/" + index + "/" + field;
}

} // namespace

// Two 14 km rectangles share the A-C rung:
//   B----C----D
//   |    |    |
//   E----A----F
// From A at 14 km the left loop turns at B and the right one at D, 10 km apart.
class RoundTripExclude : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      B----C----D
      |    |    |
      E----A----F
    )";
    const gurka::ways ways = {
        {"BC", {{"highway", "secondary"}}}, {"CD", {{"highway", "secondary"}}},
        {"EB", {{"highway", "secondary"}}}, {"AC", {{"highway", "secondary"}}},
        {"FD", {{"highway", "secondary"}}}, {"EA", {{"highway", "secondary"}}},
        {"AF", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/roundtrip_exclude");
  }

  static valhalla::Api shuffle(const std::vector<midgard::PointLL>& exclude,
                               std::unordered_map<std::string, std::string> extra = {}) {
    std::unordered_map<std::string, std::string> options = {
        {"/roundtrip/target_distance", "14000"},
        {"/roundtrip/num_candidates", "1"},
        {"/roundtrip/seed", "0"},
    };
    for (size_t i = 0; i < exclude.size(); ++i) {
      options[at(std::to_string(i), "lat")] = std::to_string(exclude[i].lat());
      options[at(std::to_string(i), "lon")] = std::to_string(exclude[i].lng());
    }
    for (const auto& kv : extra)
      options[kv.first] = kv.second;
    return gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle", options);
  }
};
gurka::map RoundTripExclude::map = {};

TEST_F(RoundTripExclude, ExcludedTurnaroundIsNotServedAgain) {
  const auto first = shuffle({});
  ASSERT_EQ(first.trip().routes_size(), 1);
  const auto t1 = turnaround(first, 0);

  const auto second = shuffle({t1});
  ASSERT_EQ(second.trip().routes_size(), 1) << "the other rectangle is still available";
  EXPECT_GT(turnaround(second, 0).Distance(t1), 700.0) << "the excluded turnaround came back";
}

TEST_F(RoundTripExclude, ExhaustedCellStillFills) {
  // On this map a rectangle can turn at either of its far corners, 2 km apart, so the
  // radius covers both: one excluded turnaround then excludes the whole rectangle.
  const std::unordered_map<std::string, std::string> wide = {{"/roundtrip/exclude_radius_m", "2500"}};
  const auto first = shuffle({});
  const auto second = shuffle({turnaround(first, 0)}, wide);
  ASSERT_EQ(second.trip().routes_size(), 1);
  EXPECT_GT(turnaround(second, 0).Distance(turnaround(first, 0)), 2500.0);

  // Both rectangles excluded: no fresh loop is left, so the cell repeats one; the recency
  // fallback keeps excluding the most recent (the list comes oldest first), so it is the
  // first rectangle that comes back.
  const auto third = shuffle({turnaround(first, 0), turnaround(second, 0)}, wide);
  ASSERT_EQ(third.trip().routes_size(), first.trip().routes_size());
  EXPECT_LT(turnaround(third, 0).Distance(turnaround(first, 0)), 2500.0);
}

TEST_F(RoundTripExclude, SpentCellRepeatsTheOldestLoopFirst) {
  const std::unordered_map<std::string, std::string> wide = {{"/roundtrip/exclude_radius_m", "2500"}};
  const auto t1 = turnaround(shuffle({}), 0);
  const auto t2 = turnaround(shuffle({t1}, wide), 0);
  ASSERT_GT(t2.Distance(t1), 2500.0);

  // Both rectangles excluded, t1 the more recent: t2's rectangle comes back, although the
  // plain run (the top-up without the fallback) serves t1's.
  const auto recency = shuffle({t2, t1}, wide);
  ASSERT_EQ(recency.trip().routes_size(), 1);
  EXPECT_LT(turnaround(recency, 0).Distance(t2), 2500.0);

  gurka::map plain_top_up = map;
  plain_top_up.config.put("thor.roundtrip_exclude_recency_fallback", false);
  std::unordered_map<std::string, std::string> options = {
      {"/roundtrip/target_distance", "14000"},
      {"/roundtrip/num_candidates", "1"},
      {"/roundtrip/seed", "0"},
      {"/roundtrip/exclude_radius_m", "2500"},
  };
  for (const auto& [i, t] : {std::make_pair(0, t2), std::make_pair(1, t1)}) {
    options[at(std::to_string(i), "lat")] = std::to_string(t.lat());
    options[at(std::to_string(i), "lon")] = std::to_string(t.lng());
  }
  const auto top_up =
      gurka::do_action(valhalla::Options::route, plain_top_up, {"A", "A"}, "motorcycle", options);
  ASSERT_EQ(top_up.trip().routes_size(), 1);
  EXPECT_LT(turnaround(top_up, 0).Distance(t1), 2500.0);
}

TEST_F(RoundTripExclude, ShortFreshRunIsToppedUpRankedLast) {
  const auto first = shuffle({});
  const auto t1 = turnaround(first, 0);

  // K=2 excluding the first rectangle (both its far corners, as above): one fresh loop,
  // topped up with the excluded rectangle from the plain run.
  const auto both =
      shuffle({t1}, {{"/roundtrip/num_candidates", "2"}, {"/roundtrip/exclude_radius_m", "2500"}});
  ASSERT_EQ(both.trip().routes_size(), 2);
  EXPECT_GT(turnaround(both, 0).Distance(t1), 2500.0) << "a fresh loop ranks first";
  EXPECT_LT(turnaround(both, 1).Distance(t1), 2500.0) << "the repeat ranks last";
  EXPECT_NE(gurka::detail::get_paths(both)[0], gurka::detail::get_paths(both)[1]);
}

TEST_F(RoundTripExclude, RadiusBoundsTheExclusion) {
  const auto first = shuffle({});
  const auto t1 = turnaround(first, 0);
  // A point 2 km from the served turnaround excludes it only when the radius reaches.
  const midgard::PointLL near(t1.lng(), t1.lat() + 2000.0 / 111111.0);

  const auto narrow = shuffle({near}, {{"/roundtrip/exclude_radius_m", "1000"}});
  EXPECT_EQ(gurka::detail::get_paths(narrow), gurka::detail::get_paths(first));

  const auto wide = shuffle({near}, {{"/roundtrip/exclude_radius_m", "3000"}});
  ASSERT_EQ(wide.trip().routes_size(), 1);
  EXPECT_GT(turnaround(wide, 0).Distance(t1), 700.0);
}

TEST_F(RoundTripExclude, NoExclusionsLeavesTheAnswerUnchanged) {
  const auto plain = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                      {{"/roundtrip/target_distance", "14000"},
                                       {"/roundtrip/num_candidates", "2"},
                                       {"/roundtrip/seed", "3"}});
  const auto radius_only = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                            {{"/roundtrip/target_distance", "14000"},
                                             {"/roundtrip/num_candidates", "2"},
                                             {"/roundtrip/seed", "3"},
                                             {"/roundtrip/exclude_radius_m", "5000"}});
  ASSERT_GE(plain.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(radius_only), gurka::detail::get_paths(plain));
  for (int i = 0; i < plain.trip().routes_size(); ++i)
    EXPECT_EQ(radius_only.trip().routes(i).legs(0).shape(), plain.trip().routes(i).legs(0).shape());
}

TEST_F(RoundTripExclude, DeepPickServesTheBestSurvivorWhateverTheSeed) {
  // With exclusions present each sector serves its best survivor, so the seed no longer
  // chooses: two seeds excluding the same far-off point get the same loops.
  const midgard::PointLL far(0.0, 0.0);
  const auto a = shuffle({far}, {{"/roundtrip/num_candidates", "2"}, {"/roundtrip/seed", "1"}});
  const auto b = shuffle({far}, {{"/roundtrip/num_candidates", "2"}, {"/roundtrip/seed", "2"}});
  ASSERT_GE(a.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(a), gurka::detail::get_paths(b));
}

TEST_F(RoundTripExclude, OffTargetFreshLoopGivesWayToTheRepeat) {
  // Under exclusions a fresh loop past thor.roundtrip_disterr_tier is never served: the
  // plain run answers instead, a repeat the orchestrator reads as a spent cell. A tier of
  // ~0 and an ask 1 m off the rectangles make every loop on this map off target.
  gurka::map strict = map;
  strict.config.put("thor.roundtrip_disterr_tier", 1e-9);
  auto run = [](const gurka::map& m, const std::vector<midgard::PointLL>& exclude) {
    std::unordered_map<std::string, std::string> options = {
        {"/roundtrip/target_distance", "14001"},
        {"/roundtrip/num_candidates", "1"},
        {"/roundtrip/seed", "0"},
    };
    for (size_t i = 0; i < exclude.size(); ++i) {
      options[at(std::to_string(i), "lat")] = std::to_string(exclude[i].lat());
      options[at(std::to_string(i), "lon")] = std::to_string(exclude[i].lng());
    }
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle", options);
  };
  const auto first = run(map, {});
  ASSERT_EQ(first.trip().routes_size(), 1);
  const auto t1 = turnaround(first, 0);

  // No exclusions: the guard does not apply, the answer is the plain one.
  EXPECT_EQ(gurka::detail::get_paths(run(strict, {})), gurka::detail::get_paths(first));

  // Excluded: the other rectangle is off target, so the plain run is served.
  const auto guarded = run(strict, {t1});
  ASSERT_EQ(guarded.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(guarded), gurka::detail::get_paths(first));

  // Guard off: the fresh off-target loop is served as before.
  gurka::map unguarded = strict;
  unguarded.config.put("thor.roundtrip_exclude_disterr_guard", false);
  const auto fresh = run(unguarded, {t1});
  ASSERT_EQ(fresh.trip().routes_size(), 1);
  EXPECT_GT(turnaround(fresh, 0).Distance(t1), 700.0);
}

// One 26 km ring with the start A on it. P is A's antipode; Q, 2 km on, is a turnaround of
// the very same ring (out 11 km by E and C, home 15 km by P and B). The stubs make P and Q
// junctions, as the roads a near-twin turns on are:
//        X Y
//        | |
//   B----P-Q---C
//   |          |
//   D-----A----E
class RoundTripExcludeRing : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
           X Y
           | |
      B----P-Q---C
      |          |
      D-----A----E
    )";
    const gurka::ways ways = {
        {"BP", {{"highway", "secondary"}}},   {"PQ", {{"highway", "secondary"}}},
        {"QC", {{"highway", "secondary"}}},   {"CE", {{"highway", "secondary"}}},
        {"EA", {{"highway", "secondary"}}},   {"AD", {{"highway", "secondary"}}},
        {"DB", {{"highway", "secondary"}}},   {"PX", {{"highway", "residential"}}},
        {"QY", {{"highway", "residential"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/roundtrip_exclude_ring");
  }

  static valhalla::Api shuffle(const gurka::map& m, const std::vector<midgard::PointLL>& exclude) {
    std::unordered_map<std::string, std::string> options = {
        {"/roundtrip/target_distance", "26000"},
        {"/roundtrip/num_candidates", "1"},
        {"/roundtrip/seed", "0"},
        {"/roundtrip/exclude_radius_m", "1000"},
    };
    for (size_t i = 0; i < exclude.size(); ++i) {
      options[at(std::to_string(i), "lat")] = std::to_string(exclude[i].lat());
      options[at(std::to_string(i), "lon")] = std::to_string(exclude[i].lng());
    }
    return gurka::do_action(valhalla::Options::route, m, {"A", "A"}, "motorcycle", options);
  }
};
gurka::map RoundTripExcludeRing::map = {};

TEST_F(RoundTripExcludeRing, NearTwinRidingThroughAnExcludedTurnaroundIsRefused) {
  const auto first = shuffle(map, {});
  ASSERT_EQ(first.trip().routes_size(), 1);
  const auto t1 = turnaround(first, 0);

  // Without the test, excluding t1 serves the same ring turning 2 km on: a near-twin.
  gurka::map no_pass = map;
  no_pass.config.put("thor.roundtrip_exclude_pass_m", 0.0);
  const auto twin = shuffle(no_pass, {t1});
  ASSERT_EQ(twin.trip().routes_size(), 1);
  EXPECT_GT(turnaround(twin, 0).Distance(t1), 1000.0);
  double through = std::numeric_limits<double>::max();
  for (const auto& leg : twin.trip().routes(0).legs())
    for (const auto& p : midgard::decode<std::vector<midgard::PointLL>>(leg.shape()))
      through = std::min(through, static_cast<double>(p.Distance(t1)));
  EXPECT_LT(through, 50.0) << "the twin rides through the excluded turnaround";

  // With it, that loop rides through t1 and is refused: the ring is spent, and the plain
  // run serves it as the repeat it is.
  const auto guarded = shuffle(map, {t1});
  ASSERT_EQ(guarded.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(guarded), gurka::detail::get_paths(first));
}

TEST_F(RoundTripExcludeRing, SpentBuildBudgetServesTheNearTwinInHand) {
  const auto first = shuffle(map, {});
  ASSERT_EQ(first.trip().routes_size(), 1);
  const auto t1 = turnaround(first, 0);
  gurka::map no_pass = map;
  no_pass.config.put("thor.roundtrip_exclude_pass_m", 0.0);
  const auto twin = shuffle(no_pass, {t1});
  ASSERT_EQ(twin.trip().routes_size(), 1);

  // A budget of one refusal: the near-twin refused there is what the request has in hand,
  // so it is served instead of a further search and the plain run.
  gurka::map capped = map;
  capped.config.put("thor.roundtrip_exclude_max_refused", 1);
  const auto spent = shuffle(capped, {t1});
  ASSERT_EQ(spent.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(spent), gurka::detail::get_paths(twin));

  // A budget the request never reaches changes nothing.
  gurka::map roomy = map;
  roomy.config.put("thor.roundtrip_exclude_max_refused", 1000);
  roomy.config.put("thor.roundtrip_exclude_budget_ms", 600000);
  EXPECT_EQ(gurka::detail::get_paths(shuffle(roomy, {t1})), gurka::detail::get_paths(first));
}

TEST_F(RoundTripExclude, SpentBuildBudgetStillRefusesOffTargetLoops) {
  // Every loop on this map is off target under a tier of ~0: a spent budget serves none of
  // them fresh, so the plain run answers, as without a budget.
  gurka::map strict = map;
  strict.config.put("thor.roundtrip_disterr_tier", 1e-9);
  strict.config.put("thor.roundtrip_exclude_max_refused", 1);
  std::unordered_map<std::string, std::string> options = {
      {"/roundtrip/target_distance", "14001"},
      {"/roundtrip/num_candidates", "1"},
      {"/roundtrip/seed", "0"},
  };
  const auto first =
      gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle", options);
  ASSERT_EQ(first.trip().routes_size(), 1);
  const auto t1 = turnaround(first, 0);
  options[at("0", "lat")] = std::to_string(t1.lat());
  options[at("0", "lon")] = std::to_string(t1.lng());
  const auto guarded =
      gurka::do_action(valhalla::Options::route, strict, {"A", "A"}, "motorcycle", options);
  ASSERT_EQ(guarded.trip().routes_size(), 1);
  EXPECT_EQ(gurka::detail::get_paths(guarded), gurka::detail::get_paths(first));
}

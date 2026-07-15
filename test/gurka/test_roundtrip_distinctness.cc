#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace valhalla;

// Coverage for the wayfinder #46 cross-candidate distinctness work: the soft
// cross-candidate corridor penalty (thor.roundtrip_xcand_penalty) and the item 4
// near-duplicate sharing filter (thor.roundtrip_sharing_filter). Both are config-gated
// and default off, so the existing round-trip suite already pins the flags-off behavior.

namespace {

// Mean pairwise undirected-edge overlap across a served bank — the gurka-scale analog of
// the loopqual bank_overlap metric. Edge names are way names (direction-agnostic), so a
// road ridden either way collides. Overlap of a pair = shared way count / smaller loop.
double mean_pair_overlap(const std::vector<std::vector<std::string>>& paths) {
  double sum = 0.0;
  int pairs = 0;
  for (size_t i = 0; i < paths.size(); ++i)
    for (size_t j = i + 1; j < paths.size(); ++j) {
      const std::set<std::string> a(paths[i].begin(), paths[i].end());
      const std::set<std::string> b(paths[j].begin(), paths[j].end());
      std::vector<std::string> shared;
      std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(shared));
      const size_t denom = std::min(a.size(), b.size());
      if (denom) {
        sum += static_cast<double>(shared.size()) / static_cast<double>(denom);
        ++pairs;
      }
    }
  return pairs ? sum / pairs : 0.0;
}

} // namespace

// A 3x3 grid (adjacency, 1500 m cells so the interior edges sit beyond the 1.5 km Start
// Exemption) with no forced start stem: from corner A the loops to different in-band
// turnarounds reach across a shared interior and overlap on the middle edges. The grid's
// alternative paths let the cross-candidate penalty route a later candidate around an
// earlier loop, dropping bank overlap. The penalty strength is raised well above the ship
// value here: on a minimal map the prod-scale 0.5 surcharge rarely flips a discrete
// short-loop routing decision, whereas at scale it moves bank overlap materially (the
// loopqual A/B measures -19% over 232 real banks). The test pins the MECHANISM — that the
// penalty diversifies and never shrinks the bank — not the ship magnitude.
class RoundTripXCandDiversity : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      GHI
      DEF
      ABC
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"DE", {{"highway", "secondary"}}}, {"EF", {{"highway", "secondary"}}},
        {"GH", {{"highway", "secondary"}}}, {"HI", {{"highway", "secondary"}}},
        {"AD", {{"highway", "secondary"}}}, {"DG", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}}, {"EH", {{"highway", "secondary"}}},
        {"CF", {{"highway", "secondary"}}}, {"FI", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1500);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/roundtrip_xcand_diversity");
  }

  static std::vector<std::vector<std::string>> run(bool penalty) {
    map.config.put("thor.roundtrip_xcand_penalty", penalty);
    map.config.put("thor.roundtrip_xcand_strength", 2.0);
    auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                   {{"/roundtrip/target_distance", "6750"},
                                    {"/roundtrip/num_candidates", "4"},
                                    {"/costing_options/motorcycle/reuse_penalty", "0.8"}});
    return gurka::detail::get_paths(result);
  }
};
gurka::map RoundTripXCandDiversity::map = {};

TEST_F(RoundTripXCandDiversity, PenaltyLowersBankOverlap) {
  const auto off = run(false);
  const auto on = run(true);
  ASSERT_GE(off.size(), 2u) << "need a 2-loop bank to measure overlap";
  ASSERT_GE(on.size(), 2u) << "the penalty must not shrink the served bank";
  const double ov_off = mean_pair_overlap(off);
  const double ov_on = mean_pair_overlap(on);
  EXPECT_LT(ov_on, ov_off) << "cross-candidate penalty did not diversify the bank (off=" << ov_off
                           << " on=" << ov_on << ")";
}

// The forced-stem cul-de-sac (mirrors MotorcycleRoundTripCulDeSac): A's only access is
// the short AB edge, inside the Start Exemption. The cross-candidate penalty must NEVER
// surcharge exemption-zone edges, or the second candidate loses its only way home and the
// bank under-fills / 442s. Pins that the penalty leaves the forced start stem alone.
class RoundTripXCandExemption : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      AB------C
       |      |
       E------D
    )";
    const gurka::ways ways = {
        {"AB", {{"highway", "secondary"}}}, {"BC", {{"highway", "primary"}}},
        {"CD", {{"highway", "primary"}}},   {"ED", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/roundtrip_xcand_exemption");
    map.config.put("thor.roundtrip_xcand_penalty", true);
  }
};
gurka::map RoundTripXCandExemption::map = {};

TEST_F(RoundTripXCandExemption, PenaltyLeavesTheForcedStemRoutable) {
  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                                 {{"/roundtrip/target_distance", "18000"},
                                  {"/roundtrip/num_candidates", "2"},
                                  {"/costing_options/motorcycle/reuse_penalty", "0.8"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u) << "the penalty starved the cul-de-sac of its way home";
  // Every served loop must still carry the exempt access road out and home (twice).
  for (const auto& path : paths) {
    std::map<std::string, int> count;
    for (const auto& edge : path)
      ++count[edge];
    EXPECT_EQ(count["AB"], 2) << "the exempt access road was surcharged out of the loop";
  }
}

// A tight ladder of near-identical loops (mirrors MotorcycleRoundTripCluster): the cells
// are packed so K candidates converge onto near-duplicate geometry. With the sharing
// filter on, no two SERVED loops may overlap beyond the threshold — the filter refills
// past near-dups, and where the network can't offer K distinct loops the bank honestly
// under-fills rather than serving twins.
class RoundTripSharingFilter : public ::testing::Test {
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
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/roundtrip_sharing_filter");
  }
};
gurka::map RoundTripSharingFilter::map = {};

TEST_F(RoundTripSharingFilter, ServedLoopsStayUnderTheSharingThreshold) {
  map.config.put("thor.roundtrip_sharing_filter", true);
  map.config.put("thor.roundtrip_sharing_frac", 0.6);
  auto result =
      gurka::do_action(valhalla::Options::route, map, {"A", "A"}, "motorcycle",
                       {{"/roundtrip/target_distance", "14000"}, {"/roundtrip/num_candidates", "8"}});
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_GE(paths.size(), 1u);
  // No pair of served loops shares more than the threshold of the smaller loop's ways.
  for (size_t i = 0; i < paths.size(); ++i)
    for (size_t j = i + 1; j < paths.size(); ++j) {
      const std::set<std::string> a(paths[i].begin(), paths[i].end());
      const std::set<std::string> b(paths[j].begin(), paths[j].end());
      std::vector<std::string> shared;
      std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(shared));
      const double frac =
          static_cast<double>(shared.size()) / static_cast<double>(std::min(a.size(), b.size()));
      EXPECT_LE(frac, 0.6) << "served loops " << i << " and " << j << " are near-duplicates (overlap "
                           << frac << ")";
    }
}

// Faults a single round-trip request can reach: each one must cost that request at most
// a candidate, never the process, the rest of the bank, or an abandoned search that
// nobody can stop.

#include "baldr/graphreader.h"
#include "exceptions.h"
#include "gurka.h"
#include "midgard/constants.h"
#include "test.h"
#include "tyr/actor.h"
#include "worker.h"

#include <boost/format.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace valhalla;

namespace {

std::string rt_request(const gurka::map& map,
                       const std::string& node,
                       const std::string& target,
                       const uint32_t candidates,
                       const std::string& location_extra = "") {
  const auto& ll = map.nodes.at(node);
  return (boost::format(R"({"locations":[{"lat":%1$.7f,"lon":%2$.7f%3%},)"
                        R"({"lat":%1$.7f,"lon":%2$.7f}],"costing":"motorcycle",)"
                        R"("roundtrip":{"target_distance":%4%,"num_candidates":%5%,"seed":0}})") %
          ll.lat() % ll.lng() % location_extra % target % candidates)
      .str();
}

// The HTTP-facing failure of a request, 0 when it served.
unsigned failure_code(const std::function<void()>& request) {
  try {
    request();
  } catch (const valhalla_exception_t& e) { return e.code; }
  return 0;
}

} // namespace

// A zero-length edge ties P and J on cost, so the pair pass finds no junction strictly
// below J to hang it from and J has no tree parent, while the harvest still lists J as a
// turnaround in band.  B is nearer through J than around the ring, so B cannot give J a
// parent either.
class RoundTripFaultNoTreeParent : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A---------PJB

      D-----------C
    )";
    const gurka::ways ways = {
        {"AP", {{"highway", "secondary"}, {"name", "ring"}}},
        {"PJ", {{"highway", "secondary"}, {"name", "ring"}}},
        {"JB", {{"highway", "secondary"}, {"name", "ring"}}},
        {"BC", {{"highway", "secondary"}, {"name", "ring"}}},
        {"CD", {{"highway", "secondary"}, {"name", "ring"}}},
        {"DA", {{"highway", "secondary"}, {"name", "ring"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_no_tree_parent");
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    const auto pj = std::get<0>(gurka::findEdgeByNodes(*reader, layout, "P", "J"));
    const auto jp = std::get<0>(gurka::findEdgeByNodes(*reader, layout, "J", "P"));
    test::customize_edges(map.config,
                          [pj, jp](const baldr::GraphId& edgeid, baldr::DirectedEdge& edge) {
                            if (edgeid == pj || edgeid == jp)
                              edge.set_length(0);
                          });
  }
};
gurka::map RoundTripFaultNoTreeParent::map = {};

TEST_F(RoundTripFaultNoTreeParent, JunctionWithoutATreeParentIsDroppedNotDereferenced) {
  const auto result = gurka::do_action(Options::route, map, rt_request(map, "A", "24000", 1));
  EXPECT_EQ(result.trip().routes_size(), 1) << "the ring must still be served without J";
}

// Heading east, the start keeps only its eastbound edge, so a loop riding home on the
// westbound edge arrives on an edge the start never correlated.  The stem to B lies in
// the Start Exemption, so the pair pass banks two loops out and back through B; only the
// rescue build comes home round the north road and along the start's own eastbound edge.
class RoundTripFaultUncorrelatedReturn : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      G---M--H
      |      |
      A---xB-C
           | |
           | |
           | |
           E-D
    )";
    const gurka::ways ways = {
        {"AxB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}},  {"ED", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}},  {"CH", {{"highway", "secondary"}}},
        {"HM", {{"highway", "secondary"}}},  {"MG", {{"highway", "secondary"}}},
        {"GA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_uncorrelated_return");
  }
};
gurka::map RoundTripFaultUncorrelatedReturn::map = {};

TEST_F(RoundTripFaultUncorrelatedReturn, TheLoopThatCannotBuildLeavesTheBankServed) {
  const auto result =
      gurka::do_action(Options::route, map, rt_request(map, "x", "16000", 3, R"(,"heading":90)"));
  ASSERT_EQ(result.trip().routes_size(), 1)
      << "of the three loops banked, the two home through B cannot build";
  const auto paths = gurka::detail::get_paths(result);
  EXPECT_NE(std::find(paths[0].begin(), paths[0].end(), "HM"), paths[0].end())
      << "the served loop is the one that comes home on the start's own edge";
}

// The same stem with no north road: the only loop cannot build.
class RoundTripFaultOnlyLoopUncorrelated : public ::testing::Test {
protected:
  static gurka::map map;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A---xB-C
           | |
           | |
           | |
           E-D
    )";
    const gurka::ways ways = {
        {"AxB", {{"highway", "secondary"}}}, {"BC", {{"highway", "secondary"}}},
        {"BE", {{"highway", "secondary"}}},  {"ED", {{"highway", "secondary"}}},
        {"CD", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_only_loop_uncorrelated");
  }
};
gurka::map RoundTripFaultOnlyLoopUncorrelated::map = {};

TEST_F(RoundTripFaultOnlyLoopUncorrelated, AnEmptiedBankReportsNoLoop) {
  EXPECT_EQ(failure_code([&] {
              gurka::do_action(Options::route, map,
                               rt_request(map, "x", "14000", 1, R"(,"heading":90)"));
            }),
            442u);
}

// A 280 km perimeter: the only map here a clamped target can actually serve.
class RoundTripFaultTargetBounds : public ::testing::Test {
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
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 20000);
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_target_bounds");
  }
};
gurka::map RoundTripFaultTargetBounds::map = {};

TEST_F(RoundTripFaultTargetBounds, AHugeTargetServesTheLongestLoopTheContractAllows) {
  const auto result = gurka::do_action(Options::route, map, rt_request(map, "A", "1e12", 1));
  const auto paths = gurka::detail::get_paths(result);
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths[0].size(), 4u) << "the whole 280 km rectangle, once";
}

TEST_F(RoundTripFaultTargetBounds, ANegativeTargetFindsNoLoop) {
  EXPECT_EQ(failure_code(
                [&] { gurka::do_action(Options::route, map, rt_request(map, "A", "-5000", 1)); }),
            442u);
}

TEST_F(RoundTripFaultTargetBounds, EveryTargetTheContractAllowsArrivesUntouched) {
  for (const double target : {0.5, 1000.0, 300000.0}) {
    Api api;
    ParseApi(rt_request(map, "A", std::to_string(target), 1), Options::route, api);
    EXPECT_EQ(api.options().roundtrip().target_distance(), target);
  }
}

TEST_F(RoundTripFaultTargetBounds, TargetsOutsideTheContractAreBounded) {
  const std::vector<std::pair<std::string, double>> cases = {{"-5000", 0.0},
                                                             {"1e12", 300000.0},
                                                             {"1e308", 300000.0}};
  for (const auto& [target, bounded] : cases) {
    Api api;
    ParseApi(rt_request(map, "A", target, 1), Options::route, api);
    EXPECT_EQ(api.options().roundtrip().target_distance(), bounded) << target;
  }
}

TEST_F(RoundTripFaultTargetBounds, AProtobufRequestIsBoundedLikeAJsonOne) {
  // A protobuf body never passes through the JSON /roundtrip block.
  const auto infinity = std::numeric_limits<double>::infinity();
  for (const double target : {std::numeric_limits<double>::quiet_NaN(), -1.0, infinity}) {
    Api api;
    auto* options = api.mutable_options();
    options->set_costing_type(Costing::motorcycle);
    for (int i = 0; i < 2; ++i) {
      auto* ll = options->mutable_locations()->Add()->mutable_ll();
      ll->set_lat(map.nodes.at("A").lat());
      ll->set_lng(map.nodes.at("A").lng());
    }
    options->mutable_roundtrip()->set_target_distance(target);
    ParseApi("{}", Options::route, api);
    EXPECT_EQ(api.options().roundtrip().target_distance(), target > 0.0 ? 300000.0 : 0.0) << target;
  }
}

namespace {

// A reader whose one tile stops resolving after `allowed` fetches, the way a tile goes
// missing under a running request.
class VanishingTileReader : public baldr::GraphReader {
public:
  VanishingTileReader(const boost::property_tree::ptree& conf,
                      const baldr::GraphId& vanishing,
                      const uint32_t allowed)
      : baldr::GraphReader(conf), vanishing_(vanishing.tile_base()), allowed_(allowed) {
  }
  using baldr::GraphReader::GetGraphTile;
  baldr::graph_tile_ptr GetGraphTile(const baldr::GraphId& id) override {
    if (id.tile_base() == vanishing_ && ++fetches_ > allowed_)
      return nullptr;
    return baldr::GraphReader::GetGraphTile(id);
  }
  uint32_t fetches() const {
    return fetches_;
  }

private:
  baldr::GraphId vanishing_;
  uint32_t allowed_;
  uint32_t fetches_ = 0;
};

} // namespace

// T sits across the lon 1.0 tile boundary from the start, one edge out, so nothing walks
// the leg back off it before the turnaround is correlated.
class RoundTripFaultVanishingTile : public ::testing::Test {
protected:
  static gurka::map map;
  static baldr::GraphId far_tile;
  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A-------T
      |       |
      C-------D
    )";
    const gurka::ways ways = {
        {"AT", {{"highway", "secondary"}}},
        {"TD", {{"highway", "secondary"}}},
        {"DC", {{"highway", "secondary"}}},
        {"CA", {{"highway", "secondary"}}},
    };
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 1000, {0.975, 0.5});
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_vanishing_tile");
    map.config.put("thor.roundtrip_pair_pass", false);
    map.config.put("thor.roundtrip_road_identity", false);
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    const auto at = std::get<0>(gurka::findEdgeByNodes(*reader, layout, "A", "T"));
    const auto ta = std::get<0>(gurka::findEdgeByNodes(*reader, layout, "T", "A"));
    ASSERT_NE(at.tile_base(), ta.tile_base()) << "T must lie in another tile";
    far_tile = ta;
  }
};
gurka::map RoundTripFaultVanishingTile::map = {};
baldr::GraphId RoundTripFaultVanishingTile::far_tile = {};

TEST_F(RoundTripFaultVanishingTile, ATileGoneAtAnyPointCostsAtMostTheRequest) {
  // Walks the moment of loss through the whole request; the sweep ends at the first
  // request that never reached the limit.
  for (uint32_t allowed = 0;; ++allowed) {
    auto reader =
        std::make_shared<VanishingTileReader>(map.config.get_child("mjolnir"), far_tile, allowed);
    try {
      gurka::do_action(Options::route, map, rt_request(map, "A", "20000", 2), reader);
    } catch (const std::exception&) {
      // a failed request is fine: this test is about the process surviving it
    }
    if (reader->fetches() <= allowed)
      break;
    ASSERT_LT(allowed, 2000u) << "the request never stopped fetching the tile";
  }
}

namespace {

// A client that gave up: a poke past `stop_at` throws, as the service's own does.
struct ClientGoneAfter {
  uint32_t stop_at = std::numeric_limits<uint32_t>::max();
  uint32_t pokes = 0;
  void operator()() {
    if (++pokes > stop_at)
      throw std::runtime_error("client gone");
  }
};

// The pokes a request makes before its first loop is serialized.
uint32_t pokes_before_serialization(const gurka::map& map, const std::string& request) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  tyr::actor_t actor(map.config, *reader, true);
  Api api;
  uint32_t pokes = 0;
  const std::function<void()> interrupt = [&]() { pokes += api.trip().routes_size() == 0 ? 1 : 0; };
  try {
    actor.route(request, &interrupt, &api);
  } catch (const std::exception&) {
    // a request with nothing in band still ran the stages under test
  }
  return pokes;
}

} // namespace

class RoundTripFaultInterrupt : public ::testing::Test {
protected:
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
    map = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_interrupt");
  }
};
gurka::map RoundTripFaultInterrupt::map = {};

TEST_F(RoundTripFaultInterrupt, AClientThatGaveUpIsSeenAtEveryStageBeforeSerialization) {
  // After the expansion, the chain pass, the harvest and the pair pass, then one pair
  // evaluation, one build and the first serialized loop.
  EXPECT_GE(pokes_before_serialization(map, rt_request(map, "A", "14000", 1)), 7u);
}

TEST_F(RoundTripFaultInterrupt, TheBuilderWithoutThePairPassIsPolledToo) {
  auto no_pairs = map;
  no_pairs.config.put("thor.roundtrip_pair_pass", false);
  // After the expansion, the chain pass and the harvest, then one build and the first
  // serialized loop.
  EXPECT_GE(pokes_before_serialization(no_pairs, rt_request(no_pairs, "A", "14000", 1)), 5u);
}

TEST_F(RoundTripFaultInterrupt, EveryPokeAbortsTheWholeRequest) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto request = rt_request(map, "A", "14000", 2);
  uint32_t total = 0;
  {
    tyr::actor_t actor(map.config, *reader, true);
    Api api;
    ClientGoneAfter counter;
    const std::function<void()> interrupt = std::ref(counter);
    actor.route(request, &interrupt, &api);
    total = counter.pokes;
  }
  ASSERT_GT(total, 0u);
  for (uint32_t stop_at = 0; stop_at < total; ++stop_at) {
    tyr::actor_t actor(map.config, *reader, true);
    Api api;
    ClientGoneAfter gone{stop_at};
    const std::function<void()> interrupt = std::ref(gone);
    EXPECT_THROW(actor.route(request, &interrupt, &api), std::exception) << stop_at;
  }
}

TEST_F(RoundTripFaultInterrupt, PollingDoesNotChangeTheBank) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto request = rt_request(map, "A", "14000", 2);
  tyr::actor_t actor(map.config, *reader, true);
  Api polled, plain;
  const std::function<void()> interrupt = []() {};
  actor.route(request, &interrupt, &polled);
  actor.route(request, nullptr, &plain);
  EXPECT_EQ(gurka::detail::get_paths(polled), gurka::detail::get_paths(plain));
}

// Every pair of 94 nodes joined by a road: 8 742 directed edges, past the polling interval
// on far fewer nodes than a grid would need (a gurka node is one character).
class RoundTripFaultLongExpansion : public ::testing::Test {
protected:
  static gurka::map big, small;
  static void SetUpTestSuite() {
    constexpr double kRadiusDeg = 0.005;
    std::string names;
    for (char c = '!'; c <= '~'; ++c)
      names.push_back(c);
    gurka::nodelayout layout;
    gurka::ways ways;
    for (size_t i = 0; i < names.size(); ++i) {
      const double angle =
          2.0 * midgard::kPiD * static_cast<double>(i) / static_cast<double>(names.size());
      layout[std::string(1, names[i])] = {0.5 + kRadiusDeg * std::cos(angle),
                                          0.5 + kRadiusDeg * std::sin(angle)};
      for (size_t j = i + 1; j < names.size(); ++j)
        ways[std::string{names[i], names[j]}] = {{"highway", "residential"}};
    }
    big = gurka::buildtiles(layout, ways, {}, {}, "test/data/rt_fault_long_expansion");
    big.config.put("thor.roundtrip_road_identity", false);

    const std::string ascii_map = R"(
      A----B
      |    |
      C----D
    )";
    small = gurka::buildtiles(gurka::detail::map_to_coordinates(ascii_map, 1000),
                              {{"AB", {{"highway", "residential"}}},
                               {"BD", {{"highway", "residential"}}},
                               {"DC", {{"highway", "residential"}}},
                               {"CA", {{"highway", "residential"}}}},
                              {}, {}, "test/data/rt_fault_short_expansion");
    small.config.put("thor.roundtrip_road_identity", false);
  }
};
gurka::map RoundTripFaultLongExpansion::big = {};
gurka::map RoundTripFaultLongExpansion::small = {};

TEST_F(RoundTripFaultLongExpansion, TheExpansionItselfIsPolled) {
  // Nothing lies in band on either map, so both requests stop after the expansion and the
  // extra poke can only have come from inside the one big enough to reach the interval.
  EXPECT_GT(pokes_before_serialization(big, rt_request(big, "A", "300000", 1)),
            pokes_before_serialization(small, rt_request(small, "A", "300000", 1)));
}

#include "baldr/graphconstants.h"
#include "baldr/graphreader.h"
#include "baldr/graphtileheader.h"
#include "gurka.h"
#include "mjolnir/graphtilebuilder.h"
#include "test.h"
#include "tyr/actor.h"

#include <boost/property_tree/json_parser.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace valhalla;

namespace {

const std::string kAsciiMap = R"(
  A----B----C
       |
       D----E
)";

const gurka::ways kWays = {
    {"AB", {{"highway", "secondary"}}},
    {"BC", {{"highway", "secondary"}}},
    {"BD", {{"highway", "secondary"}}},
    {"DE", {{"highway", "secondary"}}},
};

gurka::map build_map(const std::string& name) {
  const auto layout = gurka::detail::map_to_coordinates(kAsciiMap, 100);
  return gurka::buildtiles(layout, kWays, {}, {}, "test/data/" + name);
}

// What a tile looks like when it was not built by this engine: the stock tools and the fork
// before the stamp leave the word 0, a later metric leaves its own number.
void rewrite_stamp(const gurka::map& map, const uint32_t version) {
  const auto tile_dir = map.config.get<std::string>("mjolnir.tile_dir");
  size_t rewritten = 0;
  for (std::filesystem::recursive_directory_iterator it(tile_dir), end; it != end; ++it) {
    if (it->path().extension() != ".gph") {
      continue;
    }
    std::fstream file(it->path(), std::ios::in | std::ios::out | std::ios::binary);
    baldr::GraphTileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    header.set_curvature_metric_version(version);
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    ASSERT_TRUE(file.good()) << it->path();
    ++rewritten;
  }
  ASSERT_GT(rewritten, 0u);
}

boost::property_tree::ptree status(const gurka::map& map) {
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  tyr::actor_t actor(map.config, *reader, true);
  std::stringstream json(actor.status(""));
  boost::property_tree::ptree pt;
  boost::property_tree::read_json(json, pt);
  return pt;
}

} // namespace

TEST(CurvatureMetricStamp, EveryTileABuildWritesCarriesTheEnginesVersion) {
  const auto map = build_map("curvature_metric_stamp_fresh");
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto tiles = reader->GetTileSet();
  ASSERT_FALSE(tiles.empty());
  for (const auto& tile_id : tiles) {
    EXPECT_EQ(reader->GetGraphTile(tile_id)->header()->curvature_metric_version(),
              baldr::kCurvatureMetricVersion)
        << tile_id;
  }
}

TEST(CurvatureMetricStamp, StatusMatchesWithoutVerboseOnAFreshBuild) {
  const auto map = build_map("curvature_metric_stamp_status");
  const auto pt = status(map);
  EXPECT_EQ(pt.get<std::string>("engine_curvature_metric"), "1");
  EXPECT_EQ(pt.get<std::string>("tileset_curvature_metric"), "1");
}

TEST(CurvatureMetricStamp, UnstampedTilesReadUnknownAndRouteAsBefore) {
  const auto map = build_map("curvature_metric_stamp_unstamped");
  const auto before = gurka::do_action(Options::route, map, {"A", "E"}, "motorcycle",
                                       {{"/costing_options/motorcycle/prefer_curvature", "1.0"}});

  rewrite_stamp(map, baldr::kCurvatureMetricUnknown);

  const auto pt = status(map);
  EXPECT_EQ(pt.get<std::string>("engine_curvature_metric"), "1");
  EXPECT_EQ(pt.get<std::string>("tileset_curvature_metric"), "unknown");

  const auto after = gurka::do_action(Options::route, map, {"A", "E"}, "motorcycle",
                                      {{"/costing_options/motorcycle/prefer_curvature", "1.0"}});
  gurka::assert::raw::expect_path(after, {"AB", "BD", "DE"});
  EXPECT_EQ(after.trip().routes(0).legs(0).shape(), before.trip().routes(0).legs(0).shape());
  EXPECT_EQ(after.directions().routes(0).legs(0).summary().time(),
            before.directions().routes(0).legs(0).summary().time());
}

TEST(CurvatureMetricStamp, TilesFromAnotherMetricShowAsAMismatch) {
  const auto map = build_map("curvature_metric_stamp_other");
  rewrite_stamp(map, baldr::kCurvatureMetricVersion + 1);

  const auto pt = status(map);
  EXPECT_NE(pt.get<std::string>("tileset_curvature_metric"),
            pt.get<std::string>("engine_curvature_metric"));
}

TEST(CurvatureMetricStamp, RewritingATileKeepsTheStampItCameWith) {
  const auto map = build_map("curvature_metric_stamp_rewrite");
  rewrite_stamp(map, baldr::kCurvatureMetricUnknown);

  // what valhalla_add_elevation and every later stage does to a tile: open, edit, store
  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  const auto tiles = reader->GetTileSet();
  ASSERT_FALSE(tiles.empty());
  const auto tile_id = *tiles.begin();
  mjolnir::GraphTileBuilder builder(map.config.get<std::string>("mjolnir.tile_dir"), tile_id, true);
  builder.StoreTileData();

  auto fresh_reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  EXPECT_EQ(fresh_reader->GetGraphTile(tile_id)->header()->curvature_metric_version(),
            baldr::kCurvatureMetricUnknown);
}

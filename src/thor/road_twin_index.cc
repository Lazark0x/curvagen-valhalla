// PROTOTYPE — proto/v4-p1 (curvagen-valhalla#10).  See valhalla/thor/road_twin_index.h.
#include "thor/road_twin_index.h"

#include "baldr/graphconstants.h"
#include "midgard/constants.h"
#include "midgard/logging.h"
#include "midgard/pointll.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace valhalla::baldr;
using namespace valhalla::midgard;

namespace valhalla {
namespace thor {

namespace {

// Shape sampling pitch.  30 m is the twin radius, so a 25 m pitch keeps a sample inside
// every twin-radius disc along the segment; the cap bounds the memory of a long rural
// edge (a 5 km edge would otherwise carry 200 samples).
constexpr double kSampleSpacingM = 25.0;
constexpr uint32_t kMaxSamplesPerEdge = 12;
// "over most of the shorter edge": the share of one segment's samples that must sit
// inside the radius of the other segment's polyline.
constexpr double kCoverFraction = 0.70;
// The share of the SHORTER segment's length that the matched run must span along the
// other segment.  Without it a continuation reads as a twin (see run() below).
constexpr double kSpanFraction = 0.60;
// Collinearity tolerance on the chord bearings (the ticket's "~180 deg +/- tolerance").
constexpr double kBearingTolDeg = 30.0;
// Spatial bin pitch; must be >= the widest radius so a 1-ring neighbourhood is complete.
constexpr double kCellM = 120.0;
// Serbia-scale constant for the lon/metre conversion of the BIN GRID only (distances
// themselves use the pair's own latitude).  cos(46 deg) keeps every cell >= kCellM in
// the north of the tileset, which is what the 1-ring completeness argument needs.
constexpr double kGridCosLat = 0.695;

struct Pt {
  int32_t lat_e6;
  int32_t lon_e6;
};

inline double norm_deg(double d) {
  while (d < 0.0)
    d += 360.0;
  while (d >= 360.0)
    d -= 360.0;
  return d;
}

// Angle between two undirected chord bearings, 0..90 after folding at 180 (collinear
// covers both the 0 deg and the 180 deg presentations — see the header's note).
inline double collinear_angle(double b1, double b2) {
  double d = std::fabs(norm_deg(b1) - norm_deg(b2));
  if (d > 180.0)
    d = 360.0 - d;
  return d > 90.0 ? 180.0 - d : d;
}

// Squared distance, metres, from p to the segment ab in a local flat frame, plus the
// clamped projection parameter (needed for the along-run span test below).
inline double seg_dist2(double px, double py, double ax, double ay, double bx, double by,
                        double& t_out) {
  const double vx = bx - ax, vy = by - ay;
  const double wx = px - ax, wy = py - ay;
  const double vv = vx * vx + vy * vy;
  double t = vv > 0.0 ? (wx * vx + wy * vy) / vv : 0.0;
  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  t_out = t;
  const double dx = wx - t * vx, dy = wy - t * vy;
  return dx * dx + dy * dy;
}

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

} // namespace

const RoadTwinIndex& RoadTwinIndex::get(GraphReader& reader,
                                        double twin_radius_m,
                                        double parallel_radius_m,
                                        bool build_parallels) {
  // Keyed by tileset location + radii: gurka builds many tilesets in one process.
  static std::map<std::string, std::unique_ptr<RoadTwinIndex>> registry;
  const std::string key = reader.GetTileSetLocation() + "|" + std::to_string(twin_radius_m) +
                          "|" + std::to_string(build_parallels ? parallel_radius_m : 0.0);
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto it = registry.find(key);
  if (it != registry.end())
    return *it->second;
  auto idx = std::unique_ptr<RoadTwinIndex>(new RoadTwinIndex());
  idx->Build(reader, twin_radius_m, parallel_radius_m, build_parallels);
  auto* raw = idx.get();
  registry.emplace(key, std::move(idx));
  return *raw;
}

void RoadTwinIndex::Build(GraphReader& reader,
                          double twin_radius_m,
                          double parallel_radius_m,
                          bool build_parallels) {
  const auto t0 = std::chrono::steady_clock::now();
  stats_.twin_radius_m = twin_radius_m;
  stats_.parallel_radius_m = build_parallels ? parallel_radius_m : 0.0;
  const double max_radius = build_parallels ? std::max(twin_radius_m, parallel_radius_m)
                                            : twin_radius_m;

  // ---- pass 1: sample every canonical (forward) drivable, non-shortcut edge --------
  std::vector<uint64_t> edge_ids;
  std::vector<uint32_t> samp_off;
  std::vector<uint16_t> samp_cnt;
  std::vector<float> bearing;
  std::vector<float> len_m; // edge length (metres) — the span test's yardstick
  std::vector<Pt> samples;
  std::vector<std::pair<uint64_t, uint32_t>> cell_entries; // (cell key, edge index)

  const int64_t cell_lat_e6 =
      static_cast<int64_t>(kCellM / kMetersPerDegreeLat * 1e6);
  const int64_t cell_lon_e6 =
      static_cast<int64_t>(kCellM / (kMetersPerDegreeLat * kGridCosLat) * 1e6);
  auto cell_key = [&](const Pt& p) {
    const int64_t ci = static_cast<int64_t>(std::floor(double(p.lat_e6) / cell_lat_e6));
    const int64_t cj = static_cast<int64_t>(std::floor(double(p.lon_e6) / cell_lon_e6));
    return (static_cast<uint64_t>(ci + (1 << 22)) << 32) |
           (static_cast<uint64_t>(cj + (1 << 22)) & 0xffffffffull);
  };
  auto cell_key_ij = [&](int64_t ci, int64_t cj) {
    return (static_cast<uint64_t>(ci + (1 << 22)) << 32) |
           (static_cast<uint64_t>(cj + (1 << 22)) & 0xffffffffull);
  };

  const auto tiles = reader.GetTileSet();
  stats_.tiles = tiles.size();
  std::vector<uint64_t> my_cells;
  for (const auto& tid : tiles) {
    graph_tile_ptr tile = reader.GetGraphTile(tid);
    if (!tile)
      continue;
    const uint32_t n = tile->header()->directededgecount();
    for (uint32_t i = 0; i < n; ++i) {
      const GraphId eid(tid.tileid(), tid.level(), i);
      const DirectedEdge* de = tile->directededge(eid);
      if (de->is_shortcut() || de->IsTransitLine() || de->use() == Use::kTransitConnection)
        continue;
      if (!de->forward()) // canonical = the edge carrying the stored shape direction
        continue;
      if (!((de->forwardaccess() | de->reverseaccess()) & kAutoAccess))
        continue;
      const auto shape = tile->edgeinfo(de).shape();
      if (shape.size() < 2)
        continue;

      // Sample at kSampleSpacingM, endpoints always in, capped.
      const double len = static_cast<double>(de->length());
      double pitch = std::max(kSampleSpacingM, len / (kMaxSamplesPerEdge - 1));
      const uint32_t idx = static_cast<uint32_t>(edge_ids.size());
      edge_ids.push_back(eid.value);
      samp_off.push_back(static_cast<uint32_t>(samples.size()));
      auto push = [&](const PointLL& p) {
        samples.push_back({static_cast<int32_t>(std::llround(p.lat() * 1e6)),
                           static_cast<int32_t>(std::llround(p.lng() * 1e6))});
      };
      push(shape.front());
      double since = 0.0;
      for (size_t s = 1; s < shape.size(); ++s) {
        const double d = shape[s - 1].Distance(shape[s]);
        since += d;
        if (since >= pitch && s + 1 < shape.size()) {
          push(shape[s]);
          since = 0.0;
        }
      }
      push(shape.back());
      samp_cnt.push_back(static_cast<uint16_t>(samples.size() - samp_off.back()));
      bearing.push_back(static_cast<float>(shape.front().Heading(shape.back())));
      len_m.push_back(static_cast<float>(len));

      my_cells.clear();
      for (uint32_t s = samp_off.back(); s < samples.size(); ++s)
        my_cells.push_back(cell_key(samples[s]));
      std::sort(my_cells.begin(), my_cells.end());
      my_cells.erase(std::unique(my_cells.begin(), my_cells.end()), my_cells.end());
      for (uint64_t c : my_cells)
        cell_entries.emplace_back(c, idx);
    }
    // Prototype: the tileset is small enough to keep resident; no eviction dance.
  }
  const uint32_t nedges = static_cast<uint32_t>(edge_ids.size());
  samp_off.push_back(static_cast<uint32_t>(samples.size()));
  stats_.edges_indexed = nedges;
  stats_.samples = samples.size();

  // ---- CSR over the bin grid -------------------------------------------------------
  std::sort(cell_entries.begin(), cell_entries.end());
  std::vector<uint64_t> cell_keys;
  std::vector<uint32_t> cell_off, cell_edges;
  cell_edges.reserve(cell_entries.size());
  for (size_t i = 0; i < cell_entries.size();) {
    const uint64_t k = cell_entries[i].first;
    cell_keys.push_back(k);
    cell_off.push_back(static_cast<uint32_t>(cell_edges.size()));
    while (i < cell_entries.size() && cell_entries[i].first == k)
      cell_edges.push_back(cell_entries[i++].second);
  }
  cell_off.push_back(static_cast<uint32_t>(cell_edges.size()));
  stats_.peak_bytes = cell_entries.size() * sizeof(cell_entries[0]) +
                      samples.size() * sizeof(Pt) + cell_edges.size() * 4 +
                      cell_keys.size() * 12 + nedges * 18;
  std::vector<std::pair<uint64_t, uint32_t>>().swap(cell_entries);

  // ---- pass 2: pair every edge against its 1-ring bin neighbourhood ----------------
  const double twin_r2 = twin_radius_m * twin_radius_m;
  const double max_r2 = max_radius * max_radius;
  std::vector<uint32_t> stamp(nedges, 0);
  std::vector<uint32_t> cand;
  std::vector<std::pair<uint64_t, uint64_t>> twin_pairs, par_pairs;

  for (uint32_t u = 0; u < nedges; ++u) {
    const uint32_t uo = samp_off[u], uc = samp_cnt[u];
    // candidate gather
    cand.clear();
    my_cells.clear();
    for (uint32_t s = uo; s < uo + uc; ++s) {
      const int64_t ci = static_cast<int64_t>(std::floor(double(samples[s].lat_e6) / cell_lat_e6));
      const int64_t cj = static_cast<int64_t>(std::floor(double(samples[s].lon_e6) / cell_lon_e6));
      for (int64_t di = -1; di <= 1; ++di)
        for (int64_t dj = -1; dj <= 1; ++dj)
          my_cells.push_back(cell_key_ij(ci + di, cj + dj));
    }
    std::sort(my_cells.begin(), my_cells.end());
    my_cells.erase(std::unique(my_cells.begin(), my_cells.end()), my_cells.end());
    for (uint64_t k : my_cells) {
      auto it = std::lower_bound(cell_keys.begin(), cell_keys.end(), k);
      if (it == cell_keys.end() || *it != k)
        continue;
      const size_t ci = static_cast<size_t>(it - cell_keys.begin());
      for (uint32_t j = cell_off[ci]; j < cell_off[ci + 1]; ++j) {
        const uint32_t v = cell_edges[j];
        if (v <= u || stamp[v] == u + 1)
          continue;
        stamp[v] = u + 1;
        cand.push_back(v);
      }
    }
    if (cand.empty())
      continue;

    // local flat frame at u's first sample
    const double lat0 = samples[uo].lat_e6 * 1e-6;
    const double m_lat = kMetersPerDegreeLat * 1e-6;
    const double m_lon = kMetersPerDegreeLat * std::cos(lat0 * kRadPerDeg) * 1e-6;
    auto X = [&](const Pt& p) { return p.lon_e6 * m_lon; };
    auto Y = [&](const Pt& p) { return p.lat_e6 * m_lat; };

    for (uint32_t v : cand) {
      if (collinear_angle(bearing[u], bearing[v]) > kBearingTolDeg)
        continue;
      ++stats_.pair_tests;
      const uint32_t vo = samp_off[v], vc = samp_cnt[v];

      // A pair qualifies as "the same physical road, two carriageways" on TWO tests,
      // both needed:
      //   COVER — the share of a's samples whose distance to b's polyline is <= r;
      //   SPAN  — the along-b extent those samples project onto, as a share of the
      //           shorter polyline.
      // The span test is what separates a parallel run from a CONTINUATION.  Two short
      // consecutive edges (or two edges crossing at a junction) are collinear and every
      // sample sits within 30 m of the shared node, so cover alone reads 100 % — but
      // every sample projects onto the same point of the neighbour, so the span is ~0.
      // Without it the Serbia index called 40 % of all segments twins.
      auto run = [&](uint32_t ao, uint32_t ac, uint32_t bo, uint32_t bc, double r2,
                     double& cov, double& span) {
        double blen[kMaxSamplesPerEdge + 2];
        double acc = 0.0;
        blen[0] = 0.0;
        for (uint32_t t = 0; t + 1 < bc; ++t) {
          const double dx = X(samples[bo + t + 1]) - X(samples[bo + t]);
          const double dy = Y(samples[bo + t + 1]) - Y(samples[bo + t]);
          acc += std::sqrt(dx * dx + dy * dy);
          blen[t + 1] = acc;
        }
        uint32_t n = 0;
        double smin = std::numeric_limits<double>::max(), smax = -1.0;
        for (uint32_t s = ao; s < ao + ac; ++s) {
          const double px = X(samples[s]), py = Y(samples[s]);
          double best = std::numeric_limits<double>::max(), best_s = 0.0, tt = 0.0;
          for (uint32_t t = 0; t + 1 < bc; ++t) {
            const double d2 = seg_dist2(px, py, X(samples[bo + t]), Y(samples[bo + t]),
                                        X(samples[bo + t + 1]), Y(samples[bo + t + 1]), tt);
            if (d2 < best) {
              best = d2;
              best_s = blen[t] + tt * (blen[t + 1] - blen[t]);
            }
          }
          if (best <= r2) {
            ++n;
            smin = std::min(smin, best_s);
            smax = std::max(smax, best_s);
          }
        }
        cov = ac ? double(n) / ac : 0.0;
        span = (smax >= smin) ? (smax - smin) : 0.0;
      };
      auto qualifies = [&](double r2) {
        double c1 = 0, s1 = 0, c2 = 0, s2 = 0;
        run(uo, uc, vo, vc, r2, c1, s1);
        run(vo, vc, uo, uc, r2, c2, s2);
        const double shorter = std::min(len_m[u], len_m[v]);
        const bool q1 = c1 >= kCoverFraction && s1 >= kSpanFraction * shorter;
        const bool q2 = c2 >= kCoverFraction && s2 >= kSpanFraction * shorter;
        return q1 || q2;
      };
      if (qualifies(twin_r2))
        twin_pairs.emplace_back(edge_ids[u], edge_ids[v]);
      else if (build_parallels && qualifies(max_r2))
        par_pairs.emplace_back(edge_ids[u], edge_ids[v]);
    }
  }
  stats_.twin_pairs = twin_pairs.size();
  stats_.parallel_pairs = par_pairs.size();

  // ---- emit CSR maps (both directions of each pair) ---------------------------------
  auto emit = [](std::vector<std::pair<uint64_t, uint64_t>>& pairs, std::vector<uint64_t>& keys,
                 std::vector<uint32_t>& off, std::vector<uint64_t>& vals) {
    std::vector<std::pair<uint64_t, uint64_t>> both;
    both.reserve(pairs.size() * 2);
    for (const auto& p : pairs) {
      both.emplace_back(p.first, p.second);
      both.emplace_back(p.second, p.first);
    }
    std::vector<std::pair<uint64_t, uint64_t>>().swap(pairs);
    std::sort(both.begin(), both.end());
    both.erase(std::unique(both.begin(), both.end()), both.end());
    vals.reserve(both.size());
    for (size_t i = 0; i < both.size();) {
      const uint64_t k = both[i].first;
      keys.push_back(k);
      off.push_back(static_cast<uint32_t>(vals.size()));
      while (i < both.size() && both[i].first == k)
        vals.push_back(both[i++].second);
    }
    off.push_back(static_cast<uint32_t>(vals.size()));
  };
  emit(twin_pairs, twin_keys_, twin_off_, twin_vals_);
  emit(par_pairs, par_keys_, par_off_, par_vals_);
  stats_.twin_keys = twin_keys_.size();
  stats_.parallel_keys = par_keys_.size();
  stats_.retained_bytes = twin_keys_.size() * 8 + twin_off_.size() * 4 + twin_vals_.size() * 8 +
                          par_keys_.size() * 8 + par_off_.size() * 4 + par_vals_.size() * 8;
  stats_.build_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  LOG_INFO("roundtrip road identity sidecar: built in " +
           std::to_string(static_cast<int>(stats_.build_ms)) + " ms — " +
           std::to_string(stats_.tiles) + " tiles, " + std::to_string(stats_.edges_indexed) +
           " segments, " + std::to_string(stats_.samples) + " shape samples, " +
           std::to_string(stats_.pair_tests) + " pair tests; twins " +
           std::to_string(stats_.twin_pairs) + " pairs / " + std::to_string(stats_.twin_keys) +
           " keyed segments (r<=" + std::to_string(static_cast<int>(twin_radius_m)) +
           " m), parallels " + std::to_string(stats_.parallel_pairs) + " pairs / " +
           std::to_string(stats_.parallel_keys) + " keyed segments (r<=" +
           std::to_string(static_cast<int>(stats_.parallel_radius_m)) + " m); retained " +
           std::to_string(stats_.retained_bytes / 1024 / 1024) + " MiB, peak ~" +
           std::to_string(stats_.peak_bytes / 1024 / 1024) + " MiB");
}

} // namespace thor
} // namespace valhalla

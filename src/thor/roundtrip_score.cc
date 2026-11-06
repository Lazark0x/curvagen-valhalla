#include "thor/roundtrip_score.h"
#include "thor/road_twin_index.h"

#include <cmath>

namespace valhalla {
namespace thor {
namespace roundtrip {

LoopEdge resolve_edge(const baldr::GraphId& edge,
                      double seg_m,
                      double from_start_m,
                      baldr::GraphReader& reader,
                      baldr::graph_tile_ptr& tile) {
  LoopEdge le;
  le.edge = edge;
  le.seg_m = seg_m;
  le.from_start_m = from_start_m;
  if (!edge.is_valid())
    return le;
  if (reader.GetGraphTile(edge, tile)) {
    const baldr::DirectedEdge* de = tile->directededge(edge);
    le.resolved = true;
    le.curvature = static_cast<double>(de->curvature());
    le.length_m = static_cast<double>(de->length());
    baldr::graph_tile_ptr opp_tile = tile;
    le.opp = reader.GetOpposingEdgeId(edge, opp_tile);
  } else {
    le.opp = reader.GetOpposingEdgeId(edge);
  }
  le.canon = RoadTwinIndex::canonical_id(nullptr, edge, le.opp);
  return le;
}

std::vector<LoopEdge>
leg_edges(const std::vector<PathInfo>& leg, bool is_return, baldr::GraphReader& reader) {
  std::vector<LoopEdge> out;
  out.reserve(leg.size());
  const double total = leg.empty() ? 0.0 : static_cast<double>(leg.back().path_distance);
  double prev = 0.0;
  baldr::graph_tile_ptr tile;
  for (const auto& pi : leg) {
    const double cum = static_cast<double>(pi.path_distance);
    const double seg = cum - prev;
    prev = cum;
    out.push_back(resolve_edge(pi.edgeid, seg, is_return ? total - cum : cum, reader, tile));
  }
  return out;
}

double shared_fraction(const LoopKeys& a, const LoopKeys& b) {
  if (a.total <= 0.0)
    return 0.0;
  double shared = 0.0;
  for (const auto& [k, len] : a.ridden)
    if (b.ridden.count(k) || b.twins.count(k))
      shared += len;
  return shared / a.total;
}

double fwd_shared_fraction(const LoopKeys& cand, const LoopKeys& prev) {
  if (cand.fwd_total <= 0.0)
    return 0.0;
  double shared = 0.0;
  for (const auto& [k, len] : cand.fwd_ridden)
    if (prev.ridden.count(k) || prev.twins.count(k))
      shared += len;
  return shared / cand.fwd_total;
}

bool near_copies(const LoopKeys& a, const LoopKeys& b, double frac) {
  return shared_fraction(a, b) > frac || shared_fraction(b, a) > frac;
}

PairMeasure measure_pair(const std::vector<LoopEdge>& tree,
                         const std::vector<LoopEdge>& other,
                         const TwinsFn& twins,
                         double exemption_m) {
  PairMeasure m;
  std::vector<uint64_t> tw;
  std::unordered_set<uint64_t> fwd_ids, ret_ids;
  double curv = 0.0, len = 0.0, overlap = 0.0;
  for (const auto& e : tree) {
    len += e.seg_m;
    if (!e.resolved)
      continue;
    curv += e.curvature * e.seg_m;
    if (e.from_start_m <= exemption_m)
      continue;
    fwd_ids.insert(e.canon);
    m.keys.ridden[e.canon] += e.seg_m;
    m.keys.fwd_ridden[e.canon] += e.seg_m;
    if (twins) {
      tw.clear();
      twins(e.canon, tw);
      fwd_ids.insert(tw.begin(), tw.end());
      m.keys.twins.insert(tw.begin(), tw.end());
    }
  }
  m.keys.fwd_total = len; // the tree path's whole length, exempt stem included
  for (const auto& e : other) {
    len += e.seg_m;
    if (!e.resolved)
      continue;
    curv += e.curvature * e.seg_m;
    if (e.from_start_m <= exemption_m)
      continue;
    bool hit = fwd_ids.count(e.canon) > 0 || !ret_ids.insert(e.canon).second;
    if (twins) {
      tw.clear();
      twins(e.canon, tw);
      for (uint64_t t : tw)
        hit = hit || ret_ids.count(t) > 0;
      m.keys.twins.insert(tw.begin(), tw.end());
    }
    if (hit)
      overlap += e.seg_m;
    m.keys.ridden[e.canon] += e.seg_m;
  }
  m.keys.total = len;
  m.curviness = len > 0.0 ? static_cast<float>(curv / len / 15.0) : 0.f;
  m.overlap_m = overlap;
  return m;
}

BuiltMeasure measure_built(const std::vector<LoopEdge>& fwd,
                           const std::vector<LoopEdge>& ret,
                           const TwinsFn& twins,
                           bool return_vs_return,
                           double exemption_m) {
  std::vector<uint64_t> tw;
  std::unordered_set<uint64_t> fwd_ids, ret_ids;
  double curv = 0.0, len = 0.0, overlap = 0.0;
  for (const auto& e : fwd) {
    if (!e.resolved)
      continue;
    curv += e.curvature * e.length_m;
    len += e.length_m;
    if (e.from_start_m <= exemption_m)
      continue;
    fwd_ids.insert(e.canon);
    if (twins) {
      tw.clear();
      twins(e.canon, tw);
      fwd_ids.insert(tw.begin(), tw.end());
    }
  }
  for (const auto& e : ret) {
    if (!e.resolved)
      continue;
    curv += e.curvature * e.length_m;
    len += e.length_m;
    if (e.from_start_m <= exemption_m)
      continue;
    bool hit = fwd_ids.count(e.canon) > 0;
    if (return_vs_return) {
      if (!ret_ids.insert(e.canon).second)
        hit = true;
      if (!hit && twins) {
        tw.clear();
        twins(e.canon, tw);
        for (uint64_t t : tw)
          if (ret_ids.count(t)) {
            hit = true;
            break;
          }
      }
    }
    if (hit)
      overlap += e.seg_m;
  }
  BuiltMeasure m;
  m.curviness = len > 0.0 ? static_cast<float>(curv / len / 15.0) : 0.0f;
  m.overlap_m = overlap;
  return m;
}

double built_keys(const std::vector<LoopEdge>& fwd,
                  const std::vector<LoopEdge>& ret,
                  double fwd_total,
                  double ret_total,
                  const TwinsFn& twins,
                  double exemption_m,
                  std::unordered_map<uint64_t, double>& ridden,
                  std::unordered_map<uint64_t, double>& twin_metres) {
  std::vector<uint64_t> tw;
  for (const auto* leg : {&fwd, &ret}) {
    for (const auto& e : *leg) {
      if (e.from_start_m <= exemption_m)
        continue;
      ridden[e.canon] += e.seg_m;
      if (twins) {
        tw.clear();
        twins(e.canon, tw);
        for (uint64_t t : tw)
          twin_metres[t] += e.seg_m;
      }
    }
  }
  return fwd_total + ret_total;
}

double distance_error(double actual_m, double target_m) {
  return target_m > 0.0 ? std::fabs(actual_m - target_m) / target_m : 0.0;
}

double pair_score(float curviness, double dist_err, double disterr_w) {
  return static_cast<double>(curviness) / (1.0 + disterr_w * dist_err);
}

double built_score(float curviness,
                   double overlap_m,
                   double total_m,
                   double dist_err,
                   double overlap_w,
                   double disterr_w) {
  const double ov_frac = total_m > 0.0 ? overlap_m / total_m : 0.0;
  return static_cast<double>(curviness) /
         ((1.0 + overlap_w * ov_frac) * (1.0 + disterr_w * dist_err));
}

} // namespace roundtrip
} // namespace thor
} // namespace valhalla

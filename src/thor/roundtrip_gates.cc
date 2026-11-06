#include "thor/roundtrip_gates.h"

#include <algorithm>
#include <cmath>

namespace valhalla {
namespace thor {
namespace roundtrip {

void GridTrace::add(const midgard::PointLL& p) {
  std::pair<int64_t, int64_t> k{std::llround(p.lat() * 1e5), std::llround(p.lng() * 1e5)};
  if (!pts.empty() && pts.back() == k)
    return;
  if (!pts.empty())
    cum.push_back(cum.back() + midgard::PointLL(pts.back().second / 1e5, pts.back().first / 1e5)
                                   .Distance(midgard::PointLL(k.second / 1e5, k.first / 1e5)));
  pts.push_back(k);
}

bool append_leg(const std::vector<PathInfo>& leg,
                size_t from,
                size_t to,
                baldr::GraphReader& reader,
                GridTrace& trace) {
  for (size_t li = from; li < to; ++li) {
    baldr::graph_tile_ptr tile = reader.GetGraphTile(leg[li].edgeid);
    if (!tile)
      return false;
    const baldr::DirectedEdge* de = tile->directededge(leg[li].edgeid);
    auto shape = tile->edgeinfo(de).shape();
    if (!de->forward())
      std::reverse(shape.begin(), shape.end());
    for (const auto& p : shape)
      trace.add(p);
  }
  return true;
}

double longest_mirror_stub_m(const GridTrace& trace, size_t seam_idx) {
  const auto& pts = trace.pts;
  const auto& cum = trace.cum;
  if (pts.size() < 3)
    return 0.0;
  double worst = 0.0;
  for (size_t i = 1; i + 1 < pts.size(); ++i) {
    if (pts[i - 1] != pts[i + 1])
      continue;
    size_t w = 1;
    while (i >= 1 + w && i + 1 + w < pts.size() && pts[i - 1 - w] == pts[i + 1 + w])
      ++w;
    const bool counts = seam_idx == kAnywhere || (i - w <= seam_idx + 2 && i + w + 2 >= seam_idx);
    if (counts) {
      const double stub = cum[i] - cum[i - w];
      if (stub > worst)
        worst = stub;
    }
    i += w; // past this palindrome
  }
  return worst;
}

SeamWindow
seam_window(const std::vector<PathInfo>& fwd, const std::vector<PathInfo>& ret, double window_m) {
  if (window_m <= 0.0)
    return {0, ret.size()};
  auto edge_m = [](const std::vector<PathInfo>& leg, size_t i) {
    return static_cast<double>(leg[i].path_distance - (i ? leg[i - 1].path_distance : 0.f));
  };
  SeamWindow w{fwd.size(), 0};
  double acc = 0.0;
  while (w.fwd_begin > 0 && acc < window_m) {
    --w.fwd_begin;
    acc += edge_m(fwd, w.fwd_begin);
  }
  acc = 0.0;
  while (w.ret_end < ret.size() && acc < window_m) {
    acc += edge_m(ret, w.ret_end);
    ++w.ret_end;
  }
  return w;
}

double seam_stub_m(const std::vector<PathInfo>& fwd,
                   const std::vector<PathInfo>& ret,
                   baldr::GraphReader& reader,
                   double window_m) {
  const SeamWindow w = seam_window(fwd, ret, window_m);
  GridTrace trace;
  if (!append_leg(fwd, w.fwd_begin, fwd.size(), reader, trace))
    return 0.0;
  const size_t seam_idx = trace.pts.empty() ? 0 : trace.pts.size() - 1;
  if (!append_leg(ret, 0, w.ret_end, reader, trace))
    return 0.0;
  return longest_mirror_stub_m(trace, seam_idx);
}

double leg_mirror_stub_m(const std::vector<PathInfo>& leg, baldr::GraphReader& reader) {
  GridTrace trace;
  if (!append_leg(leg, 0, leg.size(), reader, trace))
    return 0.0;
  return longest_mirror_stub_m(trace);
}

GateAction gate_action(const GateVerdict& verdict, uint32_t refills_used, uint32_t refill_budget) {
  if (!verdict.rejected())
    return GateAction::kServe;
  if (verdict.seam)
    return GateAction::kStashSeam;
  if (refills_used < refill_budget)
    return GateAction::kStashGeometry;
  return GateAction::kKeepGated;
}

bool stash_before(const StashRank& a, const StashRank& b) {
  if (a.seam != b.seam)
    return !a.seam;
  return a.score > b.score;
}

size_t
stash_slot(const StashRank& loop, const std::vector<StashRank>& stash, size_t cap, size_t dup) {
  if (dup != kNoSlot)
    return stash_before(loop, stash[dup]) ? dup : kNoSlot;
  if (stash.size() < cap)
    return stash.size();
  size_t last = kNoSlot;
  for (size_t i = 0; i < stash.size(); ++i)
    if (last == kNoSlot || !stash_before(stash[i], stash[last]))
      last = i;
  return last != kNoSlot && stash_before(loop, stash[last]) ? last : kNoSlot;
}

} // namespace roundtrip
} // namespace thor
} // namespace valhalla

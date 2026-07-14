#include "thor/roundtrip_expansion.h"
#include "baldr/graphconstants.h"
#include "midgard/pointll.h"

#include <algorithm>
#include <cmath>

using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::midgard;

namespace valhalla {
namespace thor {

namespace {
constexpr float kDistanceBand = 0.18f; // accept turnarounds within ±18% of target/2
// A real turnaround must be far from the start in straight-line distance too, else a
// there-and-back path (right path distance, ~0 straight-line) reads as a turnaround and
// yields a degenerate out-and-back loop. Also disfavors near-start lollipops.
constexpr float kMinStraightFraction = 0.3f; // >= 0.3 * target/2 straight-line from start
// Explore all road levels within this radius of the start; beyond it only arterials/highways
// carry the expansion. Bounds the settled-node count on long loops so they stay feasible
// (ADR-0033 hierarchy pruning), at the cost of far-out curviness. Sized so loops up to
// ~150km (max_meters ~90km) are fully explored — unchanged from the unpruned behavior.
constexpr float kFullExploreRadiusM = 90000.0f;
} // namespace

RoundTripExpansion::RoundTripExpansion(const boost::property_tree::ptree& config)
    : Dijkstras(config) {
}

void RoundTripExpansion::GetExpansionHints(uint32_t& bucket_count,
                                           uint32_t& edge_label_reservation) const {
  bucket_count = 20000;
  edge_label_reservation = 1000000; // bounded expansion; far below isochrone default
}

// Prune purely on path distance (round trip cares about metres, not seconds).
ExpansionRecommendation RoundTripExpansion::ShouldExpand(GraphReader&,
                                                         const EdgeLabel& pred,
                                                         const ExpansionType) {
  uint32_t dist = pred.predecessor() == kInvalidLabel
                      ? 0
                      : bdedgelabels_[pred.predecessor()].path_distance();
  if (dist > static_cast<uint32_t>(max_meters_))
    return ExpansionRecommendation::prune_expansion;
  // Beyond the near radius, stop growing local roads (level 2) — only arterials/highways
  // (levels 0/1) carry the expansion out to the far turnaround band. Keeps long loops feasible.
  if (dist > static_cast<uint32_t>(near_radius_) && pred.edgeid().level() >= 2)
    return ExpansionRecommendation::prune_expansion;
  return ExpansionRecommendation::continue_expansion;
}

std::vector<Turnaround> RoundTripExpansion::Harvest(valhalla::Api& api,
                                                    GraphReader& reader,
                                                    const mode_costing_t& mode_costing,
                                                    const sif::TravelMode mode,
                                                    double target_distance_m) {
  const double target_half = target_distance_m * 0.5;
  max_meters_ = static_cast<float>(target_half * 1.2);
  near_radius_ = std::min(max_meters_, kFullExploreRadiusM);

  // Run the forward expansion (fills bdedgelabels_). Dijkstras::Expand dispatches
  // to Compute<ExpansionType::forward> using api.options().locations() as origins.
  Dijkstras::Expand(ExpansionType::forward, api, reader, mode_costing, mode);

  // Start point for bearing computation.
  const auto& start_ll_pb = api.options().locations(0).ll();
  const PointLL start_ll{start_ll_pb.lng(), start_ll_pb.lat()};

  return ScanBand(reader, start_ll, target_distance_m, 1.0f - kDistanceBand,
                  1.0f + kDistanceBand);
}

std::vector<Turnaround> RoundTripExpansion::ScanBand(GraphReader& reader,
                                                     const PointLL& start_ll,
                                                     double target_distance_m,
                                                     float lo_frac,
                                                     float hi_frac) {
  const double target_half = target_distance_m * 0.5;
  const uint32_t lo = static_cast<uint32_t>(target_half * lo_frac);
  const uint32_t hi = static_cast<uint32_t>(target_half * hi_frac);

  // Post-hoc curviness-per-km + bounce rejection (ADR-0037 turnaround hardening: the
  // costing's own U-turn test — pred.opp_local_idx() == edge.localedgeidx() — admits
  // U-turns at dead ends, so a chain can legally ride into a spur and bounce back; a
  // bounced chain bakes the out-and-back stub into the forward leg).
  // A per-label chain walk makes this O(labels x chain depth) with a tile lookup per
  // step — the measured harvest tail on 200-300 km requests (wayfinder #54). Chains
  // share their prefixes, so the sums memoize once per label instead: lazy DP over the
  // predecessor forest. An unresolvable tile skips that step's contribution and bounce
  // test, exactly like the walk did.
  const uint32_t n = static_cast<uint32_t>(bdedgelabels_.size());
  constexpr double kUnresolved = -1.0;
  std::vector<double> curv_sum(n, kUnresolved);
  std::vector<double> len_sum(n, 0.0);
  std::vector<uint8_t> bounced(n, 0);
  std::vector<uint32_t> chain;
  auto resolve = [&](uint32_t leaf) {
    chain.clear();
    for (uint32_t j = leaf; j != kInvalidLabel && curv_sum[j] == kUnresolved;
         j = bdedgelabels_[j].predecessor())
      chain.push_back(j);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const uint32_t k = *it;
      const uint32_t pred = bdedgelabels_[k].predecessor();
      const double base_curv = pred != kInvalidLabel ? curv_sum[pred] : 0.0;
      const double base_len = pred != kInvalidLabel ? len_sum[pred] : 0.0;
      const uint8_t base_bounced = pred != kInvalidLabel ? bounced[pred] : 0;
      const GraphId eid = bdedgelabels_[k].edgeid();
      graph_tile_ptr tile = reader.GetGraphTile(eid);
      if (!tile) {
        curv_sum[k] = base_curv;
        len_sum[k] = base_len;
        bounced[k] = base_bounced;
        continue;
      }
      const DirectedEdge* de = tile->directededge(eid);
      const bool bounce_here = pred != kInvalidLabel &&
                               bdedgelabels_[pred].opp_local_idx() == de->localedgeidx();
      bounced[k] = base_bounced || bounce_here ? 1 : 0;
      curv_sum[k] = base_curv + static_cast<double>(de->curvature()) * de->length();
      len_sum[k] = base_len + de->length();
    }
  };

  std::vector<Turnaround> out;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t pd = bdedgelabels_[i].path_distance();
    if (pd < lo || pd > hi)
      continue;
    resolve(i);
    if (bounced[i] || len_sum[i] <= 0.0)
      continue;

    // The turnaround node = end node of the label's leading edge; endnode may live in
    // a different tile than the edge. Walk past unresolvable tiles like the sums do.
    PointLL node_ll = start_ll;
    GraphId turn_node;
    bool got_node = false;
    for (uint32_t l = i; l != kInvalidLabel && !got_node;
         l = bdedgelabels_[l].predecessor()) {
      const GraphId eid = bdedgelabels_[l].edgeid();
      graph_tile_ptr tile = reader.GetGraphTile(eid);
      if (!tile)
        continue;
      turn_node = tile->directededge(eid)->endnode();
      graph_tile_ptr ntile = reader.GetGraphTile(turn_node);
      if (ntile) {
        node_ll = ntile->get_node_ll(turn_node);
        got_node = true;
      }
    }
    if (!got_node)
      continue;

    // Reject turnarounds whose node sits near the start (there-and-back / tiny loop).
    // Proportional to the candidate's own path distance so the widened flex band's
    // shorter candidates are judged fairly (default band: pd ~ target_half — unchanged).
    if (static_cast<double>(start_ll.Distance(node_ll)) < pd * kMinStraightFraction)
      continue;

    Turnaround t;
    t.label_index = i;
    t.path_distance = pd;
    t.bearing_deg = static_cast<float>(start_ll.Heading(node_ll)); // 0..360
    // curvature() is 0..15; normalise to 0..1 per km-equivalent for ranking only.
    t.curviness_per_km = static_cast<float>(curv_sum[i] / len_sum[i] / 15.0);
    t.node = turn_node.value;
    t.ll = node_ll;
    out.push_back(t);
  }
  return out;
}

} // namespace thor
} // namespace valhalla

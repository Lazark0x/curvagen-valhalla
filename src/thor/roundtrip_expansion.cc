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

  std::vector<Turnaround> out;
  for (uint32_t i = 0; i < bdedgelabels_.size(); ++i) {
    const uint32_t pd = bdedgelabels_[i].path_distance();
    if (pd < lo || pd > hi)
      continue;

    // Post-hoc curviness-per-km: walk the predecessor chain summing curvature*len.
    // The same walk rejects bounced chains (ADR-0037 turnaround hardening): the
    // costing's own U-turn test (pred.opp_local_idx() == edge.localedgeidx()) admits
    // U-turns at dead ends, so a chain can legally ride into a spur and bounce back —
    // and harvesting it bakes the out-and-back stub into the forward leg, a spike no
    // return leg can undo.
    double turn_sum = 0.0, len_sum = 0.0;
    PointLL node_ll = start_ll;
    GraphId turn_node;
    bool got_node = false;
    bool bounced = false;
    for (uint32_t l = i; l != kInvalidLabel; l = bdedgelabels_[l].predecessor()) {
      const GraphId eid = bdedgelabels_[l].edgeid();
      graph_tile_ptr tile = reader.GetGraphTile(eid);
      if (!tile)
        continue;
      const DirectedEdge* de = tile->directededge(eid);
      const uint32_t pred = bdedgelabels_[l].predecessor();
      if (pred != kInvalidLabel &&
          bdedgelabels_[pred].opp_local_idx() == de->localedgeidx()) {
        bounced = true;
        break;
      }
      turn_sum += static_cast<double>(de->curvature()) * de->length();
      len_sum += de->length();
      if (!got_node) { // the turnaround node = end node of its leading edge
        // endnode may live in a different tile than the edge; fetch its own tile.
        turn_node = de->endnode();
        graph_tile_ptr ntile = reader.GetGraphTile(turn_node);
        if (ntile) {
          node_ll = ntile->get_node_ll(turn_node);
          got_node = true;
        }
      }
    }
    if (bounced || len_sum <= 0.0 || !got_node)
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
    t.curviness_per_km = static_cast<float>(turn_sum / len_sum / 15.0);
    t.node = turn_node.value;
    t.ll = node_ll;
    out.push_back(t);
  }
  return out;
}

} // namespace thor
} // namespace valhalla

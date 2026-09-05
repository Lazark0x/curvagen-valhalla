#ifndef VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_
#define VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_

#include "thor/dijkstras.h"
#include "thor/road_twin_index.h"

#include <cstdint>
#include <vector>

namespace valhalla {
namespace thor {

// A turnaround candidate harvested from the forward expansion tree.
struct Turnaround {
  uint32_t label_index;   // index into bdedgelabels_
  uint32_t path_distance; // metres from start along the curvy-cost-optimal path
  float bearing_deg;      // straight-line bearing start -> turnaround node (0..360)
  float curviness_per_km; // post-hoc: sum(curvature*len)/len over the reconstructed path
  uint64_t node;          // turnaround node GraphId value (for dedup + correlation)
  midgard::PointLL ll;    // turnaround node location (for the min-separation guard)
};

// One forward Dijkstra expansion from the start under the curvy motorcycle costing,
// bounded to ~half the target loop distance, then harvest turnaround candidates from
// the settled label tree. No EdgeLabel change: curviness is read post-hoc (ADR-0033).
class RoundTripExpansion : public Dijkstras {
public:
  explicit RoundTripExpansion(const boost::property_tree::ptree& config = {});

  // Run the expansion; return turnaround candidates with path_distance in
  // [target_half*(1-band), target_half*(1+band)].
  std::vector<Turnaround> Harvest(valhalla::Api& api,
                                  baldr::GraphReader& reader,
                                  const sif::mode_costing_t& mode_costing,
                                  const sif::TravelMode mode,
                                  double target_distance_m);

  // Re-scan the settled label tree for turnaround candidates in an arbitrary band
  // [target_half*lo_frac, target_half*hi_frac] — the ADR-0037 Distance Flex pool.
  // Requires a prior Harvest() (the expansion must have run).
  std::vector<Turnaround> ScanBand(baldr::GraphReader& reader,
                                   const midgard::PointLL& start_ll,
                                   double target_distance_m,
                                   float lo_frac,
                                   float hi_frac);

  // Expose the settled labels so the action can reconstruct forward paths.
  const std::vector<sif::BDEdgeLabel>& labels() const {
    return bdedgelabels_;
  }

  // PROTOTYPE proto/v4-p1 (curvagen-valhalla#10) — F01 harvest hygiene.  With this on,
  // ScanBand rejects a candidate whose label chain is not SIMPLE: an undirected edge
  // (or, when the sidecar is supplied, its twin) revisited anywhere in the chain.  That
  // is the ring-reversal class: ride out, turn round on a roundabout/triangle/village
  // loop, ride back — a legal reversal that bounce rejection cannot see and that bakes
  // an out-and-back stub with a bulb at its tip into the FORWARD leg.
  void set_chain_simplicity(bool reject_nonsimple, const RoadTwinIndex* twins) {
    reject_nonsimple_ = reject_nonsimple;
    twin_index_ = twins;
  }
  uint32_t nonsimple_rejected() const {
    return nonsimple_rejected_;
  }

protected:
  // We only need the settled label tree, not per-node expansion callbacks.
  void ExpandingNode(baldr::GraphReader&,
                     baldr::graph_tile_ptr,
                     const baldr::NodeInfo*,
                     const sif::EdgeLabel&,
                     const sif::EdgeLabel*) override {
  }
  ExpansionRecommendation ShouldExpand(baldr::GraphReader& graphreader,
                                       const sif::EdgeLabel& pred,
                                       const ExpansionType route_type) override;
  void GetExpansionHints(uint32_t& bucket_count,
                         uint32_t& edge_label_reservation) const override;

  // proto/v4-p1 F01: fill nonsimple_ for every settled label in one forest pass.
  void ComputeChainSimplicity(baldr::GraphReader& reader);

private:
  float max_meters_ = 0.0f;   // = target/2 * 1.2
  float near_radius_ = 0.0f;  // explore all road levels within this radius; arterials-only beyond
  bool reject_nonsimple_ = false;                  // proto/v4-p1 F01
  const RoadTwinIndex* twin_index_ = nullptr;      // proto/v4-p1 F01 (twin-aware)
  uint32_t nonsimple_rejected_ = 0;                // proto/v4-p1 ledger counter
  std::vector<uint8_t> nonsimple_;                 // proto/v4-p1 per-label chain flag
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_

#ifndef VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_
#define VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_

#include "thor/dijkstras.h"

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

  // Expose the settled labels so the action can reconstruct forward paths.
  const std::vector<sif::BDEdgeLabel>& labels() const {
    return bdedgelabels_;
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

private:
  float max_meters_ = 0.0f;   // = target/2 * 1.2
  float near_radius_ = 0.0f;  // explore all road levels within this radius; arterials-only beyond
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_EXPANSION_H_

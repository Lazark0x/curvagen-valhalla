#ifndef VALHALLA_THOR_ROUNDTRIP_SCORE_H_
#define VALHALLA_THOR_ROUNDTRIP_SCORE_H_

// Round-trip loop measures: curviness, twins-aware self-overlap and the road keys the
// distinctness tests compare, all read off a loop's legs as lists of LoopEdge. The
// measures are pure; leg_edges and resolve_edge are the only functions that read tiles.

#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/thor/pathinfo.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace valhalla {
namespace thor {
namespace roundtrip {

// One ridden edge of a loop leg, as every measure reads it.
struct LoopEdge {
  baldr::GraphId edge;
  baldr::GraphId opp;      // the opposite directed edge (invalid when there is none)
  uint64_t canon = 0;      // the road's canonical id: min(edge, opp), or edge alone
  double seg_m = 0;        // metres this edge adds to its leg
  double from_start_m = 0; // the distance the Start Exemption test reads for this edge
  double curvature = 0;    // DirectedEdge::curvature()
  double length_m = 0;     // DirectedEdge::length()
  bool resolved = false;   // its tile was readable; curvature and length are 0 otherwise
};

// Appends the sidecar twins of a canonical road id. Empty when road identity is off.
using TwinsFn = std::function<void(uint64_t canon, std::vector<uint64_t>& out)>;

// Resolves one edge against the tiles. `tile` is a lookup cache the caller keeps.
LoopEdge resolve_edge(const baldr::GraphId& edge,
                      double seg_m,
                      double from_start_m,
                      baldr::GraphReader& reader,
                      baldr::graph_tile_ptr& tile);

// A built leg as LoopEdges. The forward leg's distance from the start is its path
// distance; the return leg's is what remains of the return after the edge, since the
// return ends at the start.
std::vector<LoopEdge>
leg_edges(const std::vector<PathInfo>& leg, bool is_return, baldr::GraphReader& reader);

// The roads a loop rides beyond the Start Exemption, for the distinctness tests.
struct LoopKeys {
  std::unordered_map<uint64_t, double> ridden; // canonical id -> metres
  std::unordered_set<uint64_t> twins;          // sidecar twins of the ridden roads
  double total = 0;                            // loop metres
  // The forward leg alone (pairs only; empty for a built loop's keys).
  std::unordered_map<uint64_t, double> fwd_ridden;
  double fwd_total = 0;
};

// The share of a's length riding b's roads or their twins.
double shared_fraction(const LoopKeys& a, const LoopKeys& b);
// The share of cand's forward leg riding prev's roads or their twins.
double fwd_shared_fraction(const LoopKeys& cand, const LoopKeys& prev);

// A disjoint pair read as a loop: `tree` ridden out, `other` ridden home backwards. Both
// paths run from the start, so an edge's distance from the start is its distance along
// its own path. Curviness is weighted by the metres each edge adds to its path.
struct PairMeasure {
  LoopKeys keys;
  float curviness = 0;  // curvature-weighted mean over both paths, 0..1
  double overlap_m = 0; // return metres riding the forward roads, their twins or itself
};
PairMeasure measure_pair(const std::vector<LoopEdge>& tree,
                         const std::vector<LoopEdge>& other,
                         const TwinsFn& twins,
                         double exemption_m);

// A built loop. Curviness is weighted by edge length; a return edge overlaps when its road
// or a twin is on the forward leg beyond the exemption, and with return_vs_return also
// when the return already rode it or its twin.
struct BuiltMeasure {
  float curviness = 0;
  double overlap_m = 0;
};
BuiltMeasure measure_built(const std::vector<LoopEdge>& fwd,
                           const std::vector<LoopEdge>& ret,
                           const TwinsFn& twins,
                           bool return_vs_return,
                           double exemption_m);

// A built loop's keys: every edge beyond the exemption, twins keyed by the road id.
// Returns the loop's length.
double built_keys(const std::vector<LoopEdge>& fwd,
                  const std::vector<LoopEdge>& ret,
                  double fwd_total,
                  double ret_total,
                  const TwinsFn& twins,
                  double exemption_m,
                  std::unordered_map<uint64_t, double>& ridden,
                  std::unordered_map<uint64_t, double>& twin_metres);

// |actual - target| / target, 0 without a target.
double distance_error(double actual_m, double target_m);
// The pair selection score: curviness discounted by distance error.
double pair_score(float curviness, double dist_err, double disterr_w);
// The built-loop rank score: curviness / ((1 + Wo * overlap share) * (1 + Wd * dist_err)).
double built_score(float curviness,
                   double overlap_m,
                   double total_m,
                   double dist_err,
                   double overlap_w,
                   double disterr_w);

} // namespace roundtrip
} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_SCORE_H_

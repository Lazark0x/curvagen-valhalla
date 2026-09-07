#ifndef VALHALLA_THOR_ROUNDTRIP_PAIRS_H_
#define VALHALLA_THOR_ROUNDTRIP_PAIRS_H_

// PROTOTYPE — proto/v4-p2 (curvagen-valhalla#11, P2 of the round-trip v4 ladder).
// Throwaway code: no error handling beyond runnable, no abstraction beyond what the
// measurement needs.  Do not merge into a serving branch as-is.
//
// Suurballe & Tarjan, "A Quick Method for Finding Shortest Pairs of Disjoint Paths",
// Networks 14 (1984) 325-336: for a directed graph with a source s and non-negative
// costs, ONE Dijkstra-like pass over a shortest-path tree T yields, for EVERY sink v,
// the minimum-cost pair of edge-disjoint s->v paths (implicitly; O(1) per path edge
// to construct).  Two pieces live here:
//
//   ShortestPairs      the paper's Section II/III algorithm on an abstract tree +
//                      nontree-arc multidigraph with transformed (reduced) costs.
//   RoundTripPairPass  the round-trip graph it runs on — the JUNCTION GRAPH H of the
//                      harvest region: one vertex per physical junction the expansion
//                      settled (hierarchy-level twins folded, and the two sides of a
//                      dual carriageway folded where the sidecar's twin pairs share
//                      their end junctions), one arc per road-and-direction, priced
//                      with the harvest's own potentials.  An arc A->B is usable by
//                      the FORWARD leg iff the expansion settled an edge A->B, and by
//                      the RETURN leg iff a rideable edge B->A exists (the return is
//                      the second path ridden backwards); a road ridden out on one
//                      carriageway and home on the other is one arc, capacity one —
//                      the twin exclusion by construction.  The Start Exemption is a
//                      parallel zero-cost copy of the exempt stretch's arcs.
//
// The formulation, the approximations and what does NOT transfer are written up in
// docs/curvagen/research/2026-09-06-p2-suurballe-whole-loop.md.

#include "baldr/graphreader.h"
#include "sif/edgelabel.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace valhalla {
namespace thor {

class RoundTripExpansion;
class RoadTwinIndex;

// The paper's shortest-pairs algorithm on an abstract graph.  Vertices 0..n-1, a
// rooted tree given by parent[] (kNone at the root), and nontree arcs (u, w, c') with
// c' >= 0 — the REDUCED costs of Section II step 2, so every tree arc costs 0 and is
// implicit.  Multi-arcs are allowed ("arbitrary multidigraphs", §IV): p(v) is an arc.
class ShortestPairs {
public:
  static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();
  static constexpr double kInf = std::numeric_limits<double>::infinity();

  struct Arc {
    uint32_t u, w;
    float c;
  };

  // Step in a constructed path: from -> to, either along a tree arc (arc == kNone) or
  // along nontree arc `arc`.
  struct Step {
    uint32_t from, to, arc;
  };

  // Runs the whole algorithm.  parent.size() == n; root = the one vertex with
  // parent kNone.  Arcs are consumed.
  void Run(std::vector<uint32_t> parent, std::vector<Arc> arcs);

  // d_v(s, v): the transformed cost of the shortest pair to v (kInf = no pair).
  double dist(uint32_t v) const {
    return d_[v];
  }
  bool has_pair(uint32_t v) const {
    return v != root_ && d_[v] < kInf;
  }
  // Explicit construction (§II, end): the two edge-disjoint paths from the root to v,
  // each as a list of steps root -> ... -> v.
  void Construct(uint32_t v, std::vector<Step>& path_a, std::vector<Step>& path_b) const;

  const std::vector<Arc>& arcs() const {
    return arcs_;
  }
  uint32_t root() const {
    return root_;
  }
  // ledger
  uint64_t bytes() const {
    return bytes_;
  }
  uint32_t labeled() const {
    return labeled_count_;
  }
  uint64_t steps() const {
    return steps_;
  }

private:
  uint32_t n_ = 0, root_ = kNone;
  std::vector<uint32_t> parent_;
  std::vector<Arc> arcs_;
  std::vector<uint32_t> pre_, post_;
  std::vector<double> d_;
  std::vector<uint32_t> p_, q_;
  uint64_t bytes_ = 0;
  uint32_t labeled_count_ = 0;
  uint64_t steps_ = 0;

  bool is_desc(uint32_t x, uint32_t anc) const {
    return pre_[anc] <= pre_[x] && post_[x] <= post_[anc];
  }
};

// The round-trip pair pass: builds the junction graph over the harvest forest, runs
// ShortestPairs, and answers per-junction questions.
class RoundTripPairPass {
public:
  static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

  struct Stats {
    uint32_t labels = 0, canonical_labels = 0, junctions = 0, junctions_unified = 0;
    uint32_t vertices = 0, arcs = 0;
    uint32_t arcs_tree = 0, arcs_forward = 0, arcs_return_only = 0, arcs_exempt = 0,
             arcs_clamped = 0, arcs_two_way = 0, arcs_forward_only_dropped = 0;
    double build_ms = 0, run_ms = 0;
    uint64_t bytes = 0;
    uint32_t sinks_with_pair = 0, labeled = 0, source_children = 0, root_return_arcs = 0;
    uint32_t tree_two_way = 0, tree_one_way = 0, tree_not_cheapest = 0;
    uint64_t steps = 0;
  };

  // One arc of H: the road between two junctions in one direction.
  struct HArc {
    uint32_t from = kNone, to = kNone;
    uint32_t fwd_label = kNone; // the settled label riding from -> to (kNone: none)
    uint64_t ret_edge = 0;      // the rideable edge to -> from the return uses (0: none)
    float len = 0;              // metres
    float cost = 0;             // the harvest cost of riding it (reduced later)
    bool tree = false;
  };

  // A constructed pair for one junction: two arc paths root -> junction.  `tree`
  // arrives on the junction's cheapest label (the harvest chain), `other` on the
  // second disjoint arrival.  Either may be ridden as the forward leg (every arc needs
  // fwd_label) with the other ridden home backwards (every arc needs ret_edge).
  struct Pair {
    bool exists = false;
    std::vector<uint32_t> tree, other; // HArc indices
    double reduced_cost = 0;           // d'(J) — the pair's surplus over 2 d(s, J)
    double pair_cost = 0;              // 2 d(s, J) + d'(J) in original cost units
    double tree_len = 0, other_len = 0;
  };

  RoundTripPairPass(const RoundTripExpansion& exp,
                    baldr::GraphReader& reader,
                    const RoadTwinIndex* twins,
                    uint32_t access_mask,
                    double exemption_m,
                    double twin_join_m);

  // Build the graph and run the pass.
  void Run();
  // Offer only return-rideable arcs to the second phase (default on).
  void set_return_legal(bool on);
  // Prefer two-way arrivals as tree arcs (default on) — see Run().
  void set_prefer_two_way_tree(bool on);

  // The (unified) physical junction a label ends at, kNone if the label was an alias
  // (an origin duplicate) or unresolvable.
  uint32_t junction_of(uint32_t label) const {
    return label < junction_of_.size() ? junction_of_[label] : kNone;
  }
  // The physical junction of a node (hierarchy twins and twin carriageways folded),
  // kNone when the expansion never settled an edge into it.
  uint32_t junction_of_node(const baldr::GraphId& node) const;
  // The same physical junction, hierarchy levels folded but carriageways NOT: what a
  // ridden edge sequence needs to be connected.
  bool same_physical_node(const baldr::GraphId& a, const baldr::GraphId& b) const;
  // The tree parent label of a junction: the cheapest arrival = the forward leg.
  uint32_t tree_parent(uint32_t junction) const {
    return jparent_[junction];
  }
  bool has_pair(uint32_t junction) const {
    return sp_.has_pair(junction);
  }
  double reduced_cost(uint32_t junction) const {
    return sp_.dist(junction);
  }
  Pair construct(uint32_t junction) const;
  const HArc& arc(uint32_t i) const {
    return harcs_[i];
  }

  const Stats& stats() const {
    return stats_;
  }
  uint32_t label_count() const {
    return n_labels_;
  }
  double edge_len(uint32_t label) const;

private:
  const RoundTripExpansion& exp_;
  baldr::GraphReader& reader_;
  const RoadTwinIndex* twins_;
  uint32_t access_mask_;
  double exemption_m_;
  double twin_join_m_;
  uint32_t n_labels_ = 0, n_junctions_ = 0;
  std::vector<uint32_t> junction_of_;             // per label (unified)
  std::vector<uint32_t> jparent_;                 // per junction: min-cost arrival label
  std::vector<uint32_t> canonical_of_;            // per label: itself, or the first label of its edge
  std::unordered_map<uint64_t, uint32_t> jindex_; // canonical node value -> unified junction
  std::vector<HArc> harcs_;
  std::vector<uint32_t> tree_arc_;    // per junction: its tree HArc
  std::vector<uint32_t> nontree_arc_; // ShortestPairs arc index -> HArc index
  std::vector<uint32_t> parent_of_;   // per vertex, the tree parent (S at the roots)
  bool return_legal_ = true;
  bool prefer_two_way_tree_ = true;
  ShortestPairs sp_;
  Stats stats_;

  uint32_t source_vertex() const {
    return n_junctions_;
  }
  uint64_t canonical_node(const baldr::GraphId& node, baldr::graph_tile_ptr& tile) const;
  void steps_to_arcs(const std::vector<ShortestPairs::Step>& steps,
                     std::vector<uint32_t>& arcs) const;
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_PAIRS_H_

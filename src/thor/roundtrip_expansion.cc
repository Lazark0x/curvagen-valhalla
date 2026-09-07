#include "thor/roundtrip_expansion.h"
#include "baldr/graphconstants.h"
#include "midgard/pointll.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <cmath>
#include <unordered_map>

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

  // proto/v4-p1 F01: one pass over the whole label forest, before any candidate is
  // considered.  Per-candidate chain walks are O(candidates x chain depth) and cost
  // ~1 s on a 50 km Belgrade request (measured); this is O(labels).
  // proto/v4-p1.1: cached across the primary and widened ScanBand (both use
  // hi_frac 1.18, so `hi` is identical); recomputed only if a wider band ever arrives.
  if (reject_nonsimple_ && (nonsimple_.size() != bdedgelabels_.size() || hi > f01_hi_))
    ComputeChainSimplicity(reader, hi);

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

    // proto/v4-p1 F01: reject non-simple chains (precomputed in one forest pass).
    if (reject_nonsimple_ && nonsimple_[i]) {
      ++nonsimple_rejected_;
      continue;
    }

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


// proto/v4-p1 F01 — chain simplicity over the whole settled forest.
//
// A label's predecessor chain is the forward leg the candidate would ride.  It is
// NON-SIMPLE when an undirected road segment (or, with the sidecar, its twin) appears
// twice: ride out, reverse legally on a roundabout / triangle junction / village loop,
// ride back.  ADR-0037 bounce rejection only sees an IMMEDIATE U-turn, so this class
// walks straight through it, wins the node-dedup and the curviness rank (the retraced
// curvy kilometres count twice), and bakes an out-and-back stub with a bulb at its tip
// into the FORWARD leg where neither the Defect Gate nor the stem check can see it.
//
// Because predecessor(k) < k always (a label is created after its predecessor settles),
// the labels form a forest.  One DFS carrying the canonical ids of the current
// root-to-node path in a small hash map answers "does this edge already appear above
// me" in O(1) per label, and the flag inherits down the chain.
void RoundTripExpansion::ComputeChainSimplicity(baldr::GraphReader& reader, uint32_t hi) {
  const uint32_t n = static_cast<uint32_t>(bdedgelabels_.size());
  nonsimple_.assign(n, 0);
  f01_hi_ = hi;
  f01_labels_ = 0;
  f01_key_ms_ = f01_dfs_ms_ = 0.0;
  if (n == 0)
    return;
  const auto t_key = std::chrono::steady_clock::now();

  // Canonical undirected id per label = min(edge, opposing edge) — the sidecar's key
  // convention since proto/v4-p1.1.
  //
  // P1 read it as `de->forward() ? eid : GetOpposingEdgeId(eid)`, which costs a tile
  // fetch for the edge PLUS a second fetch inside the opposing lookup, on every settled
  // label: +48 ms/request at 300 km, the single biggest line in P1's latency bill (§8).
  // Everything needed is already in the label — `endnode()` and `opp_index()` are
  // stored by BDEdgeLabel's constructor — so the opposing id is
  // `(endnode.tile, endnode.level, node->edge_index() + opp_index)` and the DirectedEdge
  // is never dereferenced.  One tile fetch remains, and it is served from a
  // single-entry cache because settle order clusters by tile.
  //
  // The forward Dijkstra does NOT populate BDEdgeLabel::opp_edgeid() (dijkstras.cc:136
  // "we don't bother ... for the forward expansion"), which is why the id is rebuilt
  // here rather than read off the label.
  // The DFS below walks the forest in traversal order, so every array it touches is a
  // random access.  BDEdgeLabel is ~80 bytes and the label vector is tens of MB at
  // 300 km, so the predecessor link is lifted into a compact uint32 array here (read
  // sequentially, written sequentially) and the label vector is never touched again.
  // Out-of-band labels are marked with kPruned so the child-skip test is one array read.
  constexpr uint64_t kPruned = std::numeric_limits<uint64_t>::max();
  std::vector<uint64_t> canon(n, 0);
  std::vector<uint32_t> pred(n, kInvalidLabel);
  graph_tile_ptr tile;
  for (uint32_t k = 0; k < n; ++k) {
    pred[k] = bdedgelabels_[k].predecessor();
    if (bdedgelabels_[k].path_distance() > hi) {
      canon[k] = kPruned; // never a candidate, never an ancestor of one
      continue;
    }
    const GraphId en = bdedgelabels_[k].endnode();
    if (!reader.GetGraphTile(en, tile))
      continue;
    const GraphId opp(en.tileid(), en.level(),
                      tile->node(en)->edge_index() + bdedgelabels_[k].opp_index());
    canon[k] = std::min(bdedgelabels_[k].edgeid().value, opp.value);
    ++f01_labels_;
  }
  f01_key_ms_ =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_key).count();
  const auto t_dfs = std::chrono::steady_clock::now();

  // children lists (reverse order so a child list comes out ascending)
  constexpr uint32_t kEnter = 0xfffffffeu;
  std::vector<uint32_t> child_head(n, kInvalidLabel), child_next(n, kInvalidLabel);
  std::vector<uint32_t> roots;
  for (uint32_t k = n; k-- > 0;) {
    const uint32_t p = pred[k];
    if (p == kInvalidLabel) {
      roots.push_back(k);
    } else {
      child_next[k] = child_head[p];
      child_head[p] = k;
    }
  }

  std::unordered_map<uint64_t, uint32_t> onpath;
  onpath.reserve(2048); // a root-to-node path is hundreds of edges, never thousands
  std::vector<std::pair<uint32_t, uint32_t>> stack; // (label, child cursor | kEnter)
  std::vector<uint8_t> inserted;                    // did this frame touch `onpath`?
  std::vector<uint64_t> tw;
  for (uint32_t r : roots) {
    if (canon[r] == kPruned)
      continue;
    stack.emplace_back(r, kEnter);
    inserted.push_back(0);
    while (!stack.empty()) {
      const size_t top = stack.size() - 1;
      const uint32_t k = stack[top].first;
      if (stack[top].second == kEnter) {
        const uint32_t p = pred[k];
        // proto/v4-p1.1: non-simplicity INHERITS, so a bad node's whole subtree is bad
        // whatever else is on the path.  ~23 % of settled labels sit in one (P1 measured
        // 41 k rejects per request), and skipping their `onpath` bookkeeping is free
        // correctness: nothing below them can change their verdict.
        const uint64_t c = canon[k];
        if (p != kInvalidLabel && nonsimple_[p]) {
          nonsimple_[k] = 1;
          inserted[top] = 0;
        } else {
          bool bad = false;
          if (c) {
            // one hash lookup for the membership test AND the insert.
            auto it = onpath.emplace(c, 0u).first;
            bad = it->second > 0; // present with a live count = already on this path
            if (!bad && twin_index_ && twin_index_->maybe_has_twins(c)) {
              // The Bloom prefilter answers "no twins" — the ~85 % case — without the
              // CSR binary search over 113 k keys.
              tw.clear();
              twin_index_->append_twins(c, tw);
              for (uint64_t t : tw) {
                auto jt = onpath.find(t);
                if (jt != onpath.end()) {
                  bad = true;
                  break;
                }
              }
            }
            ++it->second;
          }
          nonsimple_[k] = bad ? 1 : 0;
          inserted[top] = c ? 1 : 0;
        }
        stack[top].second = child_head[k];
      } else if (stack[top].second != kInvalidLabel) {
        const uint32_t ch = stack[top].second;
        stack[top].second = child_next[ch];
        if (canon[ch] == kPruned)
          continue; // proto/v4-p1.1: out-of-band subtree, pruned whole
        stack.emplace_back(ch, kEnter);
        inserted.push_back(0);
      } else {
        if (inserted[top]) {
          // erase at zero: `onpath` must hold the CURRENT path only (hundreds of
          // entries), never every canonical id the DFS has ever seen.
          auto it = onpath.find(canon[k]);
          if (it != onpath.end() && --it->second == 0)
            onpath.erase(it);
        }
        stack.pop_back();
        inserted.pop_back();
      }
    }
  }
  f01_dfs_ms_ =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_dfs).count();
}

} // namespace thor
} // namespace valhalla

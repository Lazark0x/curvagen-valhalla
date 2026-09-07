#ifndef VALHALLA_THOR_ROAD_TWIN_INDEX_H_
#define VALHALLA_THOR_ROAD_TWIN_INDEX_H_

// PROTOTYPE — proto/v4-p1 (curvagen-valhalla#10, P1 of the round-trip v4 ladder).
// Throwaway code: no error handling beyond runnable, no abstraction beyond what the
// measurement needs.  Do not merge into a serving branch as-is.
//
// The census (docs/curvagen/research/2026-09-06-defect-atlas-v2.md §3) named one
// mechanism as the whole rider-visible residual: F02, "the return rides the other
// carriageway".  v3 keys exclusion, leash, rejoin and xcand on DIRECTED EDGE IDS, and
// the opposite carriageway of a divided road is a different OSM way with different
// nodes — so nothing in the engine knows the two are the same physical road.
//
// This sidecar is the missing notion of road identity, computed ONCE per process from
// the loaded tiles:
//   * TWIN     — another road segment whose shape runs within `twin_radius_m` (30 m)
//                of this one over most of the shorter of the two, and whose chord is
//                collinear (angle within tolerance of 0 deg or 180 deg).  That is the
//                opposite carriageway / one-way pair.
//   * PARALLEL — the same test at `parallel_radius_m` (80 m) but not at the twin
//                radius: the street one block over.
//
// Orientation note.  The ticket phrases the twin test as "bearing difference ~180 deg".
// Physically that is right — the return rides the twin ANTI-parallel to the forward
// leg — but OSM's storage direction for a carriageway is arbitrary, so at index time
// the same physical pair can present as 0 deg or 180 deg.  The index therefore
// qualifies on COLLINEARITY and stores segment identity; the call sites exclude both
// directions of a twin (exactly as route_leg already does for the corridor edge and
// its opposing edge), which is what makes the anti-parallel ride impossible.
//
// Keys and values are CANONICAL directed-edge ids: the directed edge with
// DirectedEdge::forward() == true, i.e. the one whose travel direction matches the
// stored EdgeInfo shape.  Every undirected road segment has exactly one.  Call sites
// canonicalise a query id with canonical_id() below (they already hold the tile and
// the opposing id, so it is free).

#include "baldr/graphreader.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace valhalla {
namespace thor {

class RoadTwinIndex {
public:
  struct Stats {
    uint64_t tiles = 0;
    uint64_t edges_indexed = 0; // canonical (forward) directed edges sampled
    uint64_t samples = 0;
    uint64_t pair_tests = 0;
    uint64_t twin_pairs = 0;
    uint64_t parallel_pairs = 0;
    // proto/v4-p1.1 switchback test (curvagen-valhalla#12 item 1)
    uint64_t geom_twin_pairs = 0;      // pairs the P1 geometry alone called twins
    uint64_t switchback_dropped = 0;   // ...of which the switchback test rejected
    uint64_t sb_same_way = 0;          // dropped: same OSM way id
    uint64_t sb_divergent = 0;         // dropped: offset not constant along the run
    uint64_t geom_par_pairs = 0;
    uint64_t switchback_dropped_par = 0;
    uint64_t twin_keys = 0;
    uint64_t parallel_keys = 0;
    double build_ms = 0.0;
    uint64_t retained_bytes = 0;
    uint64_t peak_bytes = 0;
    double twin_radius_m = 0.0;
    double parallel_radius_m = 0.0;
  };

  // Process-wide, built once per (tile location, radii) triple.  Keyed rather than a
  // plain singleton because gurka builds many tilesets in one process.  Thread-safe.
  static const RoadTwinIndex& get(baldr::GraphReader& reader,
                                  double twin_radius_m,
                                  double parallel_radius_m,
                                  bool build_parallels,
                                  bool switchback_test);

  // The canonical id of a directed edge = min(edge, opposing edge).
  //
  // proto/v4-p1.1: this WAS `de->forward() ? eid : opp`.  min() is the same
  // undirected identity but needs no DirectedEdge dereference, which is what lets the
  // F01 forest pass (roundtrip_expansion.cc) key every settled label without a tile
  // lookup — see that file's canonical_from_label().  `de` is kept in the signature
  // (unused) so the call sites read the same.
  static uint64_t canonical_id(const baldr::DirectedEdge*,
                               const baldr::GraphId& eid,
                               const baldr::GraphId& opp) {
    return opp.is_valid() ? std::min(eid.value, opp.value) : eid.value;
  }
  static uint64_t canonical_id(uint64_t eid, uint64_t opp) {
    return std::min(eid, opp);
  }

  void append_twins(uint64_t canonical, std::vector<uint64_t>& out) const {
    append(twin_keys_, twin_off_, twin_vals_, canonical, out);
  }
  void append_parallels(uint64_t canonical, std::vector<uint64_t>& out) const {
    append(par_keys_, par_off_, par_vals_, canonical, out);
  }
  bool has_twins(uint64_t canonical) const {
    return lookup(twin_keys_, canonical) != twin_keys_.size();
  }

  // proto/v4-p1.1: one-probe Bloom prefilter over the twin key set.  A clear bit is a
  // PROOF of "no twins"; a set bit falls through to the CSR binary search.  The F01
  // forest pass asks this question once per settled label (hundreds of thousands per
  // request) and >98 % of the answers are "no" — a 512 KiB bit array turns those into
  // one cache probe instead of a ~17-step lower_bound over 115 k keys.
  bool maybe_has_twins(uint64_t canonical) const {
    if (twin_filter_.empty())
      return true;
    const uint64_t h = mix(canonical) & kFilterMask;
    return (twin_filter_[h >> 6] >> (h & 63)) & 1ull;
  }

  const Stats& stats() const {
    return stats_;
  }

private:
  RoadTwinIndex() = default;
  void Build(baldr::GraphReader& reader,
             double twin_radius_m,
             double parallel_radius_m,
             bool build_parallels,
             bool switchback_test);

  static size_t lookup(const std::vector<uint64_t>& keys, uint64_t k) {
    auto it = std::lower_bound(keys.begin(), keys.end(), k);
    return (it == keys.end() || *it != k) ? keys.size()
                                          : static_cast<size_t>(it - keys.begin());
  }
  static void append(const std::vector<uint64_t>& keys,
                     const std::vector<uint32_t>& off,
                     const std::vector<uint64_t>& vals,
                     uint64_t k,
                     std::vector<uint64_t>& out) {
    const size_t i = lookup(keys, k);
    if (i == keys.size())
      return;
    out.insert(out.end(), vals.begin() + off[i], vals.begin() + off[i + 1]);
  }

  static constexpr uint64_t kFilterBits = 1ull << 22; // 4 Mi bits = 512 KiB
  static constexpr uint64_t kFilterMask = kFilterBits - 1;
  static uint64_t mix(uint64_t x) { // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  std::vector<uint64_t> twin_filter_;

  // CSR: sorted keys, [off[i], off[i+1]) into vals.
  std::vector<uint64_t> twin_keys_, twin_vals_;
  std::vector<uint32_t> twin_off_;
  std::vector<uint64_t> par_keys_, par_vals_;
  std::vector<uint32_t> par_off_;
  Stats stats_;
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROAD_TWIN_INDEX_H_

#ifndef VALHALLA_THOR_ROUNDTRIP_GATES_H_
#define VALHALLA_THOR_ROUNDTRIP_GATES_H_

// The round-trip Defect Gate and dirty stash; pure except append_leg, which reads tiles.

#include <valhalla/baldr/graphreader.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/thor/pathinfo.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace valhalla {
namespace thor {
namespace roundtrip {

// A leg shape on the 1e-5 grid (lat, lng), repeats dropped, with along-trace metres.
struct GridTrace {
  std::vector<std::pair<int64_t, int64_t>> pts;
  std::vector<double> cum{0.0};

  void add(const midgard::PointLL& p);
};

// false on an unresolvable tile, which every caller reads as a clean check.
bool append_leg(const std::vector<PathInfo>& leg,
                size_t from,
                size_t to,
                baldr::GraphReader& reader,
                GridTrace& trace);

constexpr size_t kAnywhere = std::numeric_limits<size_t>::max();

// Metres of the longest palindrome of grid points; with seam_idx, only one covering it.
double longest_mirror_stub_m(const GridTrace& trace, size_t seam_idx = kAnywhere);

// fwd[fwd_begin, end) and ret[0, ret_end): ~window_m each side of the seam; <= 0 keeps all.
struct SeamWindow {
  size_t fwd_begin;
  size_t ret_end;
};
SeamWindow
seam_window(const std::vector<PathInfo>& fwd, const std::vector<PathInfo>& ret, double window_m);

// The longest exact-mirror stub covering the seam between the two legs.
double seam_stub_m(const std::vector<PathInfo>& fwd,
                   const std::vector<PathInfo>& ret,
                   baldr::GraphReader& reader,
                   double window_m);

// The longest exact-mirror stub anywhere in one leg.
double leg_mirror_stub_m(const std::vector<PathInfo>& leg, baldr::GraphReader& reader);

// seam: a mirror across the seam; twin and bounce: the geometry gate's two halves.
struct GateVerdict {
  bool seam = false;
  bool twin = false;
  bool bounce = false;

  bool rejected() const {
    return seam || twin || bounce;
  }
};

// A seam reject always refills; a geometry reject refills within the budget, then is kept gated.
enum class GateAction { kServe, kStashSeam, kStashGeometry, kKeepGated };

GateAction gate_action(const GateVerdict& verdict, uint32_t refills_used, uint32_t refill_budget);

// A dirty-stash entry as the stash's admission rule reads it.
struct StashRank {
  bool seam = false;
  double score = 0;
};

// The top-up takes geometry rejects before seam rejects, then the higher score.
bool stash_before(const StashRank& a, const StashRank& b);

constexpr size_t kNoSlot = std::numeric_limits<size_t>::max();

// The index to write a reject to (stash.size() appends), or kNoSlot: no two entries duplicate.
size_t stash_slot(const StashRank& loop,
                  const std::vector<StashRank>& stash,
                  size_t cap,
                  const std::vector<size_t>& dups);

// kHeldLoop: identical to, or a near-copy of, a loop the bank holds.
enum class RescueBuild { kNoLoop, kHeldLoop, kNewLoop };

constexpr uint32_t kRescueHeldRun = 5;

// The rescue stops after kRescueHeldRun held loops in a row; a build with no loop is not counted.
class RescueStop {
public:
  void note(RescueBuild build);
  bool reached() const {
    return run_ >= kRescueHeldRun;
  }

private:
  uint32_t run_ = 0;
};

} // namespace roundtrip
} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_GATES_H_

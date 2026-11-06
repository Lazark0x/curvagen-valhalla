#ifndef VALHALLA_THOR_ROUNDTRIP_GATES_H_
#define VALHALLA_THOR_ROUNDTRIP_GATES_H_

// The round-trip Defect Gate: a built loop's legs decoded onto the 1e-5 grid, the
// exact-mirror stubs read off that trace, and the verdict the build loop acts on.
// Everything here is pure except append_leg, the one function that reads tiles.

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

// A leg shape on the 1e-5 grid (lat, lng), consecutive duplicates dropped, with the
// running along-trace distance at every point.
struct GridTrace {
  std::vector<std::pair<int64_t, int64_t>> pts;
  std::vector<double> cum{0.0};

  void add(const midgard::PointLL& p);
};

// Appends the shapes of leg[from, to) to the trace. false = an unresolvable tile, which
// every caller reads as an inconclusive, clean check.
bool append_leg(const std::vector<PathInfo>& leg,
                size_t from,
                size_t to,
                baldr::GraphReader& reader,
                GridTrace& trace);

constexpr size_t kAnywhere = std::numeric_limits<size_t>::max();

// The longest exact-mirror stub (a palindrome of grid points: ridden out and straight
// back) in metres. With a seam index, only stubs whose interval covers it (slack 2).
double longest_mirror_stub_m(const GridTrace& trace, size_t seam_idx = kAnywhere);

// The leg ranges a seam window keeps: fwd[fwd_begin, end) and ret[0, ret_end), each
// ~window_m of riding next to the seam. window_m <= 0 keeps both legs whole.
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

// Which shapes a built loop carries. seam = an exact mirror across the seam (always
// read); twin = the return rides the forward road or its twin; bounce = a mirror inside
// the return. twin and bounce are the geometry gate's two halves.
struct GateVerdict {
  bool seam = false;
  bool twin = false;
  bool bounce = false;

  bool rejected() const {
    return seam || twin || bounce;
  }
};

// What the build loop does with a judged loop. A seam reject always buys a replacement
// build; a geometry reject does while the request's refill budget lasts, and past it is
// kept in the bank's last tier.
enum class GateAction { kServe, kStashSeam, kStashGeometry, kKeepGated };

GateAction gate_action(const GateVerdict& verdict, uint32_t refills_used, uint32_t refill_budget);

} // namespace roundtrip
} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_ROUNDTRIP_GATES_H_

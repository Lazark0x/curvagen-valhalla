#!/usr/bin/env python3
"""loopqual metric definitions — METRICS SPEC v1 (curvagen wayfinder #48).

This module IS the versioned metric spec: every formula and parameter that
affects a reported number lives here. Any change to a formula, a parameter,
or input handling MUST bump METRICS_VERSION — reports are only comparable
within one version.

Detector lineage: spike / lollipop / geometric-reuse detectors are the
hand-validated ticket #47 defect-atlas detectors, ported unchanged
(curvagen-valhalla/tools/defect-atlas/atlas.py; validation cases in
docs/research/2026-07-13-round-trip-defect-atlas.md §3). New in the harness:
compactness (isoperimetric quotient), distance_error, curviness_retention,
and serving-mode (app DTO) input.

Inputs:
- engine mode:  raw fork /route round-trip response — 2 legs per route,
  polyline6 shape (1e-6). Seam (turnaround) = exact leg boundary.
- serving mode: orchestrator /round-trip app DTO — one combined 3D polyline
  (1e5 coords + centimeter elevation) per route, no legs. Seam is DERIVED
  as the point farthest from the start (good approximation of the
  turnaround; seam-relative fields are approximate in this mode).

Both inputs are snapped to the same 1e-5 (~1.1 m) integer grid before any
metric is computed, which makes geometry-only metrics comparable across
modes (the serving polyline is exactly on that grid already).
"""

import math

# v1.1: + curviness_geom_clean (Gate v1, wayfinder #49/#50)
# v1.2: Start-Exemption-aware stem + reuse meters (ADR-0037 §3, curvagen #55).
#   The first stretch of a ride is often network-forced (dead-end starts, single
#   access roads) and the engine deliberately allows the loop to close on it
#   (clean-first, dirty-last-resort). The old meters billed that designed behavior
#   as defect: a ~1.5 km forced stem in a 20 km loop read 0.15 reuse and ~100%
#   lollipop BY CONSTRUCTION. v1.2: the stem meter counts only stem beyond the
#   exemption; the geometric reuse meter discounts reuse inside it, the discount
#   zone capped at the constant per loop end (no blank check). edge_reuse_way
#   keeps v1 semantics (informative cross-check, not a gate input).
METRICS_VERSION = "v1.3"

PARAMS = {
    # ADR-0037 §3 Start Exemption: path-distance radius around the start inside
    # which forward-edge reuse is designed behavior. PINNED to the fork's
    # kStartExemptionMeters (src/thor/route_action.cc, curvagen #56 T2) — change
    # them together, never one alone.
    "start_exemption_m": 1500.0,
    # point grid all geometry is snapped to (degrees; ~1.1 m)
    "grid_deg": 1e-5,
    # spike: minimum one-way stub length to count (excludes snap jitter)
    "spike_min_stub_m": 30.0,
    # spike: "big stub" threshold for the per-loop boolean
    "spike_big_stub_m": 500.0,
    # spike: index slack when classifying an interval as covering the seam
    "seam_class_slack_idx": 2,
    # lollipop: cross-leg corridor radius
    "lollipop_corridor_radius_m": 40.0,
    # lollipop: unshared gap tolerated inside a stem
    "lollipop_gap_m": 120.0,
    # lollipop: minimum stem length to count at all
    "lollipop_min_stem_m": 150.0,
    # lollipop: per-loop boolean threshold on lollipop_stem_fraction
    "lollipop_stem_frac_threshold": 0.10,
    # lollipop: minimum unshared leg0 run to count as a bulb
    "bulb_min_m": 500.0,
    "earth_radius_m": 6371000.0,
}

EARTH_R = PARAMS["earth_radius_m"]

# --- geometry ------------------------------------------------------------


def haversine_m(a, b):
    (la1, lo1), (la2, lo2) = a, b
    p1, p2 = math.radians(la1), math.radians(la2)
    dp = p2 - p1
    dl = math.radians(lo2 - lo1)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(math.sqrt(h))


def decode_polyline(shape: str, precision: float = 1e-6):
    """Valhalla polyline (default polyline6) -> [(lat, lon)] floats."""
    pts, i, lat, lon = [], 0, 0, 0
    n = len(shape)
    while i < n:
        for coord in ("lat", "lon"):
            shift, result = 0, 0
            while True:
                b = ord(shape[i]) - 63
                i += 1
                result |= (b & 0x1F) << shift
                shift += 5
                if b < 0x20:
                    break
            delta = ~(result >> 1) if result & 1 else result >> 1
            if coord == "lat":
                lat += delta
            else:
                lon += delta
        pts.append((lat * precision, lon * precision))
    return pts


def decode_polyline_3d(shape: str, precision: float = 1e-5):
    """App-DTO 3D polyline (lat/lon at 1e5, elevation in cm) -> [(lat, lon)].

    Elevation is decoded (to keep the varint stream aligned) and dropped —
    v1 metrics are planimetric.
    """
    pts, i = [], 0
    lat = lon = ele = 0
    n = len(shape)
    while i < n:
        vals = []
        for _ in range(3):
            shift, result = 0, 0
            while True:
                b = ord(shape[i]) - 63
                i += 1
                result |= (b & 0x1F) << shift
                shift += 5
                if b < 0x20:
                    break
            vals.append(~(result >> 1) if result & 1 else result >> 1)
        lat += vals[0]
        lon += vals[1]
        ele += vals[2]
        pts.append((lat * precision, lon * precision))
    return pts


def encode_polyline(pts, precision: float = 1e-6):
    """[(lat, lon)] -> Valhalla polyline (for /trace_attributes)."""
    inv = round(1.0 / precision)
    out = []
    plat = plon = 0
    for lat, lon in pts:
        ilat, ilon = round(lat * inv), round(lon * inv)
        for v in (ilat - plat, ilon - plon):
            v = ~(v << 1) if v < 0 else v << 1
            while v >= 0x20:
                out.append(chr((0x20 | (v & 0x1F)) + 63))
                v >>= 5
            out.append(chr(v + 63))
        plat, plon = ilat, ilon
    return "".join(out)


def key5(p):
    """~1.1 m integer grid key (1e-5 deg) — merges same-edge geometry across traversals."""
    return (round(p[0] * 1e5), round(p[1] * 1e5))


def dedup_consecutive(pts):
    """Round to the 1e-5 grid and drop zero-length segments."""
    out = []
    for p in pts:
        k = key5(p)
        if not out or out[-1] != k:
            out.append(k)
    return out  # integer grid points


def grid_to_ll(k):
    return (k[0] / 1e5, k[1] / 1e5)


def seg_len_m(a, b):
    return haversine_m(grid_to_ll(a), grid_to_ll(b))


# --- loop assembly --------------------------------------------------------


class Loop:
    """One round-trip candidate on the 1e-5 grid.

    pts        grid points of the whole loop
    seam_index index of the turnaround in pts (exact in engine mode, derived
               farthest-from-start in serving mode)
    leg0/leg1  start->turnaround / turnaround->start grid points
    raw_pts    un-snapped decoded points (for /trace_attributes map-snap)
    declared_m the distance the response declares (engine summary.length,
               serving paths[0].distance)
    """

    def __init__(self, meta, slot, mode, seam_source, leg0, leg1, raw_pts,
                 declared_m, curviness_used=None):
        self.meta = meta
        self.slot = slot  # response index == engine curviness rank (engine stable-sorts)
        self.mode = mode
        self.seam_source = seam_source
        self.leg0 = leg0
        self.leg1 = leg1
        self.raw_pts = raw_pts
        self.declared_m = declared_m
        self.curviness_used = curviness_used
        joined = leg1[1:] if (leg1 and leg0 and leg1[0] == leg0[-1]) else leg1
        self.pts = leg0 + joined
        self.seam_index = len(leg0) - 1
        self.seg_lens = [seg_len_m(self.pts[i], self.pts[i + 1]) for i in range(len(self.pts) - 1)]
        self.total_m = sum(self.seg_lens)
        self.cum = [0.0]
        for l in self.seg_lens:
            self.cum.append(self.cum[-1] + l)
        self.leg0_m = self.cum[self.seam_index] if self.seam_index < len(self.cum) else 0.0

    @classmethod
    def from_engine_trip(cls, trip, meta, slot):
        """Fork /route round-trip trip: legs[0] out, legs[1] back, polyline6."""
        legs = trip["legs"]
        raw_leg0 = decode_polyline(legs[0]["shape"])
        raw_leg1 = decode_polyline(legs[1]["shape"])
        raw_pts = raw_leg0 + raw_leg1[1:]
        return cls(
            meta, slot, "engine", "leg_boundary",
            dedup_consecutive(raw_leg0), dedup_consecutive(raw_leg1), raw_pts,
            declared_m=trip["summary"]["length"] * 1000.0,
        )

    @classmethod
    def from_serving_route(cls, route, meta, slot):
        """Orchestrator app DTO route: paths[0].points 3D polyline, no legs.

        The seam is derived as the grid point farthest (great-circle) from
        the start — seam-relative outputs are approximate in this mode.
        """
        path = route["paths"][0]
        raw_pts = decode_polyline_3d(path["points"])
        pts = dedup_consecutive(raw_pts)
        if len(pts) < 3:
            seam = max(0, len(pts) - 1)
        else:
            start = grid_to_ll(pts[0])
            seam = max(range(len(pts)), key=lambda i: haversine_m(start, grid_to_ll(pts[i])))
        return cls(
            meta, slot, "serving", "farthest_point",
            pts[: seam + 1], pts[seam:], raw_pts,
            declared_m=float(path["distance"]),
            curviness_used=route.get("curvinessUsed"),
        )

    @property
    def seam_frac(self):
        return self.leg0_m / self.total_m if self.total_m else 0.0


# --- detectors ------------------------------------------------------------


def find_spikes(loop: Loop, min_stub_m: float = PARAMS["spike_min_stub_m"]):
    """spike_count / max_stub_km / spike_len_fraction primitive.

    Exact-mirror retrace: a reversal apex p[i-1] == p[i+1] extended while
    p[i-w] == p[i+w]; overlapping palindrome intervals merged (longest stub
    kept as the apex); reported when the one-way stub >= min_stub_m.
    Classes (±seam_class_slack_idx on interval bounds):
      seam_uturn   apex exactly at the seam (return opens by retracing)
      seam_wrapped interval covers the seam (dead-end bounce harvested
                   into the forward leg, turnaround mid-stub)
      mid_fwd / mid_ret  strictly inside one leg (atlas measured 0 of these
                   in 2 602 engine loops)
    Caveat: dual-carriageway U-turns have direction-distinct geometry and
    are NOT exact mirrors — they surface in the corridor detector instead,
    so spike counts are a slight undercount.
    """
    pts = loop.pts
    n = len(pts)
    hits = []
    for i in range(1, n - 1):
        if pts[i - 1] == pts[i + 1]:
            w = 1
            while i - 1 - w >= 0 and i + 1 + w < n and pts[i - 1 - w] == pts[i + 1 + w]:
                w += 1
            stub = sum(loop.seg_lens[j] for j in range(i - w, i))
            hits.append({"apex": i, "w": w, "stub_m": stub, "lo": i - w, "hi": i + w})
    # merge overlapping retrace intervals, keep the longest stub's apex
    hits.sort(key=lambda h: h["lo"])
    merged = []
    for h in hits:
        if merged and h["lo"] <= merged[-1]["hi"]:
            if h["stub_m"] > merged[-1]["stub_m"]:
                merged[-1]["apex"], merged[-1]["w"], merged[-1]["stub_m"] = h["apex"], h["w"], h["stub_m"]
            merged[-1]["hi"] = max(merged[-1]["hi"], h["hi"])
            merged[-1]["lo"] = min(merged[-1]["lo"], h["lo"])
        else:
            merged.append(dict(h))
    out = []
    seam = loop.seam_index
    slack = PARAMS["seam_class_slack_idx"]
    for h in merged:
        if h["stub_m"] < min_stub_m:
            continue
        frac = loop.cum[h["apex"]] / loop.total_m if loop.total_m else 0.0
        covers = (h["lo"] - slack) <= seam <= (h["hi"] + slack)
        at_seam = h["apex"] == seam
        if at_seam:
            cls = "seam_uturn"
        elif covers:
            cls = "seam_wrapped"
        elif h["hi"] < seam:
            cls = "mid_fwd"
        else:
            cls = "mid_ret"
        out.append({
            "apex": h["apex"],
            "lo": h["lo"],
            "hi": h["hi"],
            "stub_m": round(h["stub_m"], 1),
            "frac": round(frac, 4),
            "class": cls,
            "seam_dist_frac": round(abs(frac - loop.seam_frac), 4),
            "apex_ll": grid_to_ll(loop.pts[h["apex"]]),
        })
    return out


def seam_uturn(loop: Loop) -> bool:
    """Does the return leg open by retracing the forward leg's last segment?"""
    i = loop.seam_index
    return 0 < i < len(loop.pts) - 1 and loop.pts[i - 1] == loop.pts[i + 1]


def edge_reuse_geom(loop: Loop,
                    exemption_m: float = PARAMS["start_exemption_m"]) -> float:
    """edge_reuse_geom: undirected segment reuse — fraction of total loop
    length spent on segments whose undirected 1e-5 grid key appears more
    than once (both traversals count; a pure out-and-back = 1.0). Geometric
    analog of the fork's both-direction leash marking and of the retired
    eval_routes.py edge_reuse.

    v1.2 (ADR-0037 §3): reused segments INSIDE the Start Exemption are
    designed behavior and do not count. A segment is inside when its
    midpoint lies within exemption_m of ride start (first traversal of the
    forced stem) or within exemption_m of ride end (the traversal home) —
    the discount zone is capped at the constant per loop end, so reuse
    deeper in the loop is never forgiven (no blank check).
    """
    from collections import Counter
    keys = []
    for i in range(len(loop.pts) - 1):
        a, b = loop.pts[i], loop.pts[i + 1]
        keys.append((a, b) if a <= b else (b, a))
    c = Counter(keys)
    if not loop.total_m:
        return 0.0
    reused = 0.0
    for i, (k, l) in enumerate(zip(keys, loop.seg_lens)):
        if c[k] <= 1:
            continue
        mid = (loop.cum[i] + loop.cum[i + 1]) / 2.0
        if mid < exemption_m or mid > loop.total_m - exemption_m:
            continue  # inside the Start Exemption zone — designed reuse
        reused += l
    return reused / loop.total_m


# corridor sharing (lollipop stem / shadowing)

CELL_LAT = PARAMS["lollipop_corridor_radius_m"] / 111320.0  # grid cell ≈ corridor radius


def _grid(points):
    import collections
    g = collections.defaultdict(list)
    r = PARAMS["lollipop_corridor_radius_m"]
    for k in points:
        lat, lon = grid_to_ll(k)
        cl = r / (111320.0 * math.cos(math.radians(lat)))
        g[(int(lat / CELL_LAT), int(lon / cl))].append(k)
    return g


def _shared_mask(leg_a, leg_b, radius_m):
    """For each point of leg_a: is any point of leg_b within radius?"""
    g = _grid(leg_b)
    r = PARAMS["lollipop_corridor_radius_m"]
    mask = []
    for k in leg_a:
        lat, lon = grid_to_ll(k)
        cl = r / (111320.0 * math.cos(math.radians(lat)))
        ci, cj = int(lat / CELL_LAT), int(lon / cl)
        found = False
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                for other in g.get((ci + di, cj + dj), ()):
                    if seg_len_m(k, other) <= radius_m:
                        found = True
                        break
                if found:
                    break
            if found:
                break
        mask.append(found)
    return mask


def corridor_stats(loop: Loop,
                   radius_m=PARAMS["lollipop_corridor_radius_m"],
                   gap_m=PARAMS["lollipop_gap_m"],
                   min_stem_m=PARAMS["lollipop_min_stem_m"]):
    """lollipop_stem_fraction / bulb_count primitive.

    stem_out_m: length of the maximal leg0 PREFIX within radius of leg1
                (unshared gaps <= gap_m tolerated), MINUS the Start
                Exemption (v1.2 — only stem beyond the exemption counts;
                stem_back_m likewise). rejoin_return_frac follows the
                discounted stem_back.
    stem_back_m: same for the leg1 SUFFIX vs leg0.
    stem_frac: (stem_out + stem_back) / total loop length.
    shadow_frac: fraction of leg0 length within radius of leg1 ANYWHERE
                 (stem + mid-loop + seam retrace all count; upper bound of
                 cross-leg corridor sharing).
    bulbs: maximal unshared leg0 runs >= bulb_min_m (classic lollipop = 1).
    """
    if len(loop.leg0) < 2 or len(loop.leg1) < 2:
        return {"stem_out_m": 0, "stem_back_m": 0, "stem_frac": 0, "shadow_frac": 0, "bulbs": 0}
    m0 = _shared_mask(loop.leg0, loop.leg1, radius_m)
    seglen0 = [seg_len_m(loop.leg0[i], loop.leg0[i + 1]) for i in range(len(loop.leg0) - 1)]
    leg0_m = sum(seglen0) or 1.0

    def prefix_len(mask, seglens):
        # walk while shared, tolerating unshared gaps <= gap_m
        length = 0.0
        last_shared_len = 0.0
        gap = 0.0
        for i in range(len(seglens)):
            l = seglens[i]
            if mask[i + 1]:
                length += gap + l
                gap = 0.0
                last_shared_len = length
            else:
                gap += l
                if gap > gap_m:
                    break
        return last_shared_len

    stem_out = prefix_len(m0, seglen0) if m0[0] or (len(m0) > 1 and m0[1]) else 0.0
    m1 = _shared_mask(loop.leg1, loop.leg0, radius_m)
    seglen1 = [seg_len_m(loop.leg1[i], loop.leg1[i + 1]) for i in range(len(loop.leg1) - 1)]
    stem_back = (
        prefix_len(list(reversed(m1)), list(reversed(seglen1)))
        if m1[-1] or (len(m1) > 1 and m1[-2])
        else 0.0
    )
    # v1.2 (ADR-0037 §3): the stem meter counts only stem BEYOND the Start
    # Exemption — the first exemption_m of each stem is the network-forced,
    # engine-designed stretch. Discount before the min-stem zeroing so a
    # just-past-exemption dribble does not read as a lollipop stem.
    exemption_m = PARAMS["start_exemption_m"]
    stem_out = max(0.0, stem_out - exemption_m)
    stem_back = max(0.0, stem_back - exemption_m)
    if stem_out < min_stem_m:
        stem_out = 0.0
    if stem_back < min_stem_m:
        stem_back = 0.0

    shadow = sum(l for i, l in enumerate(seglen0) if m0[i] or m0[i + 1])
    # bulbs: unshared runs of leg0 >= bulb_min_m
    bulb_min = PARAMS["bulb_min_m"]
    bulbs, run = 0, 0.0
    for i, l in enumerate(seglen0):
        if not (m0[i] or m0[i + 1]):
            run += l
        else:
            if run >= bulb_min:
                bulbs += 1
            run = 0.0
    if run >= bulb_min:
        bulbs += 1
    return {
        "stem_out_m": round(stem_out, 1),
        "stem_back_m": round(stem_back, 1),
        "stem_frac": round((stem_out + stem_back) / loop.total_m, 4) if loop.total_m else 0.0,
        "shadow_frac": round(shadow / leg0_m, 4),
        "bulbs": bulbs,
    }


def bearing_deg(a, b):
    """Initial great-circle bearing a->b in degrees [0, 360)."""
    la1, lo1 = math.radians(a[0]), math.radians(a[1])
    la2, lo2 = math.radians(b[0]), math.radians(b[1])
    dl = lo2 - lo1
    y = math.sin(dl) * math.cos(la2)
    x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def curviness_geom_clean(loop: Loop, spikes) -> float:
    """curviness_geom_clean (NEW in v1.1, Gate v1 held-quality meter):
    loop-wide heading-change per km, in degrees/km, computed on DESPIKED
    geometry.

    Spike stubs are fake curviness — a dead-end switchback bounce racks up
    heading change the rider experiences as a defect, not fun — so every
    detected spike interval [lo, hi] is removed (entry point kept; the
    palindrome guarantees continuity) before summing. Same family as the
    fork's harvest ranking (turn-per-length), but loop-wide (both legs) and
    a pure function of served shape, so it is engine/serving comparable and
    cannot be gamed by stub padding.
    """
    drop = set()
    for s in spikes:
        drop.update(range(s["lo"] + 1, s["hi"] + 1))
    kept = [p for i, p in enumerate(loop.pts) if i not in drop]
    ded = []
    for p in kept:
        if not ded or ded[-1] != p:
            ded.append(p)
    if len(ded) < 3:
        return 0.0
    total_turn = 0.0
    length_m = 0.0
    prev_bear = None
    for i in range(len(ded) - 1):
        a, b = grid_to_ll(ded[i]), grid_to_ll(ded[i + 1])
        l = haversine_m(a, b)
        if l <= 0:
            continue
        br = bearing_deg(a, b)
        if prev_bear is not None:
            d = abs(br - prev_bear)
            if d > 180.0:
                d = 360.0 - d
            total_turn += d
        prev_bear = br
        length_m += l
    return total_turn / (length_m / 1000.0) if length_m > 0 else 0.0


def compactness(loop: Loop) -> float:
    """compactness: isoperimetric quotient IQ = 4·π·A / P² of the loop.

    A = |shoelace signed area| of the grid-point polygon in a local
    equirectangular projection centered on the polygon's mean coordinate;
    P = geometric loop length + the closing gap pts[-1]->pts[0] (normally
    ~0 for a round trip). 1.0 = perfect circle; 0 = pure out-and-back.
    Caveats: self-intersecting loops (theta/figure-eight) partially cancel
    signed area, so IQ understates their enclosed area — acceptable for a
    "one fat single loop" quality reading, but do not read IQ alone for
    multi-bulb shapes (see bulb_count). Projection uses one cos(mean-lat)
    for the whole loop (~2 % area error at 300 km span).
    """
    pts = loop.pts
    if len(pts) < 3 or loop.total_m <= 0:
        return 0.0
    ll = [grid_to_ll(k) for k in pts]
    lat0 = sum(p[0] for p in ll) / len(ll)
    lon0 = sum(p[1] for p in ll) / len(ll)
    coslat = math.cos(math.radians(lat0))
    xy = [
        (EARTH_R * math.radians(lon - lon0) * coslat, EARTH_R * math.radians(lat - lat0))
        for lat, lon in ll
    ]
    area2 = 0.0
    for i in range(len(xy)):
        x1, y1 = xy[i]
        x2, y2 = xy[(i + 1) % len(xy)]
        area2 += x1 * y2 - x2 * y1
    area = abs(area2) / 2.0
    perimeter = loop.total_m + seg_len_m(pts[-1], pts[0])
    return 4.0 * math.pi * area / (perimeter * perimeter) if perimeter else 0.0


# --- cross-candidate bank distinctness (wayfinder #46) ----------------------


def _loop_edge_index(loop: Loop, exemption_m: float):
    """(keyset, [(undirected_key, seg_len, in_exemption)]) for a loop.

    Undirected 1e-5 grid key == edge_reuse_geom's key. A segment is in the
    Start Exemption when its midpoint lies within exemption_m of ride start or
    ride end — the shared forced start stem, excluded from avoidable overlap.
    """
    keyset = set()
    segs = []
    for i in range(len(loop.pts) - 1):
        a, b = loop.pts[i], loop.pts[i + 1]
        k = (a, b) if a <= b else (b, a)
        keyset.add(k)
        mid = (loop.cum[i] + loop.cum[i + 1]) / 2.0
        in_ex = mid < exemption_m or mid > loop.total_m - exemption_m
        segs.append((k, loop.seg_lens[i], in_ex))
    return keyset, segs


def bank_distinctness(loops, exemption_m: float = PARAMS["start_exemption_m"]):
    """Near-duplication WITHIN one served bank of K — the cross-candidate axis
    no per-loop metric sees (wayfinder #46).

    Per loop: `max_pair_overlap` = the largest length-weighted undirected-edge
    overlap against any sibling, i.e. the fraction of THIS loop that retreads
    its nearest twin, exemption-discounted (the shared forced start stem does
    not count — same zone as edge_reuse_geom). Also `max_pair_overlap_raw`
    (undiscounted), `best_twin_sep_m` (turnaround great-circle gap to that twin
    — near == the min-separation guard's blind spot; far == distinct
    turnarounds sharing corridor), and `common_trunk_frac_{25,33,50,75}` (loop
    fraction on edges used by >= that share of the bank — the forced-spine vs
    pairwise-avoidable decomposition).

    Returns a list parallel to `loops`; each entry merges into that loop's
    record. Banks of <2 loops yield zeros.
    """
    from collections import Counter
    n = len(loops)
    idx = [_loop_edge_index(L, exemption_m) for L in loops]
    tas = [grid_to_ll(L.pts[L.seam_index]) if L.pts else (0.0, 0.0) for L in loops]
    freq = Counter()  # distinct loops containing each undirected key
    for keyset, _ in idx:
        freq.update(keyset)
    out = []
    for i, L in enumerate(loops):
        keyset_i, segs_i = idx[i]
        rec = {"max_pair_overlap": 0.0, "max_pair_overlap_raw": 0.0, "best_twin_sep_m": 0.0}
        if n >= 2 and L.total_m:
            best = best_raw = best_sep = 0.0
            for j in range(n):
                if j == i:
                    continue
                keyset_j = idx[j][0]
                shared = shared_raw = 0.0
                for k, sl, in_ex in segs_i:
                    if k in keyset_j:
                        shared_raw += sl
                        if not in_ex:
                            shared += sl
                frac = shared / L.total_m
                if frac > best:
                    best, best_sep = frac, haversine_m(tas[i], tas[j])
                if shared_raw / L.total_m > best_raw:
                    best_raw = shared_raw / L.total_m
            rec["max_pair_overlap"] = round(best, 4)
            rec["max_pair_overlap_raw"] = round(best_raw, 4)
            rec["best_twin_sep_m"] = round(best_sep, 1)
        for share, tag in ((0.75, "75"), (0.5, "50"), (0.33, "33"), (0.25, "25")):
            thr = max(2, int(share * n + 0.999))
            clen = (sum(sl for k, sl, ex in segs_i if not ex and freq[k] >= thr)
                    if L.total_m else 0.0)
            rec[f"common_trunk_frac_{tag}"] = round(clen / L.total_m, 4) if L.total_m else 0.0
        out.append(rec)
    return out


# --- per-loop record --------------------------------------------------------


def analyze_loop(loop: Loop) -> dict:
    """All v1 metrics for one loop. edge_reuse_way is filled by the runner's
    /trace_attributes pass (None where not computed)."""
    spikes = find_spikes(loop)
    cs = corridor_stats(loop)
    leg1_m = loop.total_m - loop.leg0_m
    by_cls = {c: sum(1 for s in spikes if s["class"] == c)
              for c in ("seam_uturn", "seam_wrapped", "mid_fwd", "mid_ret")}
    stub_sum = sum(s["stub_m"] for s in spikes)
    max_stub_m = max((s["stub_m"] for s in spikes), default=0.0)
    requested_m = float(loop.meta["distance_m"])
    requested_c = float(loop.meta["curviness"])
    retention = (
        round(loop.curviness_used / requested_c, 4)
        if (loop.curviness_used is not None and requested_c > 0)
        else None
    )
    return {
        **{k: loop.meta[k] for k in ("origin", "distance_m", "curviness", "seed")},
        "k": loop.meta.get("k"),
        "slot": loop.slot,
        "mode": loop.mode,
        "seam_source": loop.seam_source,
        "loop_km": round(loop.declared_m / 1000.0, 3),
        "loop_km_geom": round(loop.total_m / 1000.0, 3),
        "distance_error": round(abs(loop.declared_m - requested_m) / requested_m, 4),
        "seam_frac": round(loop.seam_frac, 4),
        "spike_count": len(spikes),
        "max_stub_km": round(max_stub_m / 1000.0, 4),
        "spike_len_fraction": round(2.0 * stub_sum / loop.total_m, 4) if loop.total_m else 0.0,
        "spike_ge_30m": len(spikes) > 0,
        "spike_ge_500m": max_stub_m >= PARAMS["spike_big_stub_m"],
        "n_spikes_seam_uturn": by_cls["seam_uturn"],
        "n_spikes_seam_wrapped": by_cls["seam_wrapped"],
        "n_spikes_mid_fwd": by_cls["mid_fwd"],
        "n_spikes_mid_ret": by_cls["mid_ret"],
        "seam_uturn": seam_uturn(loop),
        "spikes": spikes,
        "edge_reuse_geom": round(edge_reuse_geom(loop), 4),
        "edge_reuse_way": None,
        "lollipop_stem_fraction": cs["stem_frac"],
        "is_lollipop": cs["stem_frac"] > PARAMS["lollipop_stem_frac_threshold"],
        "bulb_count": cs["bulbs"],
        "stem_out_m": cs["stem_out_m"],
        "stem_back_m": cs["stem_back_m"],
        "shadow_frac": cs["shadow_frac"],
        "rejoin_return_frac": round(cs["stem_back_m"] / leg1_m, 4) if leg1_m else 0.0,
        "compactness": round(compactness(loop), 4),
        "curviness_geom_clean": round(curviness_geom_clean(loop, spikes), 2),
        "curviness_retention": retention,
    }

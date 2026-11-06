#!/usr/bin/env python3
"""Curvature bucket histogram of a tileset, read straight from its tar.

Every DirectedEdge carries a 4-bit curvature bucket (0..15) that mjolnir's
`compute_curvature` (src/mjolnir/util.cc) maps from turning density, with
350 deg/km at bucket 15. The motorcycle costing rewards the bucket
(`kCurvatureFactor`) and treats >= 9 as a curvy road (`kCurvyBucketThreshold`);
the round-trip harvest ranks turnarounds by the same buckets. This prints how
much road length sits in each bucket, so two tilesets' curvy supply can be
compared before any loop is asked for.

Counted: one direction per road (`forward` edge info), no shortcuts, edges a
motorcycle may use in either direction. Length-weighted, km.

Regions: `--region name:lat,lon,radius_km` (repeatable) counts only edges whose
start node lies within the radius; `--split-lon` splits the whole tileset at a
longitude (e.g. 165 for AU | NZ, where NZ also takes lon < -170).

Also reported: the paved share (surface paved_smooth..paved_rough) and the main roads
(motorway..tertiary) alone.

Usage: tile_buckets.py TAR [--region ...] [--split-lon 165] [--json OUT]
Needs numpy (tools/loopqual/venv).
"""
import argparse
import json
import math
import sys
import tarfile

import numpy as np

HEADER_SIZE = 272
NODE_SIZE = 32
TRANSITION_SIZE = 8
EDGE_SIZE = 48
MOTORCYCLE_ACCESS = 1024
CLASSES = ["motorway", "trunk", "primary", "secondary", "tertiary", "unclassified",
           "residential", "service_other"]
# road hierarchy levels and their tile size in degrees (level 3, transit, is skipped)
LEVEL_SIZE = {0: 4.0, 1: 1.0, 2: 0.25}


def bits(word, lo, width):
    return (word >> np.uint64(lo)) & np.uint64((1 << width) - 1)


def read_tile(raw: bytes):
    """-> (curvature, length_m, class, paved, start lat, start lon) of the counted edges, and the
    tile's directed edge count."""
    hdr = np.frombuffer(raw, dtype=np.uint64, count=HEADER_SIZE // 8)
    base_lon, base_lat = np.frombuffer(raw, dtype=np.float32, count=2, offset=8)
    counts = int(hdr[5])
    nodecount = counts & ((1 << 21) - 1)
    edgecount = (counts >> 21) & ((1 << 21) - 1)
    transitioncount = int(np.frombuffer(raw, dtype=np.uint32, count=1, offset=48)[0]) & (
        (1 << 22) - 1)
    node_off = HEADER_SIZE
    edge_off = node_off + nodecount * NODE_SIZE + transitioncount * TRANSITION_SIZE
    nodes = np.frombuffer(raw, dtype=np.uint64, count=nodecount * 4, offset=node_off)
    nodes = nodes.reshape(nodecount, 4)
    edges = np.frombuffer(raw, dtype=np.uint64, count=edgecount * 6, offset=edge_off)
    edges = edges.reshape(edgecount, 6)

    # Start node of every edge: node i owns edges [edge_index, edge_index + edge_count).
    n0, n1 = nodes[:, 0], nodes[:, 1]
    lat = base_lat + (bits(n0, 0, 22).astype(np.float64) * 10 + bits(n0, 22, 4)) * 1e-7
    lon = base_lon + (bits(n0, 26, 22).astype(np.float64) * 10 + bits(n0, 48, 4)) * 1e-7
    first = bits(n1, 0, 21).astype(np.int64)
    count = bits(n1, 21, 7).astype(np.int64)
    owner = np.repeat(np.arange(nodecount), count)
    offset_in_node = np.arange(owner.size) - np.repeat(np.cumsum(count) - count, count)
    starts = np.repeat(first, count) + offset_in_node
    start_node = np.full(edgecount, -1, dtype=np.int64)
    start_node[starts] = owner

    w0, w2, w3, w4, w5 = edges[:, 0], edges[:, 2], edges[:, 3], edges[:, 4], edges[:, 5]
    forward = bits(w0, 61, 1) == 1
    use = bits(w2, 40, 6)
    cls = bits(w2, 54, 3).astype(np.int64)
    access = bits(w3, 0, 12) | bits(w3, 12, 12)
    length = bits(w4, 32, 24).astype(np.float64)
    curv = bits(w4, 60, 4).astype(np.int64)
    surface = bits(w2, 57, 3)
    shortcut = bits(w5, 60, 1) == 1
    # road (0), ramp (1), turn channel (2), track (3), living street (10), service road (11)
    road_use = np.isin(use, np.array([0, 1, 2, 3, 10, 11], dtype=np.uint64))
    keep = forward & ~shortcut & road_use & ((access & np.uint64(MOTORCYCLE_ACCESS)) != 0) & (
        start_node >= 0)
    sn = start_node[keep]
    paved = surface[keep] <= np.uint64(2)  # paved_smooth, paved, paved_rough
    return curv[keep], length[keep], cls[keep], paved, lat[sn], lon[sn], edgecount


def haversine_km(lat1, lon1, lat2, lon2):
    p1, p2 = np.radians(lat1), np.radians(lat2)
    dp, dl = p2 - p1, np.radians(lon2 - lon1)
    a = np.sin(dp / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dl / 2) ** 2
    return 6371.0088 * 2 * np.arcsin(np.sqrt(a))


class Hist:
    def __init__(self):
        self.km = np.zeros((len(CLASSES), 16))
        self.paved_km = np.zeros((len(CLASSES), 16))

    def add(self, curv, length, cls, paved, mask=None):
        if mask is not None:
            curv, length, cls, paved = curv[mask], length[mask], cls[mask], paved[mask]
        np.add.at(self.km, (cls, curv), length / 1000.0)
        np.add.at(self.paved_km, (cls[paved], curv[paved]), length[paved] / 1000.0)

    def summary(self):
        total = self.km.sum()
        by_bucket = self.km.sum(axis=0)
        # what the costing and the harvest can tell apart: straight, mild, moderate, curvy
        groups = {"b0": by_bucket[0], "b1_4": by_bucket[1:5].sum(), "b5_8": by_bucket[5:9].sum(),
                  "b9_15": by_bucket[9:].sum()}
        sealed = self.km[:5].sum(axis=0)  # motorway..tertiary: the network a loop rides
        paved_main = self.paved_km[:5].sum(axis=0)
        out = {
            "road_km": round(total, 1),
            "share": {k: round(v / total, 4) if total else None for k, v in groups.items()},
            "mean_bucket": round(float((by_bucket * np.arange(16)).sum() / total), 3) if total else None,
            "main_roads_km": round(sealed.sum(), 1),
            "main_roads_share": {
                "b0": round(sealed[0] / sealed.sum(), 4) if sealed.sum() else None,
                "b9_15": round(sealed[9:].sum() / sealed.sum(), 4) if sealed.sum() else None,
            },
            "main_roads_mean_bucket": round(float((sealed * np.arange(16)).sum() / sealed.sum()), 3)
            if sealed.sum() else None,
            "main_roads_paved_km": round(paved_main.sum(), 1),
            "main_roads_paved_share": {
                "b0": round(paved_main[0] / paved_main.sum(), 4) if paved_main.sum() else None,
                "b9_15": round(paved_main[9:].sum() / paved_main.sum(), 4) if paved_main.sum() else None,
            },
            "paved_share_of_all_km": round(self.paved_km.sum() / total, 4) if total else None,
            "km_by_bucket": [round(v, 1) for v in by_bucket],
            "main_roads_km_by_bucket": [round(v, 1) for v in sealed],
            "km_by_class": {c: round(self.km[i].sum(), 1) for i, c in enumerate(CLASSES)},
        }
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tar")
    ap.add_argument("--region", action="append", default=[],
                    help="name:lat,lon,radius_km — edges whose start node is within the radius")
    ap.add_argument("--split-lon", type=float, default=None,
                    help="also split the tileset at this longitude (east part also takes lon < -170)")
    ap.add_argument("--json", default=None, help="write the summaries here")
    args = ap.parse_args()

    regions = []
    for r in args.region:
        name, rest = r.split(":", 1)
        lat, lon, rad = (float(x) for x in rest.split(","))
        regions.append((name, lat, lon, rad, Hist()))
    whole, west, east = Hist(), Hist(), Hist()
    tiles = edges_total = 0
    with tarfile.open(args.tar) as tf:
        for m in tf:
            if not m.name.endswith(".gph"):
                continue
            level = int(m.name.lstrip("./").split("/")[0])
            if level not in LEVEL_SIZE:
                continue
            raw = tf.extractfile(m).read()
            curv, length, cls, paved, lat, lon, n = read_tile(raw)
            tiles += 1
            edges_total += n
            whole.add(curv, length, cls, paved)
            if args.split_lon is not None:
                is_east = (lon >= args.split_lon) | (lon < -170.0)
                east.add(curv, length, cls, paved, is_east)
                west.add(curv, length, cls, paved, ~is_east)
            for name, rlat, rlon, rad, h in regions:
                # cheap box test first: most tiles are nowhere near a region
                dlat = rad / 111.0
                dlon = rad / (111.0 * max(0.1, math.cos(math.radians(rlat))))
                box = (np.abs(lat - rlat) <= dlat) & (np.abs(lon - rlon) <= dlon)
                if not box.any():
                    continue
                inside = box.copy()
                inside[box] = haversine_km(lat[box], lon[box], rlat, rlon) <= rad
                h.add(curv, length, cls, paved, inside)
    out = {"tar": args.tar, "tiles": tiles, "directed_edges": edges_total,
           "whole": whole.summary()}
    if args.split_lon is not None:
        out["west"] = west.summary()
        out["east"] = east.summary()
    for name, rlat, rlon, rad, h in regions:
        out[f"region:{name}"] = {"center": [rlat, rlon], "radius_km": rad, **h.summary()}
    text = json.dumps(out, indent=1)
    if args.json:
        with open(args.json, "w") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())

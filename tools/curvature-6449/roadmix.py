"""PROTOTYPE (research/upstream-6449-use-curvature): what roads a run's loops ride, by the
tile's own attributes. Every loop of the first Shuffle of each cell is walked with
/trace_attributes (edge_walk) and its km split by curvature bucket, density class and
road class.
usage: python3 roadmix.py ENGINE_URL RUN_DIR..."""
import json, sys, urllib.request
from pathlib import Path


def post(base, path, body):
    req = urllib.request.Request(base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)


def mix(base, run):
    km = {"all": 0.0, "b0": 0.0, "b1_4": 0.0, "b5_8": 0.0, "b9plus": 0.0, "urban": 0.0, "rural": 0.0,
          "motorway_trunk": 0.0, "tertiary_minor": 0.0}
    for rec in map(json.loads, open(Path(run) / "cells.jsonl")):
        for loop in rec["serves"][0]["loops"]:
            for shape in loop["legs"]:
                d = post(base, "/trace_attributes", {
                    "encoded_polyline": shape, "costing": "motorcycle", "shape_match": "edge_walk",
                    "filters": {"attributes": ["edge.curvature", "edge.density", "edge.length",
                                               "edge.road_class"], "action": "include"}})
                for e in d["edges"]:
                    L = e.get("length", 0.0); c = e.get("curvature", 0); den = e.get("density", 0)
                    rc = e.get("road_class", "")
                    km["all"] += L
                    km["b0" if c == 0 else "b1_4" if c <= 4 else "b5_8" if c <= 8 else "b9plus"] += L
                    if den >= 7: km["urban"] += L
                    if den < 3: km["rural"] += L
                    if rc in ("motorway", "trunk"): km["motorway_trunk"] += L
                    if rc in ("tertiary", "unclassified", "residential", "service_other"): km["tertiary_minor"] += L
    return {k: round(100 * v / km["all"], 1) if k != "all" else round(v) for k, v in km.items()}


if __name__ == "__main__":
    for run in sys.argv[2:]:
        print(Path(run).name, mix(sys.argv[1], run))

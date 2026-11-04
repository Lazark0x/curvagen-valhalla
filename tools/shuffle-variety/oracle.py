"""Reference: the engine's own best 16 loops per cell in ONE K=16 call (the most distinct
loops any no-repeat Shuffle scheme could serve, ranked by the engine). Stored in the same
cells.jsonl shape (one 'serve' with 16 loops) for the quality read."""
import json, sys
from pathlib import Path
import variety as V
base, out = sys.argv[1], Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
with open(out / "cells.jsonl", "w") as f:
    for cell in V.cells():
        o, dkm, c = cell
        lat, lon = V.ORIGINS[o]
        body = {"locations": [{"lat": lat, "lon": lon}] * 2, "costing": "motorcycle",
                "costing_options": V.costing(c),
                "roundtrip": {"target_distance": dkm * 1000.0, "num_candidates": 16, "seed": V.seeds_for(cell)[0]},
                "units": "kilometers", "directions_type": "none"}
        data, lat_s, st = V.post(base, body)
        trips = [data["trip"]] + [a["trip"] for a in data.get("alternates") or []] if st == 200 else []
        loops = [{"legs": [l["shape"] for l in t["legs"]], "length_km": t["summary"]["length"],
                  "provenance": t.get("provenance") or {}} for t in trips]
        f.write(json.dumps({"origin": o, "distance_km": dkm, "curviness": c, "variant": "oracle16",
                            "serves": [{"n": 0, "seed": 0, "status": st, "latency_s": lat_s,
                                        "req_distance_m": dkm * 1000.0, "req_curviness": c,
                                        "loops": loops, "error": None}]}) + "\n")

"""PROTOTYPE harness (research/shuffle-variety, 2026-11-04): 6 consecutive K=3 Shuffles per
cell, emulating the orchestrator (fresh random seed per Shuffle, Bank Fill off), with
optional engine variety knobs and orchestrator-side memory/jitter.

usage: python3 variety.py fire <engine_url> <variant> <out_dir> [--cells N] [--workers 2]
       python3 variety.py analyze <out_dir>...      (prints a summary row per dir)
"""
import json, math, os, random, sys, time, urllib.request, urllib.error
import concurrent.futures as cf
from pathlib import Path

LQ = str(Path(__file__).resolve().parent.parent / "loopqual")
sys.path.insert(0, LQ)

ORIGINS = {  # Serbia only (rig tiles); city-centre level
    "belgrade-vracar": (44.7979, 20.4722), "novi-sad": (45.2671, 19.8335),
    "nis": (43.3209, 21.8958), "kragujevac": (44.0128, 20.9114),
    "zlatibor": (43.7290, 19.7000), "subotica": (46.1003, 19.6658),
}
DISTS_KM = (30, 60, 120)
CURVS = (0.5, 1.0)
N_SHUFFLES = 6
ROT_SCHEDULE = [1.0, 1.04, 0.96, 1.08, 0.92, 1.12]  # orchestrator#67, as deployed

# variant name -> {"engine": {roundtrip knobs}, "apex": bool, "identity": bool,
#                  "rot": bool, "curv_jitter": float, "start_jitter_m": float}
VARIANTS = {
    "base": {},
    "rot": {"rot": True},                                   # L6a deployed distance rotation
    "cjit": {"curv_jitter": 0.05},                          # L6b curviness micro-jitter
    "sjit": {"start_jitter_m": 300.0},                      # L6c start-point jitter
    "apex": {"apex": True},                                 # L1 apex exclusion (+ top-up)
    "apex2": {"apex": True, "engine": {"exclude_radius_m_factor": 2.0}},
    "off": {"engine": {"var_sector_offset": True}},         # L2
    "pick": {"engine": {"var_shortlist": 16}},              # L3a widen pool, seed over all
    "pickt": {"engine": {"var_shortlist": 16, "var_pick_tol": 0.15}},  # L3b near-best only
    "b12": {"engine": {"var_buckets": 12}},                 # L5
    "ident": {"identity": True},                            # L4 identity exclusion 0.5
    "ident3": {"identity": True, "engine": {"exclude_loop_frac": 0.3}},
    "ident8": {"identity": True, "engine": {"exclude_loop_frac": 0.8}},
    "apexh": {"apex": True, "engine": {"exclude_radius_m_factor": 0.5}},
    "dist": {"engine": {"var_shortlist": 32, "var_distinct": 0.5}},          # L3c
    "distt": {"engine": {"var_shortlist": 32, "var_distinct": 0.5, "var_pick_tol": 0.15}},
    "dist16": {"engine": {"var_shortlist": 16, "var_distinct": 0.5}},
    # radius as a fraction of the requested distance (PR #29's parameterisation):
    # apex = 0.05 x distance (the min-separation default), apex2 = 0.10
    "apex08": {"apex": True, "engine": {"exclude_radius_frac": 0.08}},
    "apex10": {"apex": True, "engine": {"exclude_radius_frac": 0.10}},
    "apex12": {"apex": True, "engine": {"exclude_radius_frac": 0.12}},
    "apex15": {"apex": True, "engine": {"exclude_radius_frac": 0.15}},
    # L3d: deepest-best — evaluate 32 per sector, serve the best-scoring survivor (the seed
    # no longer picks; with apex memory supplying the variety, the pick maximises quality)
    "apex06": {"apex": True, "engine": {"exclude_radius_frac": 0.06}},
    "guard": {"engine": {"var_guard": 0.30}},   # L1g: quality guard on the excluded run
    "deep": {"engine": {"var_shortlist": 32, "var_pick_tol": 1e-9}},
}


def r2(x):
    return float(f"{x:.2f}")


def costing(c):
    uh = max(0.1, 1 - c * 0.7)
    return {"motorcycle": {
        "use_highways": r2(uh), "use_trails": r2(min(c * 0.8, 0.8)),
        "use_tolls": 0.5, "top_speed": int(120 - c * 40),
        "maneuver_penalty": max(int(10 - c * 10), 0), "prefer_curvature": r2(c),
        "reuse_penalty": 0.8, "curviness_continuity": r2(c), "prefer_elevation": 0.3}}


def post(base, body):
    req = urllib.request.Request(f"{base}/route", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    t = time.time()
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            data = json.load(r)
        return data, time.time() - t, 200
    except urllib.error.HTTPError as e:
        return {"error": e.read()[:300].decode(errors="replace")}, time.time() - t, e.code


def decode(s, prec=6):
    pts = []; i = lat = lon = 0
    while i < len(s):
        for which in (0, 1):
            shift = res = 0
            while True:
                b = ord(s[i]) - 63; i += 1; res |= (b & 0x1f) << shift; shift += 5
                if b < 0x20:
                    break
            d = ~(res >> 1) if res & 1 else res >> 1
            if which == 0:
                lat += d
            else:
                lon += d
        pts.append((lat / 10 ** prec, lon / 10 ** prec))
    return pts


def cells():
    out = []
    for o in ORIGINS:
        for d in DISTS_KM:
            for c in CURVS:
                out.append((o, d, c))
    return out


def seeds_for(cell):
    rng = random.Random(f"{cell[0]}|{cell[1]}|{cell[2]}")
    return [rng.randrange(2 ** 31) for _ in range(N_SHUFFLES)]


def variant(vname):
    """'a+b+c' merges the named variants (engine knobs merged)."""
    out = {"engine": {}}
    for part in vname.split("+"):
        v = VARIANTS[part]
        for k, val in v.items():
            if k == "engine":
                out["engine"].update(val)
            else:
                out[k] = val
    return out


def run_cell(base, vname, cell):
    v = variant(vname)
    o, dkm, c = cell
    lat0, lon0 = ORIGINS[o]
    seeds = seeds_for(cell)
    rng = random.Random(f"jit|{o}|{dkm}|{c}")
    apexes, idents, serves = [], [], []
    for n, seed in enumerate(seeds):
        dm = dkm * 1000.0 * (ROT_SCHEDULE[n] if v.get("rot") else 1.0)
        cc = c
        if v.get("curv_jitter") and n > 0:
            cc = min(1.0, max(0.0, c + rng.choice([-1, 1]) * v["curv_jitter"] * ((n + 1) // 2)))
        lat, lon = lat0, lon0
        if v.get("start_jitter_m") and n > 0:
            ang = rng.random() * 2 * math.pi
            r = v["start_jitter_m"]
            lat += r * math.cos(ang) / 111_320.0
            lon += r * math.sin(ang) / (111_320.0 * math.cos(math.radians(lat0)))
        rt = {"target_distance": dm, "num_candidates": 3, "seed": seed}
        eng = dict(v.get("engine", {}))
        fac = eng.pop("exclude_radius_m_factor", None)
        frac = eng.pop("exclude_radius_frac", None)
        if frac:
            fac = frac / 0.05
        rt.update(eng)
        if v.get("apex") and apexes:
            rt["exclude_turnarounds"] = [{"lat": a[0], "lon": a[1]} for a in apexes][-64:]
            if fac:
                rt["exclude_radius_m"] = fac * 0.1 * dm / 2.0
        if v.get("identity"):
            rt["emit_identity"] = True
            if idents:
                rt["exclude_loops"] = idents[-32:]
        body = {"locations": [{"lat": lat, "lon": lon}] * 2, "costing": "motorcycle",
                "costing_options": costing(cc), "roundtrip": rt,
                "units": "kilometers", "directions_type": "none"}
        data, lat_s, status = post(base, body)
        trips = []
        if status == 200:
            trips = [data["trip"]] + [a["trip"] for a in data.get("alternates") or []]
        loops = []
        for tr in trips:
            prov = tr.get("provenance") or {}
            ident = prov.pop("identity", None)
            loops.append({"legs": [l["shape"] for l in tr["legs"]],
                          "length_km": tr["summary"]["length"], "provenance": prov})
            apexes.append(decode(tr["legs"][0]["shape"])[-1])
            if ident is not None:
                idents.append(ident)
        serves.append({"n": n, "seed": seed, "status": status, "latency_s": round(lat_s, 4),
                       "req_distance_m": dm, "req_curviness": cc, "loops": loops,
                       "error": None if status == 200 else data.get("error")})
    return {"origin": o, "distance_km": dkm, "curviness": c, "variant": vname, "serves": serves}


def fire(base, vname, out_dir, workers=2, limit=None):
    out = Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    cs = cells()[:limit] if limit else cells()
    t0 = time.time()
    with open(out / "cells.jsonl", "w") as f, cf.ThreadPoolExecutor(workers) as ex:
        for rec in ex.map(lambda c: run_cell(base, vname, c), cs):
            f.write(json.dumps(rec) + "\n"); f.flush()
    (out / "meta.json").write_text(json.dumps({"engine": base, "variant": vname,
                                               "variant_spec": variant(vname),
                                               "wall_s": round(time.time() - t0, 1)}))


# ---------------- analysis ----------------
CELL = 0.0003  # ~30 m


def grid(pts):
    return {(round(a / CELL), round(b / CELL)) for a, b in pts}


def cover(pts, g):
    hit = 0
    for la, lo in pts:
        ci, cj = round(la / CELL), round(lo / CELL)
        if any((ci + di, cj + dj) in g for di in (-1, 0, 1) for dj in (-1, 0, 1)):
            hit += 1
    return hit / len(pts) if pts else 0.0


def sim(a, b):
    return min(cover(a[0], b[1]), cover(b[0], a[1]))


def analyze_cell(rec):
    import metrics
    o, dkm, c = rec["origin"], rec["distance_km"], rec["curviness"]
    seen = []
    out = {"rep": 0, "rep8": 0, "later": 0, "first_n": 0, "serves": 0, "full": 0, "errors": 0,
           "lat": [], "loops": [], "distinct": 0}
    for s in rec["serves"]:
        out["serves"] += 1
        out["lat"].append(s["latency_s"])
        if s["status"] != 200:
            out["errors"] += 1
        if len(s["loops"]) >= 3:
            out["full"] += 1
        geoms = []
        bank = []
        for slot, L in enumerate(s["loops"]):
            leg0, leg1 = decode(L["legs"][0]), decode(L["legs"][1])
            pts = leg0 + leg1[1:]
            g = (pts, grid(pts))
            geoms.append(g)
            if s["n"] > 0:
                out["later"] += 1
                best = max((sim(g, p) for p in seen), default=0.0)
                out["rep"] += best >= 0.9
                out["rep8"] += best >= 0.8
            else:
                out["first_n"] += 1
            trip = {"legs": [{"shape": L["legs"][0]}, {"shape": L["legs"][1]}],
                    "summary": {"length": L["length_km"]}}
            meta = {"origin": o, "distance_m": dkm * 1000.0, "curviness": c, "seed": s["seed"],
                    "k": 3}
            bank.append((metrics.Loop.from_engine_trip(trip, meta, slot), L["provenance"], s["n"]))
        # novelty clusters: a loop is a new distinct loop if < 0.9 vs everything seen
        for g in geoms:
            if not any(sim(g, p) >= 0.9 for p in seen):
                out["distinct"] += 1
            seen.append(g)
        dist = metrics.bank_distinctness([b[0] for b in bank]) if bank else []
        for i, (lp, prov, n) in enumerate(bank):
            r = metrics.analyze_loop(lp)
            keep = {k: r[k] for k in ("distance_error", "retrace_family", "retrace_d1", "retrace_d4",
                                      "retrace_d1l", "am25_new_m", "reuse_disc_m",
                                      "curviness_geom_clean", "spike_ge_500m", "ring_n",
                                      "compactness", "loop_km")}
            keep["max_pair_overlap_served"] = dist[i].get("max_pair_overlap_served")
            keep["shuffle"] = n
            keep["prov"] = prov
            keep["curviness"] = c
            out["loops"].append(keep)
    return out


def quant(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))] if s else float("nan")


def summarize(out_dir, workers=4):
    recs = [json.loads(x) for x in open(Path(out_dir) / "cells.jsonl")]
    with cf.ProcessPoolExecutor(workers) as ex:
        res = list(ex.map(analyze_cell, recs))
    agg = {k: sum(r[k] for r in res) for k in ("rep", "rep8", "later", "serves", "full", "errors",
                                               "distinct", "first_n")}
    lat = sum((r["lat"] for r in res), [])
    loops = sum((r["loops"] for r in res), [])
    first = [l for l in loops if l["shuffle"] == 0]
    n = len(loops)
    pct = lambda f, L=loops: 100.0 * sum(1 for l in L if f(l)) / len(L) if L else float("nan")
    mean = lambda k, L=loops: sum(l[k] for l in L) / len(L) if L else float("nan")
    de = [l["distance_error"] for l in loops]
    mpo = [l["max_pair_overlap_served"] for l in loops if l["max_pair_overlap_served"] is not None]
    by_cell_full = {}
    for rec, r in zip(recs, res):
        by_cell_full[f"{rec['origin']}/{rec['distance_km']}/{rec['curviness']}"] = (r["full"], r["serves"])
    s = {
        "dir": str(out_dir),
        "repeat_pct": 100.0 * agg["rep"] / agg["later"] if agg["later"] else float("nan"),
        "repeat08_pct": 100.0 * agg["rep8"] / agg["later"] if agg["later"] else float("nan"),
        "distinct_per_cell": agg["distinct"] / len(recs),
        "fill_pct": 100.0 * agg["full"] / agg["serves"],
        "errors": agg["errors"],
        "loops": n,
        "lat_p50": quant(lat, 0.5), "lat_p95": quant(lat, 0.95), "lat_mean": sum(lat) / len(lat),
        "curv_mean": mean("curviness_geom_clean"),
        "curv_mean_c05": mean("curviness_geom_clean", [l for l in loops if l["curviness"] == 0.5]),
        "curv_mean_c10": mean("curviness_geom_clean", [l for l in loops if l["curviness"] == 1.0]),
        "retrace_pct": pct(lambda l: l["retrace_family"]),
        "d1_pct": pct(lambda l: l["retrace_d1"]),
        "d1l_pct": pct(lambda l: l["retrace_d1l"]),
        "d4_pct": pct(lambda l: l["retrace_d4"]),
        "am25_new_m": mean("am25_new_m"),
        "spike500": sum(1 for l in loops if l["spike_ge_500m"]),
        "derr_mean": sum(de) / len(de), "derr_p90": quant(de, 0.9),
        "fallback_pct": pct(lambda l: (l["prov"] or {}).get("rung", 0) == 2),
        "gated_pct": pct(lambda l: (l["prov"] or {}).get("gated", False)),
        "rescue_pct": pct(lambda l: (l["prov"] or {}).get("builder") == "rescue"),
        "relaxed_pct": pct(lambda l: (l["prov"] or {}).get("relaxed", 0) > 0),
        "topup_pct": pct(lambda l: (l["prov"] or {}).get("topup", False)),
        "t2_mean": sum(mpo) / len(mpo) if mpo else float("nan"),
        "t2_gt06_pct": 100.0 * sum(1 for x in mpo if x > 0.6) / len(mpo) if mpo else float("nan"),
        "first_curv": mean("curviness_geom_clean", first),
        "first_retrace_pct": pct(lambda l: l["retrace_family"], first),
        "curv_by_shuffle": [round(mean("curviness_geom_clean", [l for l in loops if l["shuffle"] == k]), 1)
                            for k in range(N_SHUFFLES)],
        "retrace_by_shuffle": [round(pct(lambda l: l["retrace_family"], [l for l in loops if l["shuffle"] == k]), 1)
                               for k in range(N_SHUFFLES)],
        "derr_by_shuffle": [round(mean("distance_error", [l for l in loops if l["shuffle"] == k]), 3)
                            for k in range(N_SHUFFLES)],
        "underfilled_cells": {k: v for k, v in by_cell_full.items() if v[0] < v[1]},
    }
    return s


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "fire":
        base, vname, out = sys.argv[2:5]
        lim = int(sys.argv[sys.argv.index("--cells") + 1]) if "--cells" in sys.argv else None
        w = int(sys.argv[sys.argv.index("--workers") + 1]) if "--workers" in sys.argv else 2
        fire(base, vname, out, w, lim)
    elif cmd == "analyze":
        for d in sys.argv[2:]:
            s = summarize(d)
            Path(d, "summary.json").write_text(json.dumps(s, indent=1))
            print(json.dumps({k: (round(v, 3) if isinstance(v, float) else v) for k, v in s.items()}))

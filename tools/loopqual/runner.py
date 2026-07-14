#!/usr/bin/env python3
"""loopqual run — fire a corpus at an engine/serving endpoint, compute the
v1 metrics per loop, and write loops.jsonl + report.json + report.md.

Determinism: responses are cached on disk (resume = skip existing files);
analysis iterates files in sorted order; aggregates are pure functions of
loops.jsonl. Same corpus + same engine -> identical loops.jsonl and
identical report aggregates (only the run-provenance block varies).
"""

import concurrent.futures
import datetime
import hashlib
import json
import statistics as st
import time
import urllib.error
import urllib.request
from collections import Counter
from itertools import groupby
from pathlib import Path

import metrics
from metrics import Loop, PARAMS

HARNESS_VERSION = "1.0.0"
REQUEST_TIMEOUT_S = 120
# Cloudflare's Browser Integrity Check 403s (error 1010) the default
# Python-urllib agent — identify honestly instead.
HEADERS = {
    "Content-Type": "application/json",
    "User-Agent": f"loopqual/{HARNESS_VERSION} (curvagen loop-quality harness)",
}


# --- corpus -----------------------------------------------------------------


def load_corpus(path: Path):
    text = path.read_text()
    if path.suffix in (".yaml", ".yml"):
        import yaml
        corpus = yaml.safe_load(text)
    else:
        corpus = json.loads(text)
    for field in ("id", "origins", "cells"):
        if field not in corpus:
            raise ValueError(f"corpus missing required field '{field}'")
    return corpus, hashlib.sha256(text.encode()).hexdigest()


def enumerate_jobs(corpus):
    """Deterministic job order: cells in file order, origins in file order,
    then distances, then seeds (the atlas corpus order)."""
    jobs = []
    for cell in corpus["cells"]:
        names = cell.get("origins", "all")
        if names == "all":
            names = list(corpus["origins"])
        for name in names:
            o = corpus["origins"][name]
            for dist in cell["distances_m"]:
                for seed in cell["seeds"]:
                    jobs.append({
                        "origin": name,
                        "lat": o["lat"],
                        "lon": o["lon"],
                        "distance_m": int(dist),
                        "curviness": float(cell["curviness"]),
                        "seed": int(seed),
                        "k": int(cell.get("k", corpus.get("k", 12))),
                        "avoid_motorways": bool(
                            cell.get("avoid_motorways", corpus.get("avoid_motorways", True))
                        ),
                    })
    return jobs


def job_filename(job) -> str:
    return f"{job['origin']}_d{job['distance_m'] // 1000}_c{job['curviness']}_s{job['seed']}.json"


# --- requests ---------------------------------------------------------------


def costing(curviness: float, avoid_motorways: bool) -> dict:
    """Port of Backend/orchestrator/crates/domain/src/costing.rs::costing —
    the prod serving contract, verbatim (round2 = half-even via format,
    int() truncation)."""
    if avoid_motorways:
        use_highways = max(0.0, 1.0 - curviness * 1.2)
        use_tolls = 0.0
    else:
        use_highways = max(0.1, 1.0 - curviness * 0.7)
        use_tolls = 0.5
    use_trails = min(0.8, curviness * 0.8)
    top_speed = int(120.0 - curviness * 40.0)
    maneuver_penalty = max(0, int(10.0 - curviness * 10.0))

    def r2(x):
        return float(f"{x:.2f}")

    return {
        "motorcycle": {
            "use_highways": r2(use_highways),
            "use_trails": r2(use_trails),
            "use_tolls": use_tolls,
            "top_speed": top_speed,
            "maneuver_penalty": maneuver_penalty,
            "prefer_curvature": r2(curviness),
            "reuse_penalty": 0.8,
            "curviness_continuity": r2(curviness),
            "prefer_elevation": 0.3,
        }
    }


def build_request(job, serving: bool):
    """Engine mode: fork /route with the roundtrip sub-message, prod-verbatim
    costing. Serving mode: orchestrator POST /round-trip app DTO
    (startPoint is [lon, lat])."""
    if serving:
        return "/round-trip", {
            "startPoint": [job["lon"], job["lat"]],
            "distance": float(job["distance_m"]),
            "curviness": job["curviness"],
            "avoidMotorways": job["avoid_motorways"],
            "seed": job["seed"],
        }
    return "/route", {
        "locations": [
            {"lat": job["lat"], "lon": job["lon"]},
            {"lat": job["lat"], "lon": job["lon"]},
        ],
        "costing": "motorcycle",
        "costing_options": costing(job["curviness"], job["avoid_motorways"]),
        "roundtrip": {
            "target_distance": job["distance_m"],
            "num_candidates": job["k"],
            "seed": job["seed"],
        },
        "units": "kilometers",
        "directions_type": "none",
    }


def post_json(url: str, body: dict, timeout=REQUEST_TIMEOUT_S):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=HEADERS)
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return 200, time.perf_counter() - t0, json.load(r)
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")[:500]
        try:
            payload = json.loads(raw)
        except ValueError:
            payload = {"error": raw}
        return e.code, time.perf_counter() - t0, payload
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return 0, time.perf_counter() - t0, {"error": f"transport: {e}"}


def probe_endpoint(engine: str, serving: bool):
    """Best-effort provenance probe: engine /status or orchestrator /health."""
    url = engine.rstrip("/") + ("/health" if serving else "/status")
    try:
        probe_req = urllib.request.Request(url, headers={"User-Agent": HEADERS["User-Agent"]})
        with urllib.request.urlopen(probe_req, timeout=10) as r:
            return {"url": url, "status": r.status, "body": json.load(r)}
    except urllib.error.HTTPError as e:
        return {"url": url, "status": e.code, "body": e.read().decode(errors="replace")[:200]}
    except Exception as e:  # provenance only — never fail the run
        return {"url": url, "error": str(e)[:200]}


def fire_corpus(jobs, engine, serving, out_dir: Path, workers, engine_note):
    responses = out_dir / "responses"
    responses.mkdir(parents=True, exist_ok=True)
    total = len(jobs)
    done = 0

    def do_job(job):
        fn = responses / job_filename(job)
        if fn.exists():
            return job, "cached", None
        path, body = build_request(job, serving)
        status, dt, resp = post_json(engine.rstrip("/") + path, body)
        if serving:
            n_routes = len(resp.get("routes", [])) if status == 200 else 0
        else:
            n_routes = (1 + len(resp.get("alternates", []))) if status == 200 else 0
        record = {
            "meta": {
                "origin": job["origin"], "lat": job["lat"], "lon": job["lon"],
                "distance_m": job["distance_m"], "curviness": job["curviness"],
                "seed": job["seed"], "k": job["k"],
                "mode": "serving" if serving else "engine",
                "status": status, "latency_s": round(dt, 3), "n_routes": n_routes,
                "engine": engine_note or engine,
            },
            "request": body,
            "response": resp,
        }
        fn.write_text(json.dumps(record))
        return job, status, n_routes

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        for job, status, n_routes in ex.map(do_job, jobs):
            done += 1
            tag = "cached" if status == "cached" else f"{status} {n_routes} routes"
            print(f"[{done}/{total}] {job_filename(job)}: {tag}", flush=True)
    return responses


# --- analysis ---------------------------------------------------------------


def analyze_responses(responses_dir: Path, requested_by_file=None):
    """-> (records, failures). Iterates response files in sorted order."""
    records, failures = [], []
    for fn in sorted(responses_dir.glob("*.json")):
        rec = json.loads(fn.read_text())
        meta = rec["meta"]
        mode = meta.get("mode", "engine")
        if meta["status"] != 200:
            failures.append({
                "file": fn.name,
                **{k: meta[k] for k in ("origin", "distance_m", "curviness", "seed", "status")},
                "error": json.dumps(rec["response"])[:300],
            })
            continue
        resp = rec["response"]
        if mode == "serving":
            candidates = resp.get("routes", [])
            make = Loop.from_serving_route
        else:
            candidates = [resp["trip"]] + [a["trip"] for a in resp.get("alternates", [])]
            make = Loop.from_engine_trip
        for slot, candidate in enumerate(candidates):
            loop = make(candidate, meta, slot)
            record = metrics.analyze_loop(loop)
            record["file"] = fn.name
            records.append(record)
    return records, failures


def way_reuse_of_points(pts, trace_url: str):
    """edge_reuse_way: fraction of route length on OSM ways traversed in
    more than one non-contiguous run — the exact ADR-0033 / retired
    eval_routes.py::edge_reuse_frac metric, via /trace_attributes map_snap.
    None on trace failure (e.g. error 153 'Too many shape points' — the
    16000-point limit that 300 km loops exceed)."""
    body = {
        "encoded_polyline": metrics.encode_polyline(pts),
        "costing": "motorcycle",
        "shape_match": "map_snap",
        "filters": {"attributes": ["edge.way_id", "edge.length"], "action": "include"},
    }
    req = urllib.request.Request(trace_url, data=json.dumps(body).encode(), headers=HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            edges = json.load(r).get("edges", [])
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, OSError):
        return None
    runs = [(way, sum(e.get("length", 0.0) for e in grp))
            for way, grp in groupby(edges, key=lambda e: e.get("way_id"))]
    run_count, run_len = {}, {}
    for way, length in runs:
        run_count[way] = run_count.get(way, 0) + 1
        run_len[way] = run_len.get(way, 0.0) + length
    total = sum(e.get("length", 0.0) for e in edges) or 1.0
    reused = sum(run_len[w] for w in run_count if run_count[w] > 1)
    return round(reused / total, 4)


def way_pass(responses_dir: Path, records, trace_base: str, way_seeds, workers):
    """Fill edge_reuse_way on records whose seed is in way_seeds."""
    if not way_seeds:
        return 0, 0
    trace_url = trace_base.rstrip("/") + "/trace_attributes"
    tasks = []  # (file, slot, raw_pts)
    for fn in sorted(responses_dir.glob("*.json")):
        rec = json.loads(fn.read_text())
        meta = rec["meta"]
        if meta["status"] != 200 or meta["seed"] not in way_seeds:
            continue
        mode = meta.get("mode", "engine")
        resp = rec["response"]
        if mode == "serving":
            for slot, route in enumerate(resp.get("routes", [])):
                tasks.append((fn.name, slot,
                              metrics.decode_polyline_3d(route["paths"][0]["points"])))
        else:
            routes = [resp["trip"]] + [a["trip"] for a in resp.get("alternates", [])]
            for slot, r in enumerate(routes):
                pts = (metrics.decode_polyline(r["legs"][0]["shape"])
                       + metrics.decode_polyline(r["legs"][1]["shape"])[1:])
                tasks.append((fn.name, slot, pts))

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(way_reuse_of_points, pts, trace_url): (f, s) for f, s, pts in tasks}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            results[futs[fut]] = fut.result()
            done += 1
            if done % 100 == 0:
                print(f"way pass: {done}/{len(tasks)}", flush=True)
    filled = skipped = 0
    for r in records:
        key = (r["file"], r["slot"])
        if key in results:
            r["edge_reuse_way"] = results[key]
            if results[key] is None:
                skipped += 1
            else:
                filled += 1
    return filled, skipped


# --- aggregation --------------------------------------------------------------


def quant(vals, q):
    """Atlas quantile convention: sorted vals[min(len-1, int(q*len))]."""
    if not vals:
        return 0.0
    vals = sorted(vals)
    return vals[min(len(vals) - 1, int(q * len(vals)))]


def summarize(loops):
    """One aggregate block over a slice of per-loop records (v1 scalars)."""
    n = len(loops)
    if not n:
        return {"n_loops": 0}
    spiked = [l for l in loops if l["spike_ge_30m"]]
    stubs = [s["stub_m"] for l in loops for s in l["spikes"]]
    lolli = [l for l in loops if l["is_lollipop"]]
    reuse = [l["edge_reuse_geom"] for l in loops]
    comp = [l["compactness"] for l in loops]
    derr = [l["distance_error"] for l in loops]
    curvc = [l["curviness_geom_clean"] for l in loops]
    slf_spiked = [l["spike_len_fraction"] for l in spiked]
    way = [l["edge_reuse_way"] for l in loops if l.get("edge_reuse_way") is not None]
    ret = [l["curviness_retention"] for l in loops if l.get("curviness_retention") is not None]
    cls = Counter(s["class"] for l in loops for s in l["spikes"])
    block = {
        "n_loops": n,
        "spike_loop_frac": round(len(spiked) / n, 4),
        "spike_500m_loop_frac": round(sum(1 for l in loops if l["spike_ge_500m"]) / n, 4),
        "n_spikes": len(stubs),
        "stub_p50_m": round(quant(stubs, 0.5), 1),
        "stub_p90_m": round(quant(stubs, 0.9), 1),
        "stub_max_m": round(max(stubs), 1) if stubs else 0.0,
        "spike_count_mean": round(sum(l["spike_count"] for l in loops) / n, 4),
        "spike_len_fraction_p50_spiked": round(quant(slf_spiked, 0.5), 4),
        "spike_len_fraction_p90_spiked": round(quant(slf_spiked, 0.9), 4),
        "spikes_seam_uturn": cls.get("seam_uturn", 0),
        "spikes_seam_wrapped": cls.get("seam_wrapped", 0),
        "spikes_mid_leg": cls.get("mid_fwd", 0) + cls.get("mid_ret", 0),
        "lollipop_frac": round(len(lolli) / n, 4),
        "stem_frac_p90": round(quant([l["lollipop_stem_fraction"] for l in loops], 0.9), 4),
        "bulb_p50_lollipop": quant([l["bulb_count"] for l in lolli], 0.5) if lolli else 0,
        "edge_reuse_geom_mean": round(st.mean(reuse), 4),
        "edge_reuse_geom_p50": round(quant(reuse, 0.5), 4),
        "edge_reuse_geom_p90": round(quant(reuse, 0.9), 4),
        "edge_reuse_geom_max": round(max(reuse), 4),
        "reuse_gt_030_frac": round(sum(1 for r in reuse if r > 0.30) / n, 4),
        "curviness_geom_clean_mean": round(st.mean(curvc), 2),
        "curviness_geom_clean_p50": round(quant(curvc, 0.5), 2),
        "compactness_mean": round(st.mean(comp), 4),
        "compactness_p50": round(quant(comp, 0.5), 4),
        "compactness_p90": round(quant(comp, 0.9), 4),
        "distance_error_mean": round(st.mean(derr), 4),
        "distance_error_p50": round(quant(derr, 0.5), 4),
        "distance_error_p90": round(quant(derr, 0.9), 4),
        "distance_error_max": round(max(derr), 4),
    }
    if way:
        block["edge_reuse_way_mean"] = round(st.mean(way), 4)
        block["edge_reuse_way_n"] = len(way)
    if ret:
        block["curviness_retention_mean"] = round(st.mean(ret), 4)
        block["curviness_retention_n"] = len(ret)
    return block


def aggregate(records):
    out = {"overall": summarize(records), "by_curviness": {}}
    for c in sorted({l["curviness"] for l in records}):
        sub = [l for l in records if l["curviness"] == c]
        block = summarize(sub)
        block["by_origin"] = {
            o: summarize([l for l in sub if l["origin"] == o])
            for o in sorted({l["origin"] for l in sub})
        }
        block["by_distance_km"] = {
            str(d // 1000): summarize([l for l in sub if l["distance_m"] == d])
            for d in sorted({l["distance_m"] for l in sub})
        }
        block["by_slot"] = {
            str(s): summarize([l for l in sub if l["slot"] == s])
            for s in sorted({l["slot"] for l in sub})
        }
        out["by_curviness"][str(c)] = block
    return out


# --- reports ------------------------------------------------------------------


HEADLINE_ROWS = [
    ("spike_loop_frac", "loops with ≥1 spike (stub ≥30 m)", "pct"),
    ("spike_500m_loop_frac", "loops with a stub ≥500 m", "pct"),
    ("stub_p50_m", "stub length p50 (m, over spikes)", "num"),
    ("stub_p90_m", "stub length p90 (m)", "num"),
    ("stub_max_m", "worst stub (m)", "num"),
    ("spike_len_fraction_p50_spiked", "spike_len_fraction p50 (spiked loops)", "pct"),
    ("lollipop_frac", "lollipop loops (stem_frac >10 %)", "pct"),
    ("edge_reuse_geom_mean", "edge_reuse_geom mean", "num"),
    ("edge_reuse_way_mean", "edge_reuse_way mean (seed subset)", "num"),
    ("compactness_mean", "compactness mean (IQ)", "num"),
    ("distance_error_mean", "distance_error mean", "pct"),
    ("curviness_retention_mean", "curviness_retention mean", "num"),
]


def fmt(v, kind="num"):
    if v is None:
        return "—"
    if kind == "pct":
        return f"{100.0 * v:.1f}%"
    if isinstance(v, float):
        return f"{v:.4g}" if abs(v) < 1000 else f"{v:.0f}"
    return str(v)


def write_reports(out_dir: Path, run_info, records, failures, aggregates):
    report = {
        "metrics": metrics.METRICS_VERSION,
        "harness": f"loopqual {HARNESS_VERSION}",
        "params": PARAMS,
        "run": run_info,
        "failures": failures,
        "aggregates": aggregates,
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=1))

    lines = [
        f"# loopqual report — metrics {metrics.METRICS_VERSION}",
        "",
        f"- engine: `{run_info['engine']}` ({run_info['mode']} mode)"
        + (f" — {run_info['engine_note']}" if run_info.get("engine_note") else ""),
        f"- corpus: `{run_info['corpus_id']}` (sha256 `{run_info['corpus_sha256'][:12]}…`)",
        f"- run: {run_info['date']} — {run_info['n_requests']} requests, "
        f"{run_info['n_ok']} ok, {run_info['n_failed']} failed, {len(records)} loops",
        f"- request latency: p50 {run_info['latency_p50_s']} s, "
        f"p95 {run_info['latency_p95_s']} s (fresh requests only)",
        "",
    ]
    for c, block in aggregates["by_curviness"].items():
        lines += [f"## curviness {c} (n={block['n_loops']})", "",
                  "| metric | value |", "|---|---|"]
        for key, label, kind in HEADLINE_ROWS:
            if key in block:
                lines.append(f"| {label} | {fmt(block[key], kind)} |")
        lines.append("")
        for section, title in (("by_origin", "origin"), ("by_distance_km", "distance (km)"),
                               ("by_slot", "slot")):
            lines += [f"### by {title}", "",
                      f"| {title} | n | spike | ≥500 m | lollipop | reuse_geom | compactness | dist_err |",
                      "|---|---|---|---|---|---|---|---|"]
            for name, b in block[section].items():
                lines.append(
                    f"| {name} | {b['n_loops']} | {fmt(b.get('spike_loop_frac'), 'pct')} "
                    f"| {fmt(b.get('spike_500m_loop_frac'), 'pct')} "
                    f"| {fmt(b.get('lollipop_frac'), 'pct')} | {fmt(b.get('edge_reuse_geom_mean'))} "
                    f"| {fmt(b.get('compactness_mean'))} | {fmt(b.get('distance_error_mean'), 'pct')} |"
                )
            lines.append("")
    if failures:
        lines += ["## failed requests", ""]
        for f in failures:
            lines.append(f"- `{f['file']}` — HTTP {f['status']}: {f['error'][:120]}")
        lines.append("")
    lines += ["## parameters", "", "```json", json.dumps(PARAMS, indent=1), "```", ""]
    (out_dir / "report.md").write_text("\n".join(lines))


# --- entry ---------------------------------------------------------------------


def run(args):
    corpus_path = Path(args.corpus)
    corpus, corpus_sha = load_corpus(corpus_path)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    jobs = enumerate_jobs(corpus)
    serving = args.serving
    workers = args.workers if args.workers else (1 if serving else 3)

    probe = probe_endpoint(args.engine, serving)
    print(f"loopqual run: {len(jobs)} requests -> {args.engine} "
          f"({'serving' if serving else 'engine'} mode, {workers} workers)", flush=True)

    t0 = time.perf_counter()
    responses_dir = fire_corpus(jobs, args.engine, serving, out_dir, workers, args.engine_note)
    print(f"corpus fired/resumed in {time.perf_counter() - t0:.0f}s", flush=True)

    t1 = time.perf_counter()
    records, failures = analyze_responses(responses_dir)
    print(f"analyzed {len(records)} loops in {time.perf_counter() - t1:.0f}s "
          f"({len(failures)} failed requests)", flush=True)

    way_seeds = set(corpus.get("way_reuse_seeds", []))
    trace_base = args.trace_engine or (None if serving else args.engine)
    if way_seeds and trace_base and not args.no_way:
        t2 = time.perf_counter()
        filled, skipped = way_pass(responses_dir, records, trace_base, way_seeds, workers)
        print(f"way pass: {filled} filled, {skipped} trace-failed "
              f"in {time.perf_counter() - t2:.0f}s", flush=True)
    elif way_seeds and not trace_base:
        print("way pass skipped: no /trace_attributes endpoint in serving mode "
              "(pass --trace-engine to enable)", flush=True)

    with (out_dir / "loops.jsonl").open("w") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")

    latencies = sorted(
        json.loads(fn.read_text())["meta"]["latency_s"]
        for fn in responses_dir.glob("*.json")
    )
    run_info = {
        "engine": args.engine,
        "engine_note": args.engine_note,
        "engine_probe": probe,
        "mode": "serving" if serving else "engine",
        "corpus_id": corpus["id"],
        "corpus_file": str(corpus_path),
        "corpus_sha256": corpus_sha,
        "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "workers": workers,
        "n_requests": len(jobs),
        "n_ok": len(jobs) - len(failures),
        "n_failed": len(failures),
        "n_loops": len(records),
        "latency_p50_s": quant(latencies, 0.5),
        "latency_p95_s": quant(latencies, 0.95),
    }
    aggregates = aggregate(records)
    write_reports(out_dir, run_info, records, failures, aggregates)
    print(f"wrote {out_dir}/loops.jsonl, report.json, report.md", flush=True)
    return 0

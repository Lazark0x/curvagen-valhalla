"""PROTOTYPE (research/upstream-6449-use-curvature): read the engine's per-request stage
ledger (`thor.roundtrip_stage_timing`, plus this branch's A* label counters) for one run.

usage: python3 ledger.py <run_dir> <engine.log> <first_line> <last_line>

Shuffle-harness runs (cells.jsonl) are joined per serve by (seed, target distance); an
excluded serve that tops up from a plain run logs two lines and they are summed. Loopqual
runs (responses/) send no exclusions, so every ledger line is one request; (seed, target)
repeats across origins there, so those are read as a distribution, not joined.
"""
import json, re, sys
from pathlib import Path

LINE = re.compile(r"roundtrip timing: (.*)$")
KV = re.compile(r"(\w+)=([0-9.]+)")


def quant(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))] if s else float("nan")


def lines(log, a, b):
    out = []
    for ln in open(log, errors="replace").read().splitlines()[a:b]:
        m = LINE.search(ln)
        if m:
            out.append({k: float(v) for k, v in KV.findall(m.group(1))})
    return out


def show(name, recs, wall):
    lab = [r.get("astar_labels", 0) + r.get("astar_fb_labels", 0) for r in recs]
    per_call = [r.get("astar_labels", 0) / max(1, r.get("astar_calls", 0)) for r in recs]
    ams = [r.get("astar", 0) + r.get("astar_fb", 0) for r in recs]
    hv = [r.get("harvest", 0) for r in recs]
    print(f"  {name}: n={len(recs)} A* labels/serve p50 {quant(lab, .5):.0f} p90 {quant(lab, .9):.0f}"
          f" | labels/call p50 {quant(per_call, .5):.0f} p90 {quant(per_call, .9):.0f}"
          f" | A* ms p50 {quant(ams, .5):.0f} p90 {quant(ams, .9):.0f}"
          f" | harvest ms p50 {quant(hv, .5):.0f} | wall s p50 {quant(wall, .5):.2f} p90 {quant(wall, .9):.2f}")


def main():
    run, log, a, b = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    L = lines(log, a, b)
    if (run / "cells.jsonl").exists():
        per = {}
        for kv in L:
            acc = per.setdefault((int(kv["seed"]), int(kv["target"])), {})
            for k, v in kv.items():
                acc[k] = v if k in ("seed", "target") else acc.get(k, 0.0) + v
        rows = []
        for rec in map(json.loads, open(run / "cells.jsonl")):
            for s in rec["serves"]:
                rows.append({"c": rec["curviness"], "wall": s["latency_s"],
                             "led": per.get((s["seed"], int(s["req_distance_m"])))})
        got = [r for r in rows if r["led"]]
        print(f"{run.name}: {len(got)}/{len(rows)} serves joined")
        for c in (None, 0.5, 1.0):
            R = [r for r in got if c is None or r["c"] == c]
            show(f"c={c}", [r["led"] for r in R], [r["wall"] for r in R])
    else:
        wall = []
        for fn in sorted((run / "responses").glob("*.json")):
            meta = json.loads(fn.read_text())["meta"]
            if meta.get("status") == 200:
                wall.append(meta["latency_s"])
        print(f"{run.name}: {len(L)} ledger lines, {len(wall)} responses")
        show("all", L, wall)


if __name__ == "__main__":
    main()

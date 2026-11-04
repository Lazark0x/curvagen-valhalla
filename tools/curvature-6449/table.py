"""Markdown table of run summaries vs a baseline run, with the budget verdicts.
usage: table.py base_dir dir...   (copied from research/shuffle-variety)

Budget (user, 2026-11-04): p90 distance error < 0.30. Gate v2 meters adapted to this
corpus (K=3, 6 Shuffles, the served surface is every loop served):
  R1  Retrace family share <= today's + 1 pp on this corpus (the 5 % absolute is already
      exceeded by today's engine here: 6.2 %)
  R2  spike_ge_500m == 0
  R4  every serve returns 3 loops
  T1  curviness retention >= 0.95x today, per curviness level
  T2  served distinctness mean <= 1.10x today and near-dup >0.6 <= today + 5 pp
Latency (T3 <= 1.10x p50) is read from the dedicated bracket session, not here.
"""
import json, sys
from pathlib import Path


def load(d):
    return json.loads(Path(d, "summary.json").read_text())


def verdict(b, s):
    fails = []
    if not s["derr_p90"] < 0.30:
        fails.append("p90")
    if s["retrace_pct"] > b["retrace_pct"] + 1.0:  # +1 pp = 6 loops of 648, the noise band
        fails.append("R1")
    if s["spike500"]:
        fails.append("R2")
    if s["fill_pct"] < 100.0:
        fails.append("R4")
    for k, lvl in (("curv_mean_c05", "0.5"), ("curv_mean_c10", "1.0")):
        if s[k] < 0.95 * b[k]:
            fails.append(f"T1c{lvl}")
    if s["t2_mean"] > 1.10 * b["t2_mean"] or s["t2_gt06_pct"] > b["t2_gt06_pct"] + 5.0:
        fails.append("T2")
    return "PASS" if not fails else "fail: " + ",".join(fails)


if __name__ == "__main__":
    base = load(sys.argv[1])
    cols = [("repeat_pct", "repeats ≥0.9 %", 1), ("repeat08_pct", "≥0.8 %", 1),
            ("distinct_per_cell", "distinct /18", 1), ("fill_pct", "fill %", 1),
            ("curv_mean_c05", "curv c0.5", 0), ("curv_mean_c10", "curv c1.0", 0),
            ("retrace_pct", "R1 %", 1), ("am25_new_m", "near-mirror m", 0),
            ("derr_mean", "dist err mean", 3), ("derr_p90", "p90", 3),
            ("fallback_pct", "fallback %", 1), ("rescue_pct", "rescue %", 1),
            ("topup_pct", "top-up %", 1), ("t2_mean", "T2 mean", 3),
            ("lat_p50", "p50 s*", 3)]
    print("| run | " + " | ".join(c[1] for c in cols) + " | budget |")
    print("|---|" + "---|" * (len(cols) + 1))
    for d in sys.argv[1:]:
        s = load(d)
        cells = [f"{s[k]:.{nd}f}" for k, _, nd in cols]
        print(f"| {Path(d).name} | " + " | ".join(cells) + f" | {verdict(base, s)} |")
        if s.get("underfilled_cells"):
            print(f"|  ↳ under-filled {s['underfilled_cells']} |")

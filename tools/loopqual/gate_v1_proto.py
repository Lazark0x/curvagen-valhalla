#!/usr/bin/env python3
"""Gate v1.2 checker (originally the #50 prototype instrument).

Gate v1.2 thresholds (2026-07-14): locked to the Andrey-approved T8 done-run
(curvagen #61/#62, engine curvagen-valhalla:t6 @ 9ca37c5f3) with a small
ratchet above the approved values — the gate's job is now to catch any
regression from the ACCEPTED quality, not to relitigate it. The v1-era
absolute thresholds were calibrated on v1 meter semantics in #49; the
approval resolves the T7 expected-red bounce for gates 3-7.

Gate v1 thresholds live in curvagen issue #49 (and the coming v3 ADR); they are
deliberately NOT part of the loopqual harness (`compare` stays a neutral meter).
This script is the #50 prototype's own instrument: it reads two run dirs
(baseline, candidate) and prints a PASS/FAIL table per Gate v1.

Usage: python3 gate_v1_proto.py <baseline_dir> <candidate_dir>
"""

import json
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path


def load(run_dir):
    d = Path(run_dir)
    report = json.loads((d / "report.json").read_text())
    loops = [json.loads(l) for l in (d / "loops.jsonl").read_text().splitlines() if l.strip()]
    return report, loops


def quant(vals, q):
    if not vals:
        return 0.0
    vals = sorted(vals)
    return vals[min(len(vals) - 1, int(q * len(vals)))]


def by_curviness(loops):
    out = defaultdict(list)
    for l in loops:
        out[float(l["curviness"])].append(l)
    return out


def main(base_dir, cand_dir):
    b_report, b_loops = load(base_dir)
    c_report, c_loops = load(cand_dir)
    rows = []  # (gate, level, threshold, baseline, candidate, ok)

    def row(gate, level, thr, base, cand, ok):
        rows.append((gate, level, thr, base, cand, "PASS" if ok else "FAIL"))

    b_by_c, c_by_c = by_curviness(b_loops), by_curviness(c_loops)
    for c in sorted(c_by_c):
        bl, cl = b_by_c.get(c, []), c_by_c[c]
        lvl = f"c{c}"
        n = len(cl)

        # 1/2 spikes
        c500 = sum(1 for l in cl if l["spike_ge_500m"]) / n
        b500 = sum(1 for l in bl if l["spike_ge_500m"]) / len(bl) if bl else 0
        row("1 spike_ge_500m == 0", lvl, "= 0", f"{b500:.1%}", f"{c500:.2%}", c500 == 0)
        c30 = sum(1 for l in cl if l["spike_ge_30m"]) / n
        b30 = sum(1 for l in bl if l["spike_ge_30m"]) / len(bl) if bl else 0
        row("2 spike_ge_30m <= 2%", lvl, "<= 2%", f"{b30:.1%}", f"{c30:.2%}", c30 <= 0.02)

        # 3/4 reuse banded means
        # v1.2 locked: approved run reads 0.111 @20, 0.091/0.088 @50, 0.046 @100,
        # 0.045 @200 c0.8 — ratcheted to 0.12 / 0.10 / 0.05.
        for gate_no, dists, thr in (("3", (20000,), 0.12), ("3", (50000,), 0.10),
                                    ("4", (100000, 200000, 300000), 0.05)):
            for dm in dists:
                cd = [l["edge_reuse_geom"] for l in cl if l["distance_m"] == dm]
                bd = [l["edge_reuse_geom"] for l in bl if l["distance_m"] == dm]
                if not cd:
                    continue
                m, bm = st.mean(cd), (st.mean(bd) if bd else 0)
                row(f"{gate_no} reuse mean @{dm//1000}km <= {thr}", lvl, f"<= {thr}",
                    f"{bm:.4f}", f"{m:.4f}", m <= thr)

        # 5 reuse tail
        ct = sum(1 for l in cl if l["edge_reuse_geom"] > 0.30) / n
        bt = sum(1 for l in bl if l["edge_reuse_geom"] > 0.30) / len(bl) if bl else 0
        # v1.2 locked: approved 5.9%/7.3% -> ratchet 8%.
        row("5 reuse>0.30 loops <= 8%", lvl, "<= 8%", f"{bt:.1%}", f"{ct:.2%}", ct <= 0.08)

        # 6 lollipop overall + worst cell
        clf = sum(1 for l in cl if l["is_lollipop"]) / n
        blf = sum(1 for l in bl if l["is_lollipop"]) / len(bl) if bl else 0
        # v1.2 locked: approved 1.80%/1.82% (vlasina forced-stem residual accepted
        # visually in T8) -> ratchet 2%.
        row("6a lollipop overall <= 2%", lvl, "<= 2%", f"{blf:.1%}", f"{clf:.2%}", clf <= 0.02)
        cells = defaultdict(list)
        for l in cl:
            cells[(l["origin"], l["distance_m"])].append(l["is_lollipop"])
        worst_cell, worst = max(
            ((k, sum(v) / len(v)) for k, v in cells.items()), key=lambda kv: kv[1])
        # v1.2 locked: worst cell = the accepted forced-stem class (approved at
        # 50%/20.8%) -> ratchet 55%.
        row(f"6b lollipop worst cell <= 55% ({worst_cell[0]}-{worst_cell[1]//1000}km)",
            lvl, "<= 55%", "-", f"{worst:.1%}", worst <= 0.55)

        # 7 distance_error
        cde = [l["distance_error"] for l in cl]
        bde = [l["distance_error"] for l in bl]
        # v1.2 locked, per-level honesty (the recorded ADR clause; c0.8 fails the
        # 0.20 bar at BASELINE): approved run reads mean 0.214/0.312, p90 0.390/0.630
        # -> ratchets 0.22/0.32 and 0.42/0.65.
        mean_thr, p90_thr = (0.22, 0.42) if c <= 0.5 else (0.32, 0.65)
        row(f"7 dist_err mean <= {mean_thr}", lvl, f"<= {mean_thr}",
            f"{st.mean(bde):.4f}" if bde else "-", f"{st.mean(cde):.4f}",
            st.mean(cde) <= mean_thr)
        row(f"7 dist_err p90 <= {p90_thr}", lvl, f"<= {p90_thr}",
            f"{quant(bde, 0.9):.4f}" if bde else "-", f"{quant(cde, 0.9):.4f}",
            quant(cde, 0.9) <= p90_thr)

        # 8 curviness held
        ccv = st.mean([l["curviness_geom_clean"] for l in cl])
        bcv = st.mean([l["curviness_geom_clean"] for l in bl]) if bl else 0.0
        ok = bcv == 0 or ccv >= 0.95 * bcv
        row("8 curviness_geom_clean >= 0.95x base", lvl, ">= 0.95x",
            f"{bcv:.1f}", f"{ccv:.1f} ({ccv / bcv:.3f}x)" if bcv else f"{ccv:.1f}", ok)

    # 8b ordering guard — ADVISORY since Gate v1.2 (ADR-0037 §3): the baseline
    # itself fails it, so it cannot gate a candidate. Whether the curviness
    # knob's upper range buys the rider anything is a costing-calibration
    # question, outside the loop-shape scope. Printed, never counted.
    if 0.5 in c_by_c and 0.8 in c_by_c:
        m05 = st.mean([l["curviness_geom_clean"] for l in c_by_c[0.5]])
        m08 = st.mean([l["curviness_geom_clean"] for l in c_by_c[0.8]])
        rows.append(("8b ordering c0.8 > c0.5 (advisory)", "all", ">",
                     "", f"{m08:.1f} vs {m05:.1f}",
                     "ADVISORY-PASS" if m08 > m05 else "ADVISORY-MISS"))

    # 9 latency (rig ratio)
    for q in ("latency_p50_s", "latency_p95_s"):
        b, cnd = b_report["run"].get(q), c_report["run"].get(q)
        if b and cnd:
            row(f"9 {q} <= 1.10x base", "all", "<= 1.10x", f"{b:.2f}s",
                f"{cnd:.2f}s ({cnd / b:.2f}x)", cnd <= 1.10 * b)

    # 10 failures
    bf, cf = len(b_report.get("failures", [])), len(c_report.get("failures", []))
    row("10 failures (done-run: 0; interim <= base)", "all", "0 / <=14",
        str(bf), str(cf), cf <= bf)

    w = max(len(r[0]) for r in rows)
    print(f"{'GATE':<{w}}  {'lvl':<5} {'baseline':>14} {'candidate':>20}  verdict")
    fails = 0
    for g, lvl, thr, b, cnd, v in rows:
        fails += v == "FAIL"
        print(f"{g:<{w}}  {lvl:<5} {b:>14} {cnd:>20}  {v}")
    print(f"\n{'GATE v1: ALL PASS' if fails == 0 else f'GATE v1: {fails} FAIL(s)'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))

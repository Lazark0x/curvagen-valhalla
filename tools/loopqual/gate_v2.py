#!/usr/bin/env python3
"""Gate v2 — the loop-quality contract round-trip v4 must meet (ADR-0041,
as amended 2026-09-07).

Three tiers, judged on the **Served Surface** (slots 0-5 of blocks A+B, the
loops a Rider actually reads) with both engines at the production configuration:

  reject tier   R1-R5  absolutes calibrated on the Rider-Verdict Calibration Set
  ratchets      T1-T6  vs Baseline v2 — what a Rider cannot judge per loop
  canaries      C1-C2  reported; a move here is a regression signal
  advisory             reported, never blocking (rings, stem, block C, c0.8,
                       long asks, Bank Distinctness, the per-leg split,
                       provenance)

Two baselines, because two different things are being held:

  Baseline v2   the census run (`b4f514d7f`, corpus-v2) — what the absolutes and
                the quality ratchets (T1, T5, T6) are pinned to (ADR-0041 §2).
  prod bracket  a same-session run of the production configuration
                (`roundtrip_xcand_penalty` 0.2 cap 4) — what the configuration-
                sensitive ratchets (T2, T3, T4) must be compared against, since
                distinctness and latency both move with that knob and a
                cross-session latency read is noise (±12 % p50 at workers=1).

usage:
  gate_v2.py <baseline_v2_dir> <candidate_dir> [--bracket DIR] [--cutover] [--json FILE]

  --bracket   the prod-config, same-session baseline for T2/T3/T4; repeat it to
              pool several brackets (the P2.1 session ran one before and one
              after the variants and read T3 against their pooled p50 — a
              single bracket carries the session's ±12 % latency drift).
              Omitted, T2/T3/T4 fall back to Baseline v2 and the table says so.

  --cutover   apply the amendment's v4-cutover exception on T3 (<= 1.25x
              instead of <= 1.10x; T4 unchanged). The exception is a cutover
              exception, not the new normal — the next revision re-pins the
              baseline to v4's own reading (Baseline v3) and returns T3 to
              1.10x against it.

Exit code 0 when every blocking row passes, 1 otherwise, 2 on bad input.
Both runs must be read with the SAME metrics version (v2): the switchback-aware
near-mirror read and the fixed ring counter moved the numbers, so a v1.3
loops.jsonl is not comparable. Re-read a saved run with `loopqual reanalyze`.
"""

import argparse
import json
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import metrics  # noqa: E402
from metrics import SERVED_SLOTS  # noqa: E402

# ADR-0041 §2 surfaces. Block A = the Vračar field cell (Andrey's cell, where
# all four defect shapes bite); block B = the production demand cells (named
# after their 0.001° cell); block C = corpus-v1 verbatim, motorways avoided —
# reported, never blocking (no rider asks for it).
BLOCK_A = {"vracar", "vracar_amw"}
BLOCK_B_PREFIX = "d44"
GATED_LEVELS = (0.5, 0.7, 1.0)
LONG_ASK_M = 200_000

BARS = {
    "R1_retrace_family_pct": 5.0,
    "R3": {0.5: (0.22, 0.42), "other": (0.32, 0.65)},
    "T1_retention": 0.95,
    "T2_mean_ratio": 1.10,
    "T2_near_dup_pp": 5.0,
    "T3_p50_ratio": 1.10,
    "T3_p50_ratio_cutover": 1.25,
    "T4_p95_ratio": 1.30,
    "T5_unseen_ratio": 0.25,
}


def block_of(origin: str) -> str:
    if origin in BLOCK_A:
        return "A"
    return "B" if origin.startswith(BLOCK_B_PREFIX) else "C"


def quant(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * len(s)))] if s else float("nan")


def pct(part, whole):
    return 100.0 * part / whole if whole else float("nan")


class Run:
    """One loopqual run dir, sliced into the surfaces Gate v2 judges."""

    def __init__(self, run_dir: str):
        d = Path(run_dir)
        self.dir = d
        report_path = d / "report.json"
        loops_path = d / "loops.jsonl"
        if not report_path.exists() or not loops_path.exists():
            raise SystemExit(f"{d}: not a loopqual run dir (report.json / loops.jsonl missing)")
        self.report = json.loads(report_path.read_text())
        self.metrics_version = self.report.get("metrics")
        self.loops = [json.loads(x) for x in loops_path.read_text().splitlines() if x.strip()]
        for l in self.loops:
            l["block"] = block_of(l["origin"])
        self.run = self.report["run"]
        self.served = [l for l in self.loops if l["block"] in "AB" and l["slot"] < SERVED_SLOTS]
        self.deep = [l for l in self.loops if l["block"] in "AB" and l["slot"] >= SERVED_SLOTS]
        self.latency = self._latencies()

    def _latencies(self):
        """Per-request wall latency by block, from the saved response metas."""
        out = {"A": [], "B": [], "C": [], "long": []}
        for fn in sorted((self.dir / "responses").glob("*.json")):
            meta = json.loads(fn.read_text()).get("meta", {})
            if "latency_s" not in meta or meta.get("status") != 200:
                continue
            b = block_of(meta.get("origin", ""))
            out[b].append(meta["latency_s"])
            if float(meta.get("distance_m", 0)) >= LONG_ASK_M:
                out["long"].append(meta["latency_s"])
        return out

    @property
    def latency_ab(self):
        return self.latency["A"] + self.latency["B"]

    def by_level(self, loops, c):
        return [l for l in loops if float(l["curviness"]) == c]

    # --- meters ---------------------------------------------------------
    def retrace_share(self, loops):
        return pct(sum(1 for l in loops if l["retrace_family"]), len(loops))

    def bank_size(self):
        n = {}
        for l in self.loops:
            n[l["file"]] = n.get(l["file"], 0) + 1
        return n

    def t2(self):
        """Served-Surface Distinctness (ADR-0041 amendment 1): each served loop
        against the OTHER served loops of its bank."""
        v = [l["max_pair_overlap_served"] for l in self.served
             if l.get("max_pair_overlap_served") is not None]
        return (st.mean(v) if v else float("nan"),
                pct(sum(1 for x in v if x > 0.6), len(v)), len(v))

    def bank_distinctness(self):
        """The original whole-bank read — advisory since the amendment."""
        v = [l["max_pair_overlap"] for l in self.served]
        return (st.mean(v) if v else float("nan"), pct(sum(1 for x in v if x > 0.6), len(v)))

    def t5(self):
        return st.mean([l["am25_new_m"] for l in self.served]) if self.served else float("nan")


def pooled(runs, key):
    out = []
    for r in runs:
        out += r.latency[key] if key != "AB" else r.latency_ab
    return out


def rows_for(base: Run, cand: Run, brackets, cutover: bool):
    bracket = brackets[0]
    """-> list of dicts: tier, id, bar, baseline, candidate, verdict."""
    R = []

    def row(tier, rid, name, bar, b, c, ok=None):
        R.append({"tier": tier, "id": rid, "name": name, "bar": bar,
                  "baseline": b, "candidate": c,
                  "verdict": ("—" if ok is None else ("PASS" if ok else "FAIL"))})

    # --- reject tier (absolutes on the candidate) ------------------------
    r1_b, r1_c = base.retrace_share(base.served), cand.retrace_share(cand.served)
    row("reject", "R1", "Retrace family, served (D1 ∨ D4 ≥ 500 m ∨ D1L ≥ 1.5 km)",
        f"≤ {BARS['R1_retrace_family_pct']:.0f} %", f"{r1_b:.1f} %", f"{r1_c:.1f} %",
        r1_c <= BARS["R1_retrace_family_pct"])
    parts_b = "/".join(f"{pct(sum(1 for l in base.served if l[k]), len(base.served)):.1f}"
                       for k in ("retrace_d1", "retrace_d4", "retrace_d1l"))
    parts_c = "/".join(f"{pct(sum(1 for l in cand.served if l[k]), len(cand.served)):.1f}"
                       for k in ("retrace_d1", "retrace_d4", "retrace_d1l"))
    row("reject", "R1·", "  by part D1/D4/D1L %", "—", parts_b, parts_c)

    sp_b = sum(1 for l in base.loops if l["spike_ge_500m"])
    sp_c = sum(1 for l in cand.loops if l["spike_ge_500m"])
    row("reject", "R2", "exact-mirror seam stub (spike_ge_500m), all loops", "0 at every level",
        sp_b, sp_c, sp_c == 0)

    r3_ok = True
    for c in GATED_LEVELS:
        bar_mean, bar_p90 = BARS["R3"][0.5] if c == 0.5 else BARS["R3"]["other"]
        de_b = [l["distance_error"] for l in base.by_level(base.loops, c)]
        de_c = [l["distance_error"] for l in cand.by_level(cand.loops, c)]
        ok = st.mean(de_c) <= bar_mean and quant(de_c, 0.9) <= bar_p90
        r3_ok &= ok
        row("reject", f"R3 c{c}", "distance error mean / p90",
            f"≤ {bar_mean} / {bar_p90}",
            f"{st.mean(de_b):.3f} / {quant(de_b, 0.9):.3f}",
            f"{st.mean(de_c):.3f} / {quant(de_c, 0.9):.3f}", ok)

    banks_b, banks_c = base.bank_size(), cand.bank_size()
    k = max(banks_c.values()) if banks_c else 0
    full_b = sum(1 for v in banks_b.values() if v >= k)
    full_c = sum(1 for v in banks_c.values() if v >= k)
    row("reject", "R4", f"fills (K = {k} on every request)", f"{cand.run['n_requests']}/{cand.run['n_requests']}",
        f"{full_b}/{base.run['n_requests']}", f"{full_c}/{cand.run['n_requests']}",
        full_c == cand.run["n_requests"])
    row("reject", "R5", "failed requests", "0", base.run["n_failed"], cand.run["n_failed"],
        cand.run["n_failed"] == 0)

    # --- ratchets vs Baseline v2 -----------------------------------------
    for c in GATED_LEVELS:
        mb = st.mean([l["curviness_geom_clean"] for l in base.by_level(base.loops, c)])
        mc = st.mean([l["curviness_geom_clean"] for l in cand.by_level(cand.loops, c)])
        ratio = mc / mb if mb else float("nan")
        row("ratchet", f"T1 c{c}", "curviness retention (curviness_geom_clean)",
            f"≥ {BARS['T1_retention']}×", f"{mb:.3f}", f"{mc:.3f} ({ratio:.3f}×)",
            ratio >= BARS["T1_retention"])

    (mb, ndb, nb), (mc, ndc, nc) = bracket.t2(), cand.t2()
    bar_mean = mb * BARS["T2_mean_ratio"]
    bar_nd = ndb + BARS["T2_near_dup_pp"]
    ok = mc <= bar_mean and ndc <= bar_nd
    row("ratchet", "T2", "Served-Surface Distinctness — mean / near-dup > 0.6 (slots 0–5 vs 0–5)",
        f"≤ {bar_mean:.4f} / ≤ {bar_nd:.1f} %",
        f"{mb:.4f} / {ndb:.1f} % (n={nb})", f"{mc:.4f} / {ndc:.1f} % (n={nc})", ok)

    all_b = pooled(brackets, "A") + pooled(brackets, "B") + pooled(brackets, "C")
    all_c = cand.latency["A"] + cand.latency["B"] + cand.latency["C"]
    p50_b, p50_c = quant(all_b, 0.5), quant(all_c, 0.5)
    ratio = p50_c / p50_b if p50_b else float("nan")
    bar = BARS["T3_p50_ratio_cutover"] if cutover else BARS["T3_p50_ratio"]
    row("ratchet", "T3", "wall p50, pooled over the corpus (same-session brackets)",
        f"≤ {bar:.2f}×" + (" (v4 cutover exception)" if cutover else ""),
        f"{p50_b:.3f} s ({len(brackets)} bracket{'s' if len(brackets) > 1 else ''})",
        f"{p50_c:.3f} s ({ratio:.3f}×)", ratio <= bar)

    p95_b, p95_c = quant(pooled(brackets, "AB"), 0.95), quant(cand.latency_ab, 0.95)
    ratio = p95_c / p95_b if p95_b else float("nan")
    row("ratchet", "T4", "wall p95 on blocks A+B", f"≤ {BARS['T4_p95_ratio']:.2f}×",
        f"{p95_b:.3f} s", f"{p95_c:.3f} s ({ratio:.3f}×)", ratio <= BARS["T4_p95_ratio"])

    t5_b, t5_c = base.t5(), cand.t5()
    bar = BARS["T5_unseen_ratio"] * t5_b
    row("ratchet", "T5", "near-mirror magnitude — mean unseen metres, Served Surface",
        f"≤ {BARS['T5_unseen_ratio']}× ({bar:.0f} m)", f"{t5_b:.0f} m", f"{t5_c:.0f} m",
        t5_c <= bar)

    t6_b, t6_c = base.retrace_share(base.deep), cand.retrace_share(cand.deep)
    row("ratchet", "T6", "deep bank (slots 6–11): Retrace family share",
        f"≤ {t6_b:.1f} % (Baseline v2)", f"{t6_b:.1f} %", f"{t6_c:.1f} %", t6_c <= t6_b)

    # --- canaries ---------------------------------------------------------
    c1_b = pct(sum(1 for l in base.loops if l["spike_ge_30m"]), len(base.loops))
    c1_c = pct(sum(1 for l in cand.loops if l["spike_ge_30m"]), len(cand.loops))
    row("canary", "C1", "spike_ge_30m share (all loops)", "report", f"{c1_b:.1f} %", f"{c1_c:.1f} %")
    row("canary", "C2", "edge_reuse_geom mean (served)", "report",
        f"{st.mean([l['edge_reuse_geom'] for l in base.served]):.4f}",
        f"{st.mean([l['edge_reuse_geom'] for l in cand.served]):.4f}")

    # --- advisory ---------------------------------------------------------
    (bd_mb, bd_ndb), (bd_mc, bd_ndc) = bracket.bank_distinctness(), cand.bank_distinctness()
    row("advisory", "A1", "Bank Distinctness (served vs the whole bank — T2's original read)",
        "report", f"{bd_mb:.4f} / {bd_ndb:.1f} %", f"{bd_mc:.4f} / {bd_ndc:.1f} %")
    row("advisory", "A2", "per-leg split of the served overlap (forward / return)", "report",
        f"{st.mean([l['pair_overlap_fwd'] for l in bracket.served]):.3f} / "
        f"{st.mean([l['pair_overlap_ret'] for l in bracket.served]):.3f}",
        f"{st.mean([l['pair_overlap_fwd'] for l in cand.served]):.3f} / "
        f"{st.mean([l['pair_overlap_ret'] for l in cand.served]):.3f}")
    row("advisory", "A3", "Rings (D3): share with ≥ 1 ring / mean largest ring (served)", "report",
        f"{pct(sum(1 for l in base.served if l['ring_n']), len(base.served)):.1f} % / "
        f"{st.mean([l['ring_max_m'] for l in base.served]):.0f} m",
        f"{pct(sum(1 for l in cand.served if l['ring_n']), len(cand.served)):.1f} % / "
        f"{st.mean([l['ring_max_m'] for l in cand.served]):.0f} m")
    row("advisory", "A4", "self-crossings (D3b): share with ≥ 1 (served)", "report",
        f"{pct(sum(1 for l in base.served if l['xing_n']), len(base.served)):.1f} %",
        f"{pct(sum(1 for l in cand.served if l['xing_n']), len(cand.served)):.1f} %")
    row("advisory", "A5", "stem-lollipop (v1.3 rows 6a/6b): is_lollipop share / mean stem frac",
        "report",
        f"{pct(sum(1 for l in base.served if l['is_lollipop']), len(base.served)):.1f} % / "
        f"{st.mean([l['lollipop_stem_fraction'] for l in base.served]):.4f}",
        f"{pct(sum(1 for l in cand.served if l['is_lollipop']), len(cand.served)):.1f} % / "
        f"{st.mean([l['lollipop_stem_fraction'] for l in cand.served]):.4f}")
    row("advisory", "A6", "shadow_frac_loop mean (served)", "report",
        f"{st.mean([l['shadow_frac_loop'] for l in base.served]):.4f}",
        f"{st.mean([l['shadow_frac_loop'] for l in cand.served]):.4f}")
    for b in ("A", "B", "C"):
        sb = [l for l in base.loops if l["block"] == b and l["slot"] < SERVED_SLOTS]
        sc = [l for l in cand.loops if l["block"] == b and l["slot"] < SERVED_SLOTS]
        if not sc:
            continue
        row("advisory", f"A7 {b}", f"block {b}: Retrace family / D1b ≥ 500 m unseen (served slots)",
            "report",
            f"{base.retrace_share(sb):.1f} % / {pct(sum(1 for l in sb if l['am25_new_m'] >= 500), len(sb)):.1f} %",
            f"{cand.retrace_share(sc):.1f} % / {pct(sum(1 for l in sc if l['am25_new_m'] >= 500), len(sc)):.1f} %")
    for c in (0.8,):
        lb = base.by_level(base.loops, c)
        lc = cand.by_level(cand.loops, c)
        if not lc:
            continue
        mb_, mc_ = (st.mean([l["curviness_geom_clean"] for l in lb]),
                    st.mean([l["curviness_geom_clean"] for l in lc]))
        row("advisory", f"A8 c{c}", "c0.8 (no rider asks for it): curviness / Retrace family",
            "report", f"{mb_:.3f} / {base.retrace_share(lb):.1f} %",
            f"{mc_:.3f} ({mc_ / mb_:.3f}×) / {cand.retrace_share(lc):.1f} %")
    if cand.latency["long"]:
        row("advisory", "A9", f"long asks (≥ {LONG_ASK_M // 1000} km): wall p95", "report",
            f"{quant(base.latency['long'], 0.95):.3f} s",
            f"{quant(cand.latency['long'], 0.95):.3f} s")
    prov = [l for l in cand.served if l.get("prov_builder")]
    if prov:
        mix = {}
        for l in prov:
            mix[l["prov_builder"]] = mix.get(l["prov_builder"], 0) + 1
        row("advisory", "A10", "provenance of served loops (builder mix)", "report",
            "n/a" if not any(l.get("prov_builder") for l in base.served) else
            ", ".join(f"{k} {pct(v, len(base.served)):.0f} %" for k, v in sorted(
                {l["prov_builder"]: sum(1 for x in base.served
                                        if x.get("prov_builder") == l["prov_builder"])
                 for l in base.served if l.get("prov_builder")}.items())),
            ", ".join(f"{k} {pct(v, len(prov)):.0f} %" for k, v in sorted(mix.items())))
        row("advisory", "A11", "served tier / gated share (the served-tier check)", "report",
            "n/a",
            f"tier>0 {pct(sum(1 for l in prov if (l.get('prov_tier') or 0) > 0), len(prov)):.1f} % · "
            f"gated {pct(sum(1 for l in prov if l.get('prov_gated')), len(prov)):.1f} % · "
            f"fallback rung>0 {pct(sum(1 for l in prov if (l.get('prov_rung') or 0) > 0), len(prov)):.1f} %")
    return R


def render(base: Run, cand: Run, brackets, rows, cutover: bool) -> str:
    bracket = brackets[0]
    out = [
        "# Gate v2 — ADR-0041 (amended 2026-09-07)",
        "",
        f"- Baseline v2: `{base.dir}` — {base.run.get('engine_note') or base.run['engine']}",
        f"- prod bracket (T2/T3/T4): {', '.join(f'`{b.dir}`' for b in brackets)}"
        + ("" if bracket.dir != base.dir else "  ← **same as Baseline v2**: T2/T3/T4 are "
                                              "not same-session prod-config reads"),
        f"- candidate: `{cand.dir}` — {cand.run.get('engine_note') or cand.run['engine']}",
        f"- metrics: baseline `{base.metrics_version}`, candidate `{cand.metrics_version}`",
        f"- surface: blocks A+B, slots 0–{SERVED_SLOTS - 1} "
        f"(served {len(cand.served)} loops, deep {len(cand.deep)}); "
        f"levels {', '.join(f'c{c}' for c in GATED_LEVELS)}",
        f"- T3 bar: {'≤ 1.25× (v4 cutover exception)' if cutover else '≤ 1.10×'}",
        "",
        "| tier | # | bar | baseline | candidate | verdict |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        out.append(f"| {r['tier']} | {r['id']} {r['name']} | {r['bar']} | "
                   f"{r['baseline']} | {r['candidate']} | {r['verdict']} |")
    blocking = [r for r in rows if r["tier"] in ("reject", "ratchet") and r["verdict"] != "—"]
    failed = [r for r in blocking if r["verdict"] == "FAIL"]
    out += ["", f"**{'PASS' if not failed else 'FAIL'}** — "
            f"{len(blocking) - len(failed)}/{len(blocking)} blocking rows pass"
            + ("" if not failed else ": " + ", ".join(r["id"] for r in failed)), ""]
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gate_v2.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", help="Baseline v2 run dir (the census run, b4f514d7f)")
    ap.add_argument("candidate", help="candidate run dir (same corpus, prod config)")
    ap.add_argument("--bracket", action="append", default=None,
                    help="same-session prod-config baseline for T2/T3/T4; repeat to pool "
                         "(default: the Baseline v2 dir)")
    ap.add_argument("--cutover", action="store_true",
                    help="apply the amendment's T3 ≤ 1.25× v4-cutover exception")
    ap.add_argument("--json", dest="json_out", default=None, help="also write the rows as JSON")
    a = ap.parse_args(argv)

    base, cand = Run(a.baseline), Run(a.candidate)
    brackets = [Run(d) for d in a.bracket] if a.bracket else [base]
    for r in [base, cand] + brackets:
        if r.metrics_version != metrics.METRICS_VERSION:
            print(f"warning: {r.dir} was read with metrics {r.metrics_version}, "
                  f"this gate expects {metrics.METRICS_VERSION} — re-read it with "
                  f"`loopqual reanalyze --run {r.dir}`", file=sys.stderr)
    rows = rows_for(base, cand, brackets, a.cutover)
    print(render(base, cand, brackets, rows, a.cutover))
    if a.json_out:
        Path(a.json_out).write_text(json.dumps(
            {"baseline": str(base.dir), "candidate": str(cand.dir), "brackets": [str(b.dir) for b in brackets],
             "cutover": a.cutover, "metrics": metrics.METRICS_VERSION, "rows": rows}, indent=1))
    failed = [r for r in rows if r["tier"] in ("reject", "ratchet") and r["verdict"] == "FAIL"]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

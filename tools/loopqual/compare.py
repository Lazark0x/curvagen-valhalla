#!/usr/bin/env python3
"""loopqual compare — per-metric delta table between two report.json files.

Prints a markdown table (baseline, candidate, delta, delta %) for every
numeric leaf under `aggregates`, grouped by slice. NO thresholds and NO
pass/fail verdict live here — gate thresholds are ticket #49's decision.
Exit codes: 0 on success (whatever the deltas), 2 on parse/read errors.
"""

import json
from pathlib import Path


def flatten(node, prefix=""):
    """-> {dotted.path: numeric leaf} under an aggregates tree."""
    out = {}
    if isinstance(node, dict):
        for k, v in node.items():
            out.update(flatten(v, f"{prefix}.{k}" if prefix else k))
    elif isinstance(node, (int, float)) and not isinstance(node, bool):
        out[prefix] = node
    return out


def fmt(v):
    if v is None:
        return "—"
    if isinstance(v, float):
        return f"{v:.4g}"
    return str(v)


def compare(baseline_path: str, candidate_path: str) -> int:
    try:
        base = json.loads(Path(baseline_path).read_text())
        cand = json.loads(Path(candidate_path).read_text())
    except (OSError, ValueError) as e:
        print(f"loopqual compare: cannot read reports: {e}")
        return 2

    bm, cm = base.get("metrics"), cand.get("metrics")
    print(f"# loopqual compare — metrics {bm} vs {cm}")
    if bm != cm:
        print(f"\n> WARNING: metric spec versions differ ({bm} vs {cm}) — "
              "numbers are not directly comparable.")
    b_run, c_run = base.get("run", {}), cand.get("run", {})
    print(f"\n- baseline:  `{baseline_path}` — {b_run.get('engine', '?')} "
          f"({b_run.get('mode', '?')}), corpus {b_run.get('corpus_id', '?')}, "
          f"{b_run.get('date', '?')}")
    print(f"- candidate: `{candidate_path}` — {c_run.get('engine', '?')} "
          f"({c_run.get('mode', '?')}), corpus {c_run.get('corpus_id', '?')}, "
          f"{c_run.get('date', '?')}")

    fb = flatten(base.get("aggregates", {}))
    fc = flatten(cand.get("aggregates", {}))
    keys = sorted(set(fb) | set(fc))
    if not keys:
        print("\nloopqual compare: no aggregate metrics found in either report")
        return 2

    # group rows by slice prefix (everything up to the metric leaf name)
    groups = {}
    for key in keys:
        prefix, _, leaf = key.rpartition(".")
        groups.setdefault(prefix or "(top)", []).append((leaf, key))

    for prefix in sorted(groups):
        print(f"\n## {prefix}\n")
        print("| metric | baseline | candidate | Δ | Δ% |")
        print("|---|---|---|---|---|")
        for leaf, key in groups[prefix]:
            b, c = fb.get(key), fc.get(key)
            if b is None or c is None:
                print(f"| {leaf} | {fmt(b)} | {fmt(c)} | — | — |")
                continue
            d = c - b
            dp = f"{100.0 * d / abs(b):+.1f}%" if b != 0 else ("0.0%" if d == 0 else "n/a")
            print(f"| {leaf} | {fmt(b)} | {fmt(c)} | {fmt(d) if d else '0'} | {dp} |")
    return 0

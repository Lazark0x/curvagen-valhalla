"""PROTOTYPE (research/upstream-6449-use-curvature): paired reads between two harness runs.

  identity  A B   -> how many serves are byte-identical (every loop's leg shapes equal)
  returns   A B   -> F03 read: loops whose forward leg is identical in both runs and whose
                     return was built by route_leg in both; compares the return legs' costs
                     (same objective in modes 0/1, so a cheaper return in B is an optimality
                     loss of A's search). Only loops served before the two runs first diverge
                     in that cell count, so the Shuffle memory and the cross-candidate
                     surcharge are identical in both.
"""
import json, sys
from pathlib import Path


def load(d):
    return {(r["origin"], r["distance_km"], r["curviness"]): r
            for r in map(json.loads, open(Path(d) / "cells.jsonl"))}


def identity(a, b):
    A, B = load(a), load(b)
    same = tot = 0
    for k, ra in A.items():
        for sa, sb in zip(ra["serves"], B[k]["serves"]):
            tot += 1
            same += [l["legs"] for l in sa["loops"]] == [l["legs"] for l in sb["loops"]]
    print(f"byte-identical serves: {same}/{tot}")


def a_star_built(loop):
    p = loop["provenance"] or {}
    return p.get("builder") in ("route_leg", "rescue") or (p.get("builder") == "pair" and p.get("full_repair"))


def returns(a, b):
    A, B = load(a), load(b)
    rows = []
    for k, ra in A.items():
        # Walk the cell in serve order while everything served before is identical in both
        # runs: then the Shuffle memory and the cross-candidate surcharge are identical too,
        # and a loop with the same forward leg faced the same return-leg objective.
        diverged = False
        for sa, sb in zip(ra["serves"], B[k]["serves"]):
            for x, y in zip(sa["loops"], sb["loops"]):
                if diverged:
                    break
                if x["legs"][0] == y["legs"][0] and a_star_built(x) and a_star_built(y):
                    rows.append((k, x["leg_cost"][1], y["leg_cost"][1], x["legs"][1] == y["legs"][1]))
                if x["legs"] != y["legs"]:
                    diverged = True
            if len(sa["loops"]) != len(sb["loops"]):
                diverged = True
    report(rows)


def returns_lq(a, b):
    """The same read on two loopqual run dirs (K=12 banks): slots in bank order, up to the
    first slot whose loop differs (the cross-candidate surcharge depends on earlier slots)."""
    rows = []
    for fa in sorted(Path(a, "responses").glob("*.json")):
        fb = Path(b, "responses", fa.name)
        if not fb.exists():
            continue
        ra, rb = json.loads(fa.read_text())["response"], json.loads(fb.read_text())["response"]
        ta = [ra["trip"]] + [x["trip"] for x in ra.get("alternates") or []] if ra and ra.get("trip") else []
        tb = [rb["trip"]] + [x["trip"] for x in rb.get("alternates") or []] if rb and rb.get("trip") else []
        for x, y in zip(ta, tb):
            sx, sy = [l["shape"] for l in x["legs"]], [l["shape"] for l in y["legs"]]
            px = {"provenance": x.get("provenance")}
            py = {"provenance": y.get("provenance")}
            if sx[0] == sy[0] and a_star_built(px) and a_star_built(py):
                rows.append((fa.stem, x["legs"][1]["summary"]["cost"], y["legs"][1]["summary"]["cost"], sx[1] == sy[1]))
            if sx != sy:
                break
    report(rows)


def report(rows):
    n = len(rows)
    same = sum(r[3] for r in rows)
    cheaper = [r for r in rows if r[2] < r[1] * 0.999]
    dearer = [r for r in rows if r[2] > r[1] * 1.001]
    print(f"comparable returns: {n}; identical return {same}; B cheaper {len(cheaper)}; B dearer {len(dearer)}")
    for k, ca, cb, _ in sorted(cheaper, key=lambda r: r[2] / r[1]):
        print(f"  {k}: A {ca:.0f} -> B {cb:.0f} ({100 * (cb / ca - 1):+.1f} %)")
    for k, ca, cb, _ in dearer:
        print(f"  dearer {k}: A {ca:.0f} -> B {cb:.0f} ({100 * (cb / ca - 1):+.1f} %)")


if __name__ == "__main__":
    {"identity": identity, "returns": returns, "returns-lq": returns_lq}[sys.argv[1]](sys.argv[2], sys.argv[3])

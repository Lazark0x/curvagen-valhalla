# B1: selection-time distinctness — built, measured, FALSIFIED

**Ticket:** [curvagen #85](https://github.com/Lazark0x/curvagen/issues/85) (wayfinder map #81, lever #3)
**Date:** 2026-07-16 · **Engine:** fork `curvature-costing` tip + D1 guard (image `valhalla-fork-test:b1`, uncommitted working-tree change) · **Rig:** loopqual corpus-v1 (232 req, workers 3), one binary for every arm; neutral control byte-identical to `c1-off-w3`.

## TL;DR — the selection route to the full win is dead

1. **The D1 mechanism was built** (knobs `thor.roundtrip_fwd_distinct` / `roundtrip_fwd_overlap_cap`, default off; sector-pick guard + consume-site screen + correction check) **and iterated through four variants** after the first two blew up operationally: naive per-candidate chain memoization **OOM-killed the engine** (d200/d300 bands hold tens of thousands of candidates), the streaming rewrite **burned un-ledgered CPU** (p50 1.33–1.40×) skip-walking those bands, a tight eval budget **relaxed wholesale** (guard neutered, 227/232 requests), and the final **window-select** (build the most-distinct of the next 16 live candidates) is cheap (≈2 ms, ~120 evals, no relax) — the only operationally sound shape.
2. **Structural falsification — forward-involved overlap is CONSERVED under selection pressure.** Selection does cut fwd-fwd sharing exactly as designed (ff 0.199 → 0.099 unbounded / 0.175 windowed), but mixed fwd-ret sharing inflates in compensation (fr+rf 0.127 → 0.19 / 0.14): **unpenalized returns chase the same good roads, refilling every corridor selection frees.** Net fwd-involved barely moves (0.326 → 0.309 best case). H1's "cut fwd-involved by ⅓" premise is unreachable by any selection form — the balloon squeezes, it doesn't shrink.
3. **Best shippable candidate misses the bar and breaks a gate.** Ladder (one binary, ratios vs same-binary OFF):

   | arm | overlap | ndup>.6 | p50× | p95× | dist_err mean | gates |
   |---|---|---|---|---|---|---|
   | OFF anchor | 0.5701 | 0.478 | 1.000 | 1.000 | 0.2195 | — |
   | penalty 0.2 (prod) | 0.5114 | 0.365 | 1.035 | 0.963 | **0.1706** | 2 marginal fails, **pre-existing** (reproduce exactly on historical `c1-s02` vs `c1-off`: reuse@20km 0.1220 vs ≤0.12, reuse>0.30 8.07 % vs ≤8 %) |
   | **window 0.35 + penalty 0.2** | **0.4992** | **0.331** | 1.051 | 0.937 | **0.2464** | **gate 7 RED ×4** (dist_err mean 0.234 > 0.22 & p90 0.49 > 0.42 @ c0.5; 0.324 > 0.32 & 0.727 > 0.65 @ c0.8) |
   | window 0.35 alone | 0.5563 | 0.454 | 0.883 | 0.865 | 0.2943 | gate 7 red |

   The combo beats prod's overlap by only **−0.012** (0.4992 vs 0.5114) and pays for it by destroying the penalty's distance-targeting improvement (0.171 → 0.246): window swaps ignore path-distance fit, and the 8-correction budget can't repair it. Ship bar (overlap ≤ ~0.47) missed; full win 0.463 not approached; hypothesis "~1.0× full win" falsified.
4. **Verdict: NO SHIP.** Prod stays penalty-0.2. The knobs remain in the fork default-false — **zero cost off, proven**: the OFF arm is byte-identical (id/slot/total_m over 2 780 loops) to pre-change `c1-off-w3`. Contended spot-check skipped — nothing to ship.

## Why this kills the lever-3 premise, not just this prototype

- **Family (a)** measured dead above — conservation + gate-7 damage at every cap (0.25/0.35/0.50) and both enforcement shapes.
- **Family (b)** (upstream diversification) faces the same conservation: diversifying *forward* corridors cannot stop *returns* from re-piling onto freed roads — only return-leg pressure does that, and that IS the penalty (X1 proved its form optimal).
- **Family (c)** (over-build + select) = paying return legs to then discard loops — the latency the ratchet forbids; its cheap variant (item-4 tightening) rejects *after* the A\* spend.
- The penalty is therefore **not an implementation detail of distinctness — it is the mechanism**: distinctness must be enforced where the sharing happens (the return search), and P1/A1/A2/X1 already established that pressure costs what it costs (0.2 → 1.069×, 0.5 → 1.22×). **The full win (0.463) stays gated on latency headroom: hardware, or a lower bank-K, or accepting >1.10×.**

## Ledger of variants (for the record)

| build | mechanism | outcome |
|---|---|---|
| v1 | hard cap, chain memoization | **OOM (exit 137)** — ~500 MB/request memo on d200/d300 bands × 3 workers |
| v2 | hard cap, streaming walks, unbounded | true mechanism ceiling: overlap 0.508–0.513 selection-only, but p50 1.08–1.40× of un-ledgered walk CPU (30k+ evals/request) |
| v3 | + eval budget 16×K, shared with correction scan | budget eaten by correction scans → relaxed 232/232, guard neutered (0.55–0.57) |
| v4 | correction checks winner-only, budget 256×K | cheap (~10 ms) but 99.7 % of evaluated candidates over-cap → wholesale relax on 119–227/232, overlap 0.52–0.55 |
| v5 (final) | **window-select 16, short-circuit at cap** | ~2 ms, no relax; selection-only −0.015; combo 0.4992 gate-7-red |

Durable operational lesson: **pre-A\* screens over harvest bands must be O(window), never O(band)** — the band is 100–1000× the bank.

## Assets

- Code: fork working tree (uncommitted) — `src/thor/route_action.cc` (guard machinery + window-select + ledger fields `fwd_guard/fwd_evals/fwd_skips`), `valhalla/thor/worker.h`, `src/thor/worker.cc`. Image `valhalla-fork-test:b1`.
- Runs: `tools/loopqual/results/b1-{off,s025,s035,s050,win02,x02}` (+ v2/v4 sweeps overwritten in place; wall-times and ledgers in `/tmp/b1-*`).
- Analysis: `~/.curvagen-scratch/b1/b1_analyze.py` (ladder + H1 decomposition), gate runs via `gate_v1_proto.py`.

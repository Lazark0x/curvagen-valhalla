# 0040 — Selection-time distinctness rejected; the full distinctness win remains latency-gated

- **Status:** Accepted (2026-07-16)
- **Context:** [Wayfinder map #81](https://github.com/Lazark0x/curvagen/issues/81) — lever #3 of ADR-0039's headroom ledger: fold cross-candidate distinctness into harvest/bucketing *selection* so the full win (bank overlap 0.570 → 0.463) lands without the return-leg penalty's latency. Successor record to **ADR-0038** (xcand mechanism + gate 9) and **ADR-0039** (ships at strength 0.2) — **ADR-0039's decision is unchanged by this record.**

## Decision

1. **Selection-time distinctness is rejected** — the hypothesis is falsified by measurement, not by cost alone. Prod ship set stays exactly ADR-0039: **xcand penalty strength 0.2, default-ON, config-tunable** (overlap ~0.51, 1.069× p50 prod-validated).
2. **The B1 prototype code is reverted from the fork** (`curvature-costing` working tree; knobs `thor.roundtrip_fwd_distinct` / `roundtrip_fwd_overlap_cap` and the window-select machinery). Knowledge survives in the research assets below and the retained engine image `valhalla-fork-test:b1`; resurrection-if-ever reimplements from the D1 spec, better informed. Precedent: X1's hard-exclusion prototype.
3. **The capacity watch is closed** ([#80](https://github.com/Lazark0x/curvagen/issues/80)): burst latency is not user-visible (single-user serial traffic; miss p50 0.77 s / p95 2.45 s post-v3; K=12 fill is background single-flight, never on the serve path). Mitigation **none**. Reopen triggers on the issue's closing comment: sustained miss-mode p95 > ~5 s, any overlapping-miss 10 s window, or "Bank Fill queue full" drops. Metric of record: nginx origin `request_time` on `POST /round-trip` (≥ 0.1 s = miss).
4. **The full distinctness win (0.463) remains latency-gated.** Three routes remain, each out of scope of this effort and each a fresh effort if ever wanted: **hardware** (more cores buy the 1.22× penalty-0.5 cost under the ratchet), **lower bank-K** (shrinks return-leg count and the diffuse thor tissue, but is quality-coupled — bank fill/distinctness material — and needs its own measured effort), **relaxed ratchet** (a product decision, currently fixed at 1.10×).

## Why selection cannot do it — the conservation finding

Leg-side decomposition of gate-9 overlap (new in this effort; reproduces the metric of record to ±0.001) splits sharing into fwd-fwd (ff), mixed (fr+rf), and ret-ret (rr):

| arm | overlap | ff | fr+rf | rr |
|---|---|---|---|---|
| OFF | 0.5695 | 0.199 | 0.127 | 0.243 |
| penalty 0.5 (full win) | 0.4627 | 0.205 | 0.115 | **0.143** |
| selection unbounded (best) | 0.508–0.513 | **0.099–0.121** | 0.19–0.22 | 0.20–0.24 |
| window-select + penalty 0.2 (best combo) | 0.4992 | 0.175 | 0.137 | 0.187 |

- The penalty's entire win is **rr** (H1). Selection's target was the fwd-involved 0.326.
- Selection cuts **ff** exactly as designed — but **fr+rf inflate in compensation: unpenalized returns chase the same good roads, refilling every corridor selection frees.** Net fwd-involved barely moves (0.326 → 0.309 best case). The balloon squeezes; it does not shrink.
- The best shippable combo lands **0.4992 @ 1.051×** — only −0.012 vs prod — and **breaks gate 7** (dist_err mean 0.171 → 0.246; distance-blind window swaps destroy the penalty's targeting improvement). The ship bar (≤ ~0.47) is missed; the D1 hypothesis (~1.0× full win) is falsified.
- Families (b) upstream diversification and (c) over-generate+select die by the same logic: forward diversity cannot stop returns re-piling (only return-leg pressure does — and that *is* the penalty, whose form X1 proved optimal), and over-building pays the A\* the ratchet forbids.

**Conclusion: the return-leg penalty is not an implementation of distinctness — it is the mechanism.** Distinctness must be enforced where sharing happens: in the return search.

## Operational lessons (durable)

- **Pre-A\* screens over harvest bands must be O(window), never O(band)** — a d200/d300 band holds 10⁴–10⁵ candidates (100–1000× the bank); the naive per-candidate screen OOM-killed the engine (memoization) and then burned 1.33–1.40× un-ledgered CPU (streaming).
- Gate 3/5 marginal fails of the 0.2 config against a same-binary OFF anchor (reuse@20km 0.1220 vs ≤ 0.12; reuse>0.30 8.07 % vs ≤ 8 %) are **pre-existing** — they reproduce exactly on the historical C1 pair — not regressions of this work.

## The map (route walked)

H1 (audit + decomposition) → M1 (capacity metric + prod evidence) → D1 (mechanism design) → B1 (build + measure, four variants, **negative**) → C2 (capacity verdict: none) → S2 (this record). All tickets closed under [map #81](https://github.com/Lazark0x/curvagen/issues/81); backlog anchor [#79](https://github.com/Lazark0x/curvagen/issues/79) closes negative with this ADR.

## References

- `docs/research/2026-07-16-h1-harvest-bucketing-selection-audit.md` — pipeline map, hook points, the decomposition method, cost model.
- `docs/research/2026-07-16-b1-selection-distinctness-measurement.md` — the four-variant build ledger, full measurement ladder, gate runs.
- `docs/research/2026-07-16-m1-capacity-metric-prod-evidence.md` — capacity metric of record + prod evidence.
- ADR-0038 (mechanism + gate 9), ADR-0039 (ship at 0.2 + headroom ledger).

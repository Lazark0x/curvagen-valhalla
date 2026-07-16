# H1: harvest/bucketing/selection audit — distinctness hook points + cost model

**Ticket:** [curvagen #82](https://github.com/Lazark0x/curvagen/issues/82) (wayfinder map #81, lever #3)
**Date:** 2026-07-16 · **Code base:** fork `curvature-costing` @ `a2fd86343` (Valhalla 3.8.2)
**Inputs:** fork source, P1 stage ledger, gate-9 metric source, **new offline overlap decomposition** on saved C1 runs (no engine runs).

## TL;DR

1. **The pipeline over-generates candidates for free and pays only at build.** Turnaround candidates come from one shared forward expansion (24 ms p50, amortized over K); a candidate's cost before its return leg is **~3–4 ms**, the return leg is **~20–50 ms** (astar 45 % + astar_fb 11 % of request p50). Selection-time (pre-build) hooks are effectively free; over-*building* is what the ratchet punishes.
2. **Selection today is distinctness-blind on corridors.** It diversifies by bearing sector, node identity, and turnaround straight-line separation — but two candidates 40° apart can (and do) share their whole arterial trunk out of the start valley. Nothing looks at *which roads* a candidate's forward corridor rides until the post-build item-4 filter, which pays a full return leg before rejecting.
3. **NEW MEASUREMENT — the penalty's entire win is return-return sharing; the forward side is untouched and bigger than the whole full-win delta.** Decomposing gate-9 overlap by leg side on saved C1 runs (232 req × 12 loops, decomposition reproduces the metric of record to ±0.001):

   | run | bank_overlap | ff (fwd-fwd) | fr+rf (mixed) | rr (ret-ret) |
   |---|---|---|---|---|
   | OFF | **0.5695** | 0.1988 | 0.1273 | 0.2434 |
   | penalty 0.2 (prod) | 0.5108 | 0.2060 | 0.1200 | 0.1848 |
   | penalty 0.5 (full win) | **0.4627** | 0.2050 | 0.1148 | 0.1430 |

   Penalty 0.5 moves **only rr** (0.243→0.143, −0.100 of the −0.107 total); forward-involved sharing (ff+fr+rf ≈ **0.326**) is structurally unreachable by any return-leg mechanism — it is why the penalty's floor is ~0.46. **Selection-time distinctness attacks exactly that 0.326 pool.** Cutting it by ~⅓ matches the full win (−0.107) at zero return-leg cost; selection + the coexisting 0.2 penalty could plausibly land **below 0.463** (fwd −⅓ ⇒ ~0.22, plus rr@0.2 0.185 ⇒ ~0.40).
4. **The forced floor is small.** Hard common trunk (edges ridden by ≥75 % of the bank — the network-forced spine) is only **0.076** of loop length at OFF; the ≥25 % trunk is 0.448. Most forward-side sharing is *pile-up by choice* (selection repeatedly picking into the same curvy massif), not network forcing. Real headroom.

## 1. Pipeline map (file:line, stage cost)

All in `src/thor/route_action.cc` (`roundtrip_impl`, :1466–2189) and `src/thor/roundtrip_expansion.cc`. Stage p50s from P1 (workers=1, corpus-v1, request p50 ≈ 520 ms):

| # | stage | where | p50 | what |
|---|---|---|---|---|
| 1 | **harvest** | `RoundTripExpansion::Harvest` roundtrip_expansion.cc:54 | 24 ms | ONE forward `Dijkstras::Expand` (max_meters = target/2 × 1.2; local roads pruned beyond `near_radius_` = min(max, **90 km hardcoded** `kFullExploreRadiusM`); 1 M label reservation) + `ScanBand` |
| 2 | **ScanBand** | roundtrip_expansion.cc:75 | (in 1; re-scan 8 ms) | walks **all** labels with path_distance in ±18 % of target/2 (`kDistanceBand`), lazy-DP curviness/bounce per label chain, straight-line ≥ 0.3×pd filter → `Turnaround{label_index, path_distance, bearing_deg, curviness_per_km, node, ll}` |
| 3 | **selection** | route_action.cc:1494–1585 | ~0 (in F1's 40 ms tissue) | node-dedup (best curviness per node, :1498) → **bearing-bucket into K sectors** (:1511) → per-sector **seed-rotated pick among top-8 curviest** (`kShuffleTopM`, :1535) under **min-separation** 0.05×target (:1523) → curviness-sorted backfill (:1554) → **refill queue** = chosen + curviness-sorted rest (:1572) |
| 4 | **build loop** | :1993–2125, `attempt_build` :1808 | the rest | per candidate: `ForwardPath` label-chain walk (:1067, ~free) → walk-back (~0) → turnaround hardening → **`route_leg` return** (:1611 — bidir A\*, hard-exclude own fwd beyond 1.5 km exemption, leash, progress-graded rejoin, **xcand surcharge merge :1678**; fallback 2nd A\* on failure) → seam Defect Gate (1 ms) → Second Via (4 ms p50, up to 2 retries × 2 legs) → distance correction (≤8/req, each = full extra `attempt_build`) → byte-dedup (:2064) → **item-4 sharing filter** (:2079) → commit + xcand memory register (:2106) |
| 5 | rank+serialize | :2148–2173 | 20 ms | stable-sort by curviness; TripLegBuilder × 2K legs |

Return-leg cost per candidate: astar 230 ms p50 / ~12 successful builds ≈ **19 ms median**, fallback searches add 55 ms p50 pool (median fallback 16 ms, ~8.4/req short-dist). Pre-return per candidate: walkback ~0 + rejoin setup ~0.2 ms + chain walk + glue (≈ F1's 40 ms tissue / 12 ≈ 3 ms) ⇒ **pre:post ≈ 1:6–1:8**.

## 2. What selection optimizes today — and the blind spot

The chosen set maximizes **curviness within bearing sectors** subject to node uniqueness and turnaround spatial separation. Diversity controls in force: bearing sector (direction), min-separation 0.05×target (turnaround spread), node dedup, seed rotation (per-seed loop variety), post-build byte-dedup + item-4 near-dup reject (frac 0.6, ON in prod).

**Blind spot:** corridor identity. The expansion is a single tree, so candidate forward corridors are *paths in one forest* — adjacent-sector candidates frequently share their entire trunk until a late divergence (the measured ff = 0.199 with a hard floor of only 0.076). The item-4 filter sees this only **after** paying the return leg, and at frac 0.6 it only rejects near-twins, not trunk-sharers.

## 3. Hook points + cost model per mechanism family

### (a) Pure selection re-scoring — hook :1535–1585 — **~free, the primary lever**

The full forward corridor of every candidate is already in memory at selection time: the label chain from `cands[i].label_index` through `expander.labels()` (exactly what `ForwardPath` walks at :1809, and what ScanBand's lazy DP already traverses). Chains in one tree share label indices, so **shared-trunk length between two candidates = intersection of their chain edge-id sets** — computable incrementally against the ≤K−1 already-chosen candidates with hash sets (~hundreds of edges each; μs-scale per comparison; even pool-wide K×N re-ranking is sub-ms at N ≈ hundreds).

Concrete shape: replace the per-sector "top-8 curviest, seed-rotated" pick with a greedy **curviness − λ·trunk-overlap(chosen)** score (or a hard trunk-overlap cap with curviness tiebreak) at :1545, and the same term in backfill/queue ordering (:1554/:1581). Design points for D1: score form (λ vs cap), keep seed semantics (seed must still rotate the loop set), exemption-discount the shared start stem (reuse the :1631 rule), determinism (chosen-set greedy is order-dependent — fine, order is deterministic).

- **Cost:** ~zero (μs–ms in a stage whose whole neighborhood is 40 ms tissue).
- **Reach:** direct on ff (0.199) + fr/rf partially (mixed pairs involve one fwd corridor); indirect on rr (returns from corridor-distinct forward legs start their searches in genuinely different places). Floor: hard trunk 0.076.
- **Risk:** trades curviness for distinctness inside the sector pick — the exact trade gate 3 + drift meters must referee in B1.

### (b) Upstream diversification — hook roundtrip_expansion.cc:54–73 — **+24 ms/tree, secondary**

The expansion itself is vanilla Dijkstras from one origin; all diversity is post-hoc. Options: N jittered trees (per-tree random edge-cost noise or per-sector cost re-weighting) at ~24 ms p50 each (harvest stage cost; +87 ms @ 300 km — ratchet-relevant at large targets), or a single tree with in-expansion trunk-spread pressure (code-invasive, touches `ShouldExpand`). Given (a) reaches the same pool for free, (b) is a fallback if (a)'s in-tree choice set proves too correlated — i.e. if even distinct chains share trunks because the tree only *has* one way out per sector (the 0.076 floor says this is rare).

### (c) Over-generate + select — hook :1993 build loop — **~25–50 ms per extra build, already half-built**

Candidate over-generation is already maximal (ScanBand returns the whole band; the queue holds every unique-node turnaround; build-until-full consumes it). The *expensive* variant — over-BUILD (build K+N loops, keep the most distinct K) — costs a return leg per extra loop (~20–50 ms each, plus tail risk), and its cheap reject-and-refill cousin **already exists as the item-4 filter** (:2079): reject a built loop sharing > frac with a kept one, pull the next candidate. Tightening item-4 (frac 0.6 → ~0.4) is a one-knob over-generate-and-select experiment — but every reject re-pays a return leg, so it's the (a)-fails fallback, not the plan.

## 4. Gate-9 metric vs in-engine score

Gate 9 (`tools/loopqual/metrics.py:592 bank_distinctness`) keys on **1e-5 grid geometry segments** of the decoded polyline, exemption-discounted, per-loop max-vs-siblings. In-engine, the item-4 filter (`loop_keylen` :1749) already implements the same semantics on **undirected graph edge ids + length weighting + exemption discount** — a faithful proxy (same graph both sides; geometry-vs-edge-key differences are noise at loop scale). A selection-time score reuses the edge-id form on label chains directly. No new metric machinery needed; gate 9 stays the external referee.

## 5. Answers to the ticket's key questions

- **Cost sunk before return leg:** ~3–4 ms of ~25–50 ms total per candidate (1:6–1:8). Over-generating *candidates* is free and already done; over-building *loops* is the only expensive form of (c).
- **Where F1's 40 ms tissue lives:** selection (§1 stage 3), per-candidate glue in `attempt_build` (ForwardPath, correlate_node, corridor marking with per-edge `GetOpposingEdgeId`, keylen bookkeeping), distance-correction re-aims, and pre-build finalisation — diffuse, K/geometry-scaled, as F1 said. A selection-stage distinctness term adds μs to a 40 ms bucket.
- **What data a selection score needs:** candidate label chains (in memory), edge ids + per-label path_distance (for the exemption discount) — all present; zero new tile I/O beyond what ScanBand's DP already touched.

## 6. Implications for D1 (design ticket)

1. **Family (a) is the lever.** Free, sited at :1535–1585, attacks the 0.326 forward-involved pool whose hard floor is 0.076. Target mechanism: greedy chosen-set trunk-overlap scoring; tunable strength/cap knob (config, like xcand's) for the quality dial.
2. **Coexistence synergy is real and now quantified:** selection (fwd side) + penalty 0.2 (rr side, already prod, 1.069×) address **disjoint** overlap components — combined estimate ~0.40 vs the 0.463 full-win bar, *at prod's current latency*. The B1 A/B ladder should include selection-ON+penalty-0.2 as a first-class arm, not just selection-ON+penalty-0.
3. **Quality referee:** the trade is distinctness-vs-curviness inside sectors (not distinctness-vs-routability as with the penalty), so expect gate-3/drift pressure, not latency pressure. The under-fill lesson carries: selection must degrade to *less-distinct*, never to *unfilled* (keep backfill's curviness order as the terminal fallback).
4. **(b)/(c) are fallbacks** with known price tags (+24 ms/tree; +20–50 ms/extra build or item-4 tightening) if (a) under-delivers — decision deferred to B1 data.

## Method appendix — offline decomposition

Script: `~/.curvagen-scratch/h1-overlap-decomp.py` (imports loopqual's `metrics.Loop`; classifies every shared, exemption-discounted segment of the gate-9 best-twin pair by leg side in both loops). Run: `cd tools/loopqual && python3 ~/.curvagen-scratch/h1-overlap-decomp.py results/c1-off-w3 results/c1-s05-w3 results/c1-s02-w3`. Reproduces the metric of record: OFF 0.5695 (report 0.5701), s05 0.4627 (0.463), s02 0.5108 (0.51). 2 780 loops per run; `fr` vs `rf` asymmetry ≈ 0.004 (sanity). Common-trunk fractions from the runs' own `report.json` (`common_trunk_frac_{25,75}_mean`).

# A1 — return-leg A* (`astar`) hotspot audit

_Wayfinder map #66 (latency optimization), ticket #71. 2026-07-15._

**Verdict: the `astar` hotspot (45% of p50) has NO configuration lever — the
round-trip hot path is insulated from Valhalla's search-tuning config.** Every
tested knob left the output byte-identical; the only ways to cheapen the
return-leg search are code changes with quality cost. Headroom for the 1.10×
ratchet must come from elsewhere (xcand-direct #73, fallback tail #72, framing
#74, RSS #75), not from tuning the biggest hotspot down.

## What was tested

Same rig as P1 (`valhalla-fork-test:rebase38`, loopqual corpus-v1, workers=1,
no-way, xcand OFF). Baseline = `results/p1-xcandoff` (p50 514 ms). Three
single-knob configs, each vs baseline:

| config | knob changed | tot p50 | astar p50 | gate |
|---|---|---|---|---|
| baseline | `threshold_delta`=420, unidir L1=100 km | 514 | 230 | — |
| thr100 | `bidirectional_astar.threshold_delta` → 100 | 578 (1.12×) | 255 | latency-only FAIL |
| bidir10k | `bidirectional_astar` hierarchy L1 20→10 km | 528 (1.03×) | 237 | PASS |
| unidir30k | `unidirectional_astar` hierarchy L1 100→30 km | 571 (1.11×) | 249 | latency-only FAIL |

## The output was byte-identical across all four configs

The counters were identical to 2 decimals (fallbacks 6.29, corrections 4.24,
dirty 1.09, attempts 13.08) and the **geometry aggregates were identical to 5
decimals across all 2780 loops**:

```
overlap 0.57009 · edge_reuse_geom 0.05867 · distance_error 0.21946 · loop_km 139.1753
```

So the knobs changed **nothing** in the algorithm. The gate "FAILs" were only
gate 10 (latency ≤ 1.10× base); every quality gate passed because the loops are
unchanged. **The latency spread (514–578 ms) is pure run-to-run timing noise on
identical deterministic output.**

### Methodology byproduct: the latency noise floor

Four runs of byte-identical computation spanned **514–578 ms p50 (±~12%)** at
workers=1. **Latency deltas below ~15% p50 at workers=1 are noise** — future
lever comparisons need multiple runs, larger effects, or a contended harness to
be real. (P1's xcand finding survives this: xcand's counters *did* change, its
effect was a +271 ms p95 tail well above the floor, and the overlap metric moved
0.570→0.463. But small config-lever latency deltas are below the floor.)

## Why the config levers are inert (code-proven)

- **Harvest ignores `unidirectional_astar`.** `RoundTripExpansion` overrides
  `ShouldExpand` / `GetExpansionHints` (`roundtrip_expansion.cc`) with **hardcoded**
  pruning: `kFullExploreRadiusM = 90000` (90 km full-explore radius), per-request
  `max_meters_ = 1.2 × target/2`, and a level-2 prune beyond the near radius. It
  never reads `expand_within_distance` from config. So R2's lever #1 is
  **falsified** — capping `unidirectional_astar` L1 (100→30 km) left harvest
  byte-identical.
- **The return bidir A* is penalty-bound, not threshold-bound.** The return leg
  (`bidir_astar.GetBestPath`, `route_action.cc:1701`) searches a graph with the
  forward corridor hard-excluded + leash + rejoin + xcand surcharges. That penalty
  structure gives a **sharp optimum** — `threshold_delta` 420→100 (less
  post-connection over-search) found no different path, so the output is
  identical. The hierarchy-limit plumbing at `route_action.cc:558` is the *normal*
  `/route` path (`route_two_locations`), which round-trip bypasses.

## The astar cost is structural, not slack

`astar` is expensive because each of the K=12 return legs must expand a
**bidirectional A* around a hard-excluded forward corridor** to find a distinct
fresh-road route home (ADR-0033). That width is a *consequence of the distinctness
constraint*, not loose search bounds — which is exactly why bounding the search
(threshold, hierarchy) changes nothing: there is no slack to trim. Reducing astar
therefore requires changing **what the search is asked to do**, not how far it may
look:

- Loosen the distinctness / exclusion (fewer excluded edges → narrower search) —
  trades the bank-distinctness win. This is **#73 X1's** territory (the xcand
  penalty *is* part of that exclusion set).
- Share a reverse tree from the common `start` across the K return legs (Part 2) —
  algorithmic, approximate, execution-backlog scale.
- Shrink the hardcoded harvest radius (`kFullExploreRadiusM`) — but that caps only
  the 5% `harvest` stage, not the 45% return search, and its own comment says it
  preserves "far-out curviness." ≤5% p50 ceiling at real quality cost. **Ledger
  line, not a ticket.**

## Part 2 — can the K return searches share pruning / the forward tree?

- **Forward tree: already shared.** One `RoundTripExpansion::Harvest` Dijkstras
  fills `bdedgelabels_` once; all candidates read it.
- **Return legs: cannot share, as built.** Each carries per-candidate state — the
  hard-excluded forward corridor, the progress-graded rejoin map, and the
  *cumulative* xcand bank (grows per committed loop). Sharing a tree would give
  every candidate the same home path and converge them (defeats ADR-0033
  distinctness).
- **One real opportunity (backlog):** the *backward* front of every return bidir
  A* starts from the same `start`, and edges within the Start Exemption are never
  surcharged — so a reverse tree from `start` over base costing could seed/bound
  all K searches, with per-candidate penalties applied as corrections. Non-trivial
  and approximate (penalties perturb the shared costs); an algorithmic prototype,
  not a config lever.

## Part 3 — per-edge costing eval (`motorcyclecost` curvature)

Settled by code inspection (no sampling profiler in the image — perf/valgrind/pprof
absent; gprof needs a `-pg` rebuild). `MotorcycleCost::EdgeCost`
(`motorcyclecost.cc:450`) is a sum of **table lookups on precomputed tile fields**:
`kDensityFactor[density]`, `kHighwayFactor[classification]`,
`kSurfaceFactor[surface]`, and the curvature term
`curvature_factor_ * kCurvatureFactor[edge->curvature()]` — where `curvature()` is
baked into the tile by mjolnir, not computed at query time. The fork's curvature
preference adds ~**one multiply-add per edge** over stock auto costing. Negligible.
`astar` is **node-expansion-count-bound**, not costing-arithmetic-bound — so
optimizing `EdgeCost` yields ~nothing; the lever is fewer nodes (which, per above,
config can't deliver). A dedicated perf pass is unwarranted given the code is
unambiguous.

## Ledger lines (for #75 synthesis)

- **`astar` (return bidir A*, 45% p50 / 1061 ms p95):** no config lever; output
  byte-identical under `threshold_delta` / bidir-hierarchy / unidir-hierarchy
  changes. Structurally bound by the fresh-road distinctness constraint. **Not a
  headroom source via tuning.** Reducible only by X1 (loosen the xcand share of
  the exclusion) or a shared-reverse-tree rewrite (backlog).
- **`kFullExploreRadiusM` harvest radius (code lever):** caps the 5% harvest only;
  ≤5% p50 ceiling, at far-out-curviness cost. Low priority; ledger line.
- **Redirect:** headroom for 1.10× must come from **#73 X1** (xcand penalty
  reformulation — *not* bounding its search, which A1 shows is inert), **#72 A2**
  (fallback `astar_fb` tail — note fallbacks 6.29/req are config-insensitive =
  structural), **#74 F1** (framing), and RSS-rightsize (#75, memory not latency).

## Caveats

- workers=1 single-run latency; ±12% noise floor (measured). Absolutes are
  Balkans-tile / warm-cache specific.
- `threshold_delta` inertness is empirical (byte-identical output) + explained by
  the sharp penalty optimum; not separately proven that the value was parsed. The
  harvest inertness is code-proven (hardcoded radius), independent of parsing.

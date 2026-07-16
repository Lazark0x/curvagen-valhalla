# Native Round-Trip Thor Action (Engine-First Phase 2)

> **Status: BUILT, corpus-gated, and DEPLOYED + verified live (2026-06-14).** Phase A (fork
> action) and Phase B (pure-translation backend) shipped to branches `curvature-costing` /
> `planning-ux-overhaul`, and deployed to api.curvagen.cc (Hetzner 37.27.86.38). Outcome vs the engine-first baseline (`docs/plans/route-gen-baseline.md`,
> tag `phase2`): edge_reuse RT 0.146→0.108 / OW 0.022→0.000, curvature ~held (1.000→0.982),
> one-way p95 3.68s→0.19s, 3 distinct shuffleable candidates. Round-trip range **300km** —
> the expansion hierarchy-prunes beyond ~90km radius (arterials-only) so long loops stay
> feasible (300km p95 **2.41s**); a **shuffle `seed`** rotates the bearing-diversity pick.
> One open trade: dist dev 0.197 (vs 0.158) — the converging one-shot distance correction was
> dropped to keep candidates distinct; a node-guarded correction can restore it at a latency
> cost (maintainer's call). Deployed via a fork `build-amd64` CI image + `deploy-curvagen-cc.sh`;
> fixed two stale-deploy bugs (api Dockerfile missing geometry.py; deploy now `up -d --build`).
> Remaining: iOS-app NAVSIM ride.

**Completes [ADR 0032](./0032-engine-first-route-generation.md)'s Phase 2** (the native
round-trip loop action it designed-but-deferred). **Supersedes the round-trip
orchestration of [ADR 0030](./0030-unified-candidate-pipeline-corridor-exclusion.md) and
[ADR 0031](./0031-seed-route-rank-isochrone-loops.md)** — the geometric anchor ring,
radius-adjustment retry, candidate fan-out, off-axis injection, and the serving-path pure
scorer all go. **Leaves [ADR 0029](./0029-route-segments-are-index-ranges.md) intact** (the
client still owns observed **Curvature** as index ranges).

## Why

ADR 0032 moved the routing *cost model* into the fork and declared the goal of a thin
Python layer, but only the cost-model emission shipped. The **Round Trip** request path
still runs the entire pre-engine-first orchestration — `loop_anchors` (a geometric ring
guess), `_compose_round_trip` (radius-retry distance targeting), a 6-way fan-out, and the
post-hoc retread scorer — exactly the "intelligence mis-placed in Python" ADR 0032 said
must move into the engine. **One-Way** still injects geometric off-axis **Shaping Points**
and ranks them with the same scorer. The "thin Python" vision was ~10% done.

The Phase 1 corpus gate proved the cost model is right (round-trip true reuse 0.246 → 0.146,
p95 1.83s → 0.76s, **Curvature** held). That was the precondition ADR 0032 set for building
a native action on the cost model rather than on faith.

**We go straight to a native `thor` action (research Option B), skipping the research's
explicit "measure Option A first" recommendation** (`docs/superpowers/research/2026-06-13-phase2-native-roundtrip-research.md`).
Option A (thin orchestration over `/isochrone` + leashed `/route`) would still leave Python
*orchestrating* — two engine calls per candidate, turnarounds chosen geometrically on a
contour rather than by curviness — i.e. thinner than today but not pure translation. We
accept the larger fork-maintenance cost of a custom action to reach the genuinely-thin
end-state, and bound that cost by the design choices below.

## Decision

**Round Trip becomes a single native engine call; both modes become pure translation in
Python.** `(start, target_distance, curviness) → N` diverse, curvy, on-target loops, all
intelligence in the fork.

### Wiring (least-invasive — option B2)

- A `roundtrip` sub-message on the **route** `Options` (carries `target_distance`,
  `num_candidates`); **no new top-level `Action`**. The client calls `/route` with
  `locations:[start, start]`, the sub-message, and motorcycle costing.
- **loki** correlates it as an ordinary route (two trivial same-point correlations — no loki
  change). In `thor_worker_t::route`, branch on `options.has_roundtrip()` into the loop
  generator. The N loops are written into `Trip.routes` and serialize as **alternates** —
  the JSON `/route` response already emits `trip` + `alternates[]`.
- **odin** and **tyr** are untouched; the action requests `directions_type: none` (geometry
  only — turn-by-turn maneuvers still come from the separate `/directions` call at
  navigation time, exactly as today).

### Algorithm (thin orchestration over stock primitives)

1. **One forward expansion.** Subclass `thor::Dijkstras`; `Compute` from the **Start Point**
   under the curvy motorcycle costing, capped at `max_meters_ = target/2 × 1.2`, with
   hierarchy limits pruning local roads in the far reaches (arterials suffice to reach the
   turnaround band; local/secondary roads stay fully explored where the turnaround lands).
2. **Turnaround selection — post-hoc, no shared-struct change.** Candidate turnarounds are
   settled nodes with `path_distance ≈ target/2`. For each, walk the `predecessor()` chain in
   `bdedgelabels_` and sum `DirectedEdge::curvature() × edge_len ÷ total_len` →
   curviness-per-km (the *same* turning-density metric as the cost model and the eval
   harness). **No curviness field is added to `EdgeLabel`/`BDEdgeLabel`** — the expansion is
   already curvy-cost-weighted, so the near-`target/2` frontier is already curvy; post-hoc
   scoring only ranks among it, and reconstructing the K-node shortlist is microseconds.
3. **Diversity.** Bearing-bucket the frontier by straight-line bearing from the Start into
   `num_candidates` sectors; keep the best curviness-per-km turnaround per sector; backfill
   empty sectors so N candidates always come back.
4. **Penalized return.** Per turnaround: use the expansion tree's forward path as the
   outbound leg (free), seed the **edge-reuse leash** (`used_edges_`, both directions) with
   its edges, route `turnaround → start` with `BidirectionalAStar` under the same costing
   (the **soft** leash prefers fresh roads but guarantees a route home in sparse networks),
   then `clear_used_edges()` before the next candidate.
5. **Distance targeting — free from the tree.** Route the return for the `target/2`
   turnaround, measure its length, then re-pick the turnaround at
   `path_distance ≈ target − return_length` (**from the same tree, no re-expansion**) and
   route that return once; keep whichever total lands closer to target.
6. **Engine ranks.** Return the N loops best-first by curviness-per-km (distance is a *gate*,
   reuse is already minimized by the leash).

**Curviness stays purely a costing input** — it makes curvy edges cheap during the
expansion, so curvy turnarounds settle first; the action itself is curviness-agnostic.

### One-Way also goes engine-native

`/route` with `alternates: N-1` under the curvy cost model returns N genuinely-different
curvy routes between the fixed endpoints, ranked by the engine — **zero new C++** (stock
Valhalla alternates). `max_candidates == 1` → `alternates: 0` (the navigation-connector
case, unchanged).

### Python collapses to translation

Both endpoints become: parse/validate → `build_valhalla_costing(curviness)` → one Valhalla
call → format N alternates (polyline6 → 3D polyline + elevation via `/height`, distance,
time) → error mapping. **Deleted:** `loop_anchors`, `_compose_round_trip`, radius-retry,
`off_axis_anchors`, `generate_candidates`/`generate_one_way` fan-out, `exclusion.py`, and the
serving-path `score_candidate`/`rank_candidates`. **`scoring.py` is demoted to eval-only** —
`curvature_reward`/`retread_fraction`/`find_lollipops` survive as the corpus harness's KPI
functions, not in the request path.

## Considered Options

- **Option A — thin orchestration over `/isochrone` + leashed `/route` (research's
  measure-first recommendation).** Rejected for the goal: it keeps Python orchestrating (two
  calls per candidate, geometric turnaround-on-contour). We chose maximal thinness over the
  cheaper measure-first path, accepting the fork cost.
- **Curviness accumulator carried in `BDEdgeLabel`.** Rejected: it is the hottest shared
  search struct (route, isochrone, expansion, centroid, reachability), so a field there is a
  forever upstream-merge tax and a layering violation, and it double-counts curviness already
  priced into `sortcost`. Post-hoc reconstruction (chosen) keeps the concern in the one
  subclass that owns it and is trivially upgradeable later if expansion-time biasing is ever
  proven necessary.
- **New top-level `roundtrip` Action (B1).** Rejected: forces matching changes in loki, the
  HTTP→action mapping, and odin/tyr. B2 reuses the `route` path and touches one branch.
- **Hard-exclude the outbound edges on the return.** Rejected: risks "no route home" in
  sparse mountain networks (Kraljevo — the worst Round Trip entry). The soft leash prefers
  fresh roads while guaranteeing connectivity.
- **Option C — arc-orienteering loop solver.** Rejected as disproportionate (a from-scratch
  search replacing Valhalla's), per the research.

## Consequences

- **Python is pure translation for both modes.** `scoring.py`/`shaping.py`/`exclusion.py`
  leave the serving path; the endpoints are parse → costing → one call → format.
- **This is the project's biggest fork-maintenance commitment.** Mitigated by design: B2
  wiring (one branch, loki/odin/tyr untouched), post-hoc scoring (no shared-struct change),
  thin orchestration over stock `Dijkstras` + `BidirectionalAStar`, and a gurka test.
- **No extra tile rebuild for the action** — it reads the baked per-edge `curvature()`; it
  rides Phase 1.1's existing Serbia tile rebuild.
- **Latency / memory risk on long loops.** A 500 km loop is a ~250 km-radius expansion on a
  3.7 GB box. Bounded by the distance cap + hierarchy pruning; peak memory and p95 on the
  longest corpus entries are a **gate**. If they blow the budget the lever is to **clamp the
  supported native round-trip distance down** — deliberately **no Python fallback** (a
  fallback would defund the thin-Python goal). Hierarchy pruning trades some
  turnaround-curviness on long loops for feasibility; accepted.
- **Lollipop rate is a measured KPI, not separately engineered.** ADR 0032 noted the leash
  did not move it; the action's curviness-per-km + distance targeting should disfavor bulbs,
  but a residual is logged, not blocked on.
- **Deploy sequencing: bank Phase 1 now.** The proven cost-model gains deploy immediately;
  this action ships as its own gated cutover when its gates pass. Deploy stays user-gated.
- **"Shaping Point" retires** from CONTEXT.md when this ships (no live referent once injection
  is deleted); "Candidate Ranking" stands (ranking moves into the engine).

## Gates (build/deploy contract — nothing measured yet)

- **gurka** (`test_motorcycle_roundtrip.cc`): N candidates, each closes at the Start, total
  within tolerance of target, near-zero edge reuse, candidates differ by bearing.
- **Corpus** (`eval_routes.py`): round-trip `edge_reuse` down, `curvature` held/up, p95
  ≤ ~2 s on the longest entries, lollipop count tracked — vs the `engine-first` baseline.
- **Backend `pytest`**: both endpoints map native `/route` (`trip` + `alternates[]`) to the
  unchanged iOS contract; `max_candidates == 1` → exactly one route.
- **Deploy** is a user-gated hard cutover (rebuilt tiles + engine + thinned API), rolled back
  by redeploying the prior image tag.

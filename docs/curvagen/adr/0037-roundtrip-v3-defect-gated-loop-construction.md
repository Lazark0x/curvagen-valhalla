# Round-Trip v3: Defect-Gated Loop Construction

> **Status: Accepted (2026-07-14) — design locked; build + gate rerun via the v3 execution
> backlog (wayfinder #52).** Outcome of the perfect-round-trips effort (wayfinder map #45):
> survey #46, defect atlas #47, harness #48, Gate v1 #49, prototype #50 (v3e, "almost
> perfect"), latency #54 (v3h, faster than baseline). Prototype evidence: spikes 43.4 % →
> **0.00 %** at both curviness levels, full K=12 fills, failures 14 → 0, curviness held
> (1.014×/1.002×), latency p50 0.60 s (0.72×) / p95 2.53 s (0.60×).

**Amends [ADR 0033](./0033-native-round-trip-thor-action.md)**: replaces its expansion
internals — turnaround selection, return costing, distance handling — with the
defect-gated pipeline below. Keeps ADR 0033's wiring intact: `roundtrip` sub-message on
`/route` (B2), loops serialized as alternates, node-dedup distinctness, shuffle `seed`,
300 km range clamp, hierarchy-pruned long-loop expansion. **Touches
[ADR 0036](./0036-rust-orchestrator-architecture.md)** only at the Costing Fingerprint
version constant. One-Way is untouched (ruled out in the Gate v1 grilling, #49:
single-leg optimal paths structurally cannot spike, lollipop, or reuse).

## Why

The defect atlas (#47) proved the current engine ships visibly broken loops: spikes in
43.4 % of corpus loops (every one wrapping the Turnaround Seam), lollipop saturation in
sparse highlands, edge reuse 0.121, and 6 % of K=12 bank fills dying whole (#53). Gate v1
(#49) fixed the quality contract; five measured prototype iterations (#50) plus three
latency iterations (#54) found a pipeline that kills the spike class structurally while
ending *faster* than baseline. What remains is committing the design, re-cutting the two
meters that bill the algorithm for deliberate behavior, and defining the path to prod.

## Decision

### 1. The committed pipeline (measured: v3h, fork `proto/v3-shortlist` @ `fab213758`)

Round Trip loop construction becomes **build → inspect → refill**:

- **Turnaround hardening** — harvest bounce rejection via the costing's own U-turn test;
  U-turn door dropped from the return origin; arrival edge guaranteed (the #53 crash
  shape); **trap-aware tip walk-back** (a turnaround's "fresh exit" must survive a bounded
  probe walk, so it never sits above a dead-end corridor that forces a bounce).
- **Hard-excluded return with fallback** — the return search bars the forward leg's edges
  (Start Exemption near the origin excepted); if no route home exists, one retry under the
  soft reuse leash (a Fallback Loop, tagged and gate-counted).
- **Progress-graded rejoin penalties** on the return leg.
- **Build-time Defect Gate** — the built loop is decoded and inspected; any seam mirror
  ≥ 30 m is rejected and the slot refilled from the queue. Hard-exclude successes take a
  seam-window (~1.5 km) verdict as final — their return search had the forward edges
  barred and cannot mirror the seam; **Fallback Loops always get the full-leg decode**
  (windowed inspection provably leaks wrapped-bounce spikes). Dirty loops are served only
  when a cell would otherwise return no route (clean-first, Distance Flex, dirty-last-resort).
- **Distance Flex** — harvest accepts turnarounds in a widened band (`ScanBand`
  0.55–1.18×) so a clean off-target loop beats a defective on-target one.
- **Memoized harvest** — per-label curviness/bounce chain sums computed once via lazy DP
  over the predecessor forest (the #54 decisive fix; harvest tail 2 684 → 254 ms).
- **Lazy wide-scan + attempt budget** — the widened band runs only when the primary queue
  stalls, and the widen grants a fresh budget (a cap that can fire before the widen
  starves hard cells — measured, v3f).

### 2. Two committed extensions (designed here, built in execution)

- **Second Via** (Gate v1's pre-wired lollipop remedy — the trigger fired). Fires from the
  Defect Gate, not a distance band: the existing post-build decode adds the metrics-v1.2
  stem check, and a loop over the stem threshold is requeued as a two-lobe build — second
  via bearing-diverse from the first, opposite half-sector, same distance band. Ladder: if
  the two-lobe build fails, the one-via loop is kept (never a no-route over shape).
  Principle is pinned here; constants (sector width, retry count) are build-time tunables.
  Rejected: a static ≤50 km two-via band (pays on the ~85 % of short loops already clean,
  misses 100 km lollipops, and the band is a Serbia-corpus artifact).
- **Node-guarded one-shot distance correction** (ADR 0033's deferred "maintainer's call" —
  now called). When a build lands outside tolerance, one corrective re-aim of the
  turnaround at a compensated distance; the node guard preserves K-candidate
  distinctness. The v3h latency margin (0.72×/0.60×) funds it, and misses concentrate at
  20–50 km where rebuilds are cheapest.

### 3. Metrics v1.2 and Gate v1.2

One new measurement concept, applied to two meters; gate *numbers* do not move:

- **Start Exemption** becomes a spec'd constant: the fork's return-exclusion exemption and
  the harness PARAM are pinned to the same value. The stem meter counts only stem beyond
  it; the reuse meter discounts reuse inside it, **capped at the constant** (no blank
  check). Rationale: a ~1.5 km network-forced stem in a 20 km loop reads 0.15 reuse and
  ~100 % lollipop *by construction* — the meters were billing the algorithm for designed,
  Andrey-confirmed behavior (clean-first, dirty-last-resort). Baseline and candidates are
  re-read under v1.2; the Second Via trigger band is recomputed from the v1.2 scorecard.
- **Gate 8b (curviness ordering c0.8 > c0.5) demotes to advisory** — the baseline itself
  fails it, so it cannot gate a candidate. Per-level gate 8 (≥ 0.95× baseline, despiked)
  stays blocking. Whether the curviness knob's upper range buys the rider anything is a
  costing-calibration question — **a separate future effort, outside the loop-shape
  scope**.
- Gates 3–7 keep their v1 thresholds and rerun in execution after Second Via + distance
  correction land. Recorded for then: baseline c0.8 already fails dist_err (0.204 vs
  0.20), so a post-correction c0.8 landing at baseline parity re-opens per-level honesty
  (c0.5 ≤ 0.20, c0.8 ≤ baseline) — a decision that returns with data, not one made now.
  A still-red gate after the rerun bounces a decision back; it does not slip silently.

### 4. Productionization

**Clean re-implementation on `curvature-costing`** — the prototype branch is
throwaway-marked and stays as reference. Each mechanism lands as a reviewed commit with
gurka coverage (the fork's test idiom): hardening, hard-exclude/fallback, rejoin grading,
walk-back, defect gate + refill, chain-sum DP, wide-scan/budget, second via, distance
correction. The per-request stage-timing ledger **stays, demoted to debug level**
(config-enabled): it found the decisive ScanBand bug and is the only stage-cost
visibility on the box.

### 5. Orchestrator and cutover

- **Zero API surface change**: same request/response shape, same alternates, same
  node-dedup bank distinctness, same definitive-codes-only Negative Cache (v3's
  dirty-last-resort *reduces* 442s). No app change; no new rider knobs (shape variety
  remains the `seed`).
- **`FINGERPRINT_VERSION` bumps 1 → 2 at cutover.** The cache key's tileset component
  tracks the tiles dataset, not the engine binary — without the bump, riders keep
  getting cached v2 loops and banked v2 candidates from identical keys. The constant's
  contract widens from "costing format" to "costing recipe **or engine algorithm
  semantics**". Old-generation entries age out (TTL 30 d + LRU); **rollback = redeploy
  the previous engine + orchestrator images — v1 keys are still warm**. Rejected: an
  engine-version key component (collapses into the same manual bump with more machinery);
  a fresh `CACHE_DB` (loses warm rollback).

### 6. Budgets

- **Latency**: governed by the rig-ratio gate (≤ 1.10×; v3h holds 0.72×/0.60×). Prod
  expectation stays descriptive: bank serve ~7 ms, fresh build p50 ~0.6 s / p95 ~2.5 s
  class. The 300 km clamp stands.
- **Memory**: a **checkpoint, not a gate** — during box smoke, capture engine-container
  peak RSS across the smoke corpus (incl. 300 km c0.8 K=12 fills); acceptance = no swap
  pressure and stated headroom on the 3.7 GB box. The measured number becomes the budget
  baseline here. A rig-side memory gate was rejected (Docker-on-Mac is a poor proxy).
  **Measured at the T10 cutover (2026-07-14, curvagen #64): engine peak RSS 400 MiB**
  across prod-smoke + a fresh 300 km c0.8 K=12 fill; zero swap configured or needed,
  2.7 GB available at peak — the budget baseline is 400 MiB with ~7× headroom.

### 7. Rollout

1. Clean re-impl merges to `curvature-costing` → fork `build-amd64` CI → ghcr amd64 image.
2. **Done-run on the rig**: corpus-v1, metrics v1.2, Gate v1.2 all-green (incl. the two
   extensions). Blocks deploy.
3. **Visual acceptance on the rig gallery, pre-deploy** — corpus and harness are
   deterministic and the image bits are what ships; the box has no RAM for a side-by-side
   engine. This is the "beautiful beyond the numeric gate" step (Andrey verdict).
4. **Engine + orchestrator deploy together, one compose up** — staging them either caches
   old loops under new keys or serves stale loops under old ones.
5. Box verification: prod-smoke subset vs api.curvagen.cc, `docker stats` RSS checkpoint,
   bank fill/serve spot checks.
6. Rollback: previous image tags stay pinned on the box; compose up on old tags; v1 cache
   keys warm within the 30 d TTL window.
7. Bank warm-up **on-demand** (first rider per cell pays one 0.6–2.5 s live build); no
   pre-warm machinery.
8. Fork push, repo push, and deploy each wait on Andrey's explicit go.

## Consequences

- Fork-maintenance surface grows (walk-back probe, defect gate, second via, correction) —
  bounded by per-mechanism gurka tests and accepted per ADR 0032/0033's engine-first
  trade. Upstream rebase (fork sits on Valhalla 3.7.0) is fork maintenance, orthogonal,
  and no precondition for v3.
- Lollipop, short-band reuse, and dist_err gates are *expected* red until Second Via and
  the distance correction land in execution — the #52 backlog carries the rerun; misses
  return as decisions, not silent re-cuts.
- The curviness-knob semantics question (8b) leaves this effort as a future-effort
  candidate.
- Backend Serving Context glossary gains Start Exemption, Defect Gate, Second Via,
  Distance Flex; the Loop-Quality Gate and Baseline entries track v1.2.
- The wayfinder map's remaining fog (deploy shape, orchestrator interplay, app knobs,
  rebase timing, visual acceptance) resolves into §5–§7; #52 wires the execution backlog
  and this map's destination is reached when it does.

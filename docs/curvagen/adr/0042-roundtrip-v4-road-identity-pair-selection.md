# 0042 — Round-trip v4: road identity, harvest hygiene, and distinctness at selection

> **Status: Accepted (2026-09-07) — built on `curvature-costing`, green against Gate v2
> (15/15 blocking rows); ships at the cutover ([#18](https://github.com/Lazark0x/curvagen-valhalla/issues/18)),
> not yet deployed.** Outcome of the round-trip v4 effort (wayfinder map
> [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4), ticket
> [#17](https://github.com/Lazark0x/curvagen-valhalla/issues/17)): delta survey
> [orch#37](https://github.com/Lazark0x/curvagen-orchestrator/issues/37), blind spots
> [orch#38](https://github.com/Lazark0x/curvagen-orchestrator/issues/38), v3 audit
> [orch#39](https://github.com/Lazark0x/curvagen-orchestrator/issues/39), census
> [#5](https://github.com/Lazark0x/curvagen-valhalla/issues/5), calibration set
> [#6](https://github.com/Lazark0x/curvagen-valhalla/issues/6), direction
> [#8](https://github.com/Lazark0x/curvagen-valhalla/issues/8), prototypes P1
> [#10](https://github.com/Lazark0x/curvagen-valhalla/issues/10) / P1.1
> [#12](https://github.com/Lazark0x/curvagen-valhalla/issues/12) / P2
> [#11](https://github.com/Lazark0x/curvagen-valhalla/issues/11) / P2.1
> [#14](https://github.com/Lazark0x/curvagen-valhalla/issues/14), build
> [#15](https://github.com/Lazark0x/curvagen-valhalla/issues/15).

**Judged by [ADR-0041](./0041-roundtrip-v4-loop-quality-gate-v2.md)** (Gate v2, as amended
2026-09-07) — that record holds the thresholds, this one holds the algorithm.
**Amends [ADR-0037](./0037-roundtrip-v3-defect-gated-loop-construction.md)**: replaces v3's
turnaround selection, its identity model for "the same road", its ranking key and its
fallback step, and deletes Second Via. 0037 stays the v3 record and keeps everything v4
does not touch — the Start Exemption, the seam Defect Gate, Distance Flex, the memoized
harvest, the distance correction, the attempt budget. **Keeps
[ADR-0033](./0033-native-round-trip-thor-action.md)'s wiring** intact (the `roundtrip`
sub-message on `/route`, loops as alternates, the shuffle `seed`, the 300 km clamp).
[ADR-0038](./0038-cross-candidate-bank-distinctness.md) /
[0039](./0039-xcand-distinctness-ships-at-0.2-latency-headroom-ledger.md) keep their place —
the cross-candidate penalty ships *inside* v4 at 0.2 —
and [0040](./0040-selection-time-distinctness-rejected.md)'s negative result stands as
written (its item-4 built-loop filter is still refused for the fills it costs).

## Why

The census (#5, 552 requests / 6 622 loops on the prod-equivalent engine) found the residual
riders complain about is one mechanism no v3 meter could see: **the return rides the other
carriageway** — 36.8 % of loops, 52.8 % on the Vračar field cell — because every v3
mechanism and every v3 meter keys on *directed edge ids*, and the opposite carriageway is a
different way. Exact-mirror spikes were identically zero; Gate v1.3 passed everywhere. The
delta survey (orch#37) found no algorithm worth adopting for that: it is an **identity**
problem before it is a search problem. The v3 audit (orch#39) added the mechanisms that feed
it — ring-reversal harvest chains (F01), one-level rejoin grades (F04), a return-blind
ranking key (F20), a Second Via that never fires in production.

## Decision

### 1. The pipeline as built

Loop construction becomes **identify → harvest clean → select a disjoint pair → build →
gate → rank**. Each rung below is measured; the ticket links carry the numbers.

**1.1 Road identity — the twin/parallel sidecar.** `thor::RoadTwinIndex`
(`valhalla/thor/road_twin_index.h`, `src/thor/road_twin_index.cc`) is built once per process
in the thor worker's constructor, keyed by tileset location + radii. Pass 1 samples every
*canonical* segment (`min(edge, opposing edge)`) at 25 m into a 120 m spatial bin; pass 2
filters the 1-ring neighbourhood on chord collinearity (±30°) and applies **COVER** (≥ 0.70
of one segment's samples within `r` of the other's polyline) and **SPAN** (those samples
project onto ≥ 0.60 of the shorter segment). `r ≤ 30 m` = **twin**, 30–80 m = **parallel**.
The span test is the detector: cover alone called 39.6 % of the tileset twins.

A geometric pair must also be **the same physical road** — *different OSM way* **or**
*constant lateral offset along the matched run* (`max − min ≤ 12 m` or `min ≥ 0.40 × max`);
a hairpin's arms converge at the apex and fail it. Serbia (211 tiles): 960 188 segments →
**80 469 twin pairs / 113 642 keyed segments**, 180 610 parallel pairs / 215 223 segments;
**5.5 s and 7 MiB at engine start** (~106 MiB transient, freed).

The index qualifies on collinearity and stores *segment* identity — OSM's storage direction
for a carriageway is arbitrary — and the call sites exclude **both directions** of a twin,
exactly as `route_leg` already does for a corridor edge and its opposite. That is what makes
the anti-parallel ride impossible.

**1.2 Tiered identity at the call sites.** Twins get the corridor's own treatment — leashed
everywhere, hard-excluded beyond the Start Exemption; parallels join the soft leash and
carry the same progress-graded rejoin as the corridor stretch they shadow. The
cross-candidate surcharge (ADR-0039) keys on twins too: a later candidate riding a committed
loop's opposite carriageway pays the same.

**1.3 Harvest hygiene.** **F01** — `ScanBand` rejects chains that revisit an undirected
segment *or its twin*, computed as **one DFS over the settled label forest**
(`ComputeChainSimplicity`, `src/thor/roundtrip_expansion.cc`) rather than a walk per
candidate: O(labels), cached across the primary and widened bands, **22.5 ms/request** mean
(the naive per-candidate shape cost +1 100 ms on a 50 km Belgrade ask). It rejects ~41 k
chains per request. **F04** — the rejoin map is built with `for_each_junction_edge`, so a
rejoin grade covers the physical junction at **all** hierarchy levels, not the corridor
node's own level. **Second Via is deleted** (mechanism, six constants, `stem_fraction()`,
its ledger line and its gurka tests; a gravestone comment marks the site): the census
measured it never firing.

**1.4 Selection — the Suurballe–Tarjan pair pass, as a selection stage.** A junction graph
`H` is built from the settled label forest (one vertex per physical junction with
hierarchy-level twins folded, the harvest's own turn-aware distances as potentials, ~103 k
junctions and 239 k arcs per request); Suurballe & Tarjan's labeling pass then yields, for
**every** sink at once, the cheapest pair of arc-disjoint paths (`src/thor/roundtrip_pairs.cc`,
`ShortestPairs`; the paper's Fig. 1–3 is a gurka pin). Directed-arc disjointness in `H` *is*
"the return never rides the forward leg's road the other way" — the defect, made structurally
impossible at selection instead of detected after the build. Pairs exist for 63.5 % of
junctions. Six approximations are named and measured in the P2 doc §1.3 (turn costs dropped
from the reduced costs and clamped, 1.1 % of arcs; a direction-symmetric return cost, re-costed
for real before serving; return-rideable arcs only for the second path; two-way arrivals
preferred as tree arcs; the second path confined to the harvest region; twins as a
post-check, not a vertex fold — the fold chains through parallel village streets and was
measured off).

**It is a selector, not a constructor, and that is the shipped reading**: 7.6 % of served
returns ride the pair's second path; 92.4 % are `route_leg` repairs built afterwards. What
the pass buys is that the *sink* is one with a disjoint way home, which is why the geometry
gate's rejects fall from 4.7 to 1.6 per request and the F02 mechanism reads **0.0 %** in
every block and slot.

**1.5 Distinctness at selection.** Selection-time rejects, each a path walk and never a
search: `no_pair` (some road lies on every route home), `band` (pair length outside
target ± 20 %), `fwd_illegal`, **`twin`** (twins-aware self-overlap ≥ 500 m, *including
return-vs-return* — the geometry gate's verdict moved before the A\*), and **sharing**:

- a **per-leg forward-sharing threshold** — a candidate whose tree path shares more than
  **0.35** of its own forward length with any bank entry's roads or their twins is refused;
- the **whole-pair 0.6 test keyed on the *built* bank** (both legs as served, plus twins),
  so later candidates are tested against the returns actually served, not against the pair;
- a **relaxation ladder** for the fills bar — the queue is re-walked at +0.15, +0.30, then
  with the leg test off, stopping when the bank is full;
- **relaxed-last** and **rescue-last** ranking, so ladder and rescue loops sink below the
  strict pair-built ones and off the Served Surface;
- **one evaluation budget per request** (1 200 fresh evaluations, 150 per relaxation rung)
  with memory-light rejects. This is mandatory, not a tuning knob: the uncapped variant
  spent 44 906 evaluations on a single 200 km ask and OOM-killed the engine.

**1.6 Build, gate, rank, fill.** `attempt_pair_build` re-costs the forward leg edge by edge,
correlates the turnaround on its arrival edge (U-turn door dropped) and assembles the return
from the pair's `ret_edge`s in reverse, bridging or rebuilding where that fails; then v3's
seam gate plus the **geometry Defect Gate** — twin-ride ≥ 500 m (free, shared with the rank
score) and a mid-return exact-mirror ≥ 30 m read over the **whole** return leg, not the seam
window — with a **refill budget of 2**, past which a gated loop is kept in the bank's last
tier instead of buying a replacement build. Ranking is on the **built** loop:

```
score = curviness(both legs) / ( (1 + 4 · overlap_frac) · (1 + dist_err) )
```

with tiers absolute before score — rung 0 (clean hard-exclude) → rung 1 (twins released) →
rung 2 (soft-leash Fallback Loop) → gated — and a stable sort inside a tier, so the seed
still owns the tie-break. **Fallback rungs are off** in v4 (the P1.1 knee): rung 1 converted
2.1 % of the legs that fail rung 0 and cost ~200 ms/request on hard cells. When the pair
queue runs dry with the bank short, the **rescue pass** builds the remaining `ScanBand`
candidates P1.1's way through the same gates (9.2 % of requests, 7.2 % of served loops) —
tree-shaped networks have no disjoint pair anywhere, and a pass that only builds pairs has
nothing to build there. K = 12 is met on every request in the corpus.

### 2. The shipped configuration lives in `worker.cc`

Fifteen `roundtrip_*` defaults are flipped in `src/thor/worker.cc` to P2.1's `d`
configuration (the full table is `docs/curvagen/research/2026-09-07-v4-build.md` §1), so
**a config with no `roundtrip_*` keys at all IS the measured engine**. The alternative —
leave the knobs off and set them in `curvagen-meta/deploy` — was rejected: prod's
`valhalla.json` is operator-supplied on the box and lives in no repo (that is how P1 measured
the xcand penalty off by omission), and this fork has no CI to catch a missing key. Every
knob remains a knob, so setting them back reaches v3 / P1 / P2 behaviour for a bisect, and
each pre-pair-pass gurka fixture now pins its own configuration rather than inheriting one.

The claim is checked on geometry, not on config: the built engine, started with no
`roundtrip_*` keys, replayed 32 saved corpus-v2 requests (384 loops) **byte-identical** to
the decision run — so Gate v2's reading of `p2-1-d` is a reading of this build.

### 3. What it costs, and what was accepted

Gate v2 on Baseline v2 with same-session prod-config brackets — **15/15 blocking rows pass**
(`tools/loopqual/results/p2-1-d/gate-v2.{md,json}`):

| row | bar | v4 |
|---|---|---|
| R1 Retrace family, served | ≤ 5 % (baseline 43.9 %) | **2.0 %** |
| R2 `spike_ge_500m` | 0 | 0 |
| R3 distance error | v1.3 row 7 | pass |
| R4 fills | 552/552 | 552/552 |
| R5 failures | 0 | 0 |
| T1 curviness retention | ≥ 0.95× per level | 1.04–1.29× |
| T2 Served-Surface Distinctness | ≤ 0.3488 / ≤ 16.6 % | **0.3370 / 13.8 %** (1.063× / +2.2 pp) |
| T3 wall p50 pooled | ≤ 1.25× (cutover exception) | **1.220×** |
| T4 wall p95 A+B | ≤ 1.30× | 1.143× |
| T5 unseen near-mirror metres, served | ≤ 1 198 m | **36 m** |
| T6 deep-bank Retrace family | ≤ 53 % | 2.6 % |

Accepted with it, each with its own record:

- **T3 at 1.220× is a cutover exception**, not the new normal (ADR-0041 amendment 2): v4
  changes the product, and the Rider judged the trade on the galleries. The next gate
  revision re-pins the baseline to v4's own production-equivalent reading (**Baseline v3**)
  and returns the ratchet to ≤ 1.10× against it. The latency floor is **building the distinct
  sinks' returns**, not the evaluation walk — with the walk back at P2's cost the median does
  not move.
- **The debt line** (measured to matter, none gated): canonical-id caching on the pass's arcs
  (the tile lookups in the evaluation walk, ~95–130 µs per candidate), degree-2 junction
  folding for the 200–300 km pass cost, and the rescue-build share (3.4 per request against
  P2's 0.9). v4.x items, sized at the Baseline v3 revision.
- **The return-repair residual is deferred**: served returns share 0.210 of the loop with a
  sibling against prod's 0.151 while the forward legs are at prod's level (0.234 vs 0.242).
  The returns are `route_leg` repairs built *after* selection, which no selection-time test
  sees. The advisory per-leg split watches it; no ticket on this map.
- **Bank Distinctness is worse than prod and reported, not gated** (0.4443 / 24.3 % vs
  0.392 / 14.4 %): the relaxation ladder that keeps K = 12 fills slots 6–11 with exactly the
  loops that failed the strict test. Honest under-fill was refused as a contract question
  (short banks in the app), not a gate one.

### 4. What v4 does not fix — and why that is right

The Rider-Verdict Calibration Set (#6, 180 census loops labeled blind) is the authority here,
not the meters:

- **Rings are not a rider defect** (F22: 20/20 rated OK) — v4 does not chase them, and D3
  stays advisory. The costing-level anti-ring leash the direction grilling kept in scope was
  not needed.
- **The forced start stem is not** (19/20 OK) — the Start Exemption is unchanged, at the
  shared constant.
- **Parallel-carriageway near-mirror is rider-visible only 44 % of the time** — it is
  nonetheless the mechanism v4 removes, because it is the one the *engine* can act on and it
  carries the metres; the 56 % it also removes cost nothing.
- **Same-pavement retrace (89 %) and seam residue (100 %) are what riders reject** — those
  are R1/R2, and both are green.
- No detector is trusted alone: "any detector fires" has recall 0.985 at **precision 0.54**,
  so a gate built that way would reject half the loops the Rider accepts. Gate v2 gates the
  Retrace family on the Served Surface and reports the rest.

### 5. Provenance leaves the engine

`TripRoute` gains a fork-local `CurvagenProvenance` sub-message (tag 100,
`proto/descriptors/trip.proto`), filled per candidate at serialization and written into each
route's `trip` object as `"provenance"` (`builder` = `pair`/`rescue`/`route_leg`, `rung`,
`tier`, `relaxed`, `gated`, `bridges`, `full_repair`). Additive: no other action sets it, the
orchestrator drops unknown fields, so there is **no app-contract change and no
`FINGERPRINT_VERSION` consequence of its own**. It turns the harness's "was this a Fallback
Loop" from a geometric guess into a read, and the served-tier check reads `tier` / `gated`
directly.

### 6. Verification

- **Gurka 53 green** — `gurka_roundtrip_audit` 28/28, `gurka_motorcycle_roundtrip` 22/22,
  `gurka_roundtrip_distinctness` 3/3, including the Suurballe paper pin, the comb map for the
  per-leg threshold and its ladder, and V4a/V4b for provenance on and off the pair pass.
- **Metrics v2 and `gate_v2.py` are harness code** in `tools/loopqual`, byte-for-byte with
  the scratch detectors that made the decisions (19 867 loops re-read, **0** mismatched
  fields). `loopqual reanalyze` re-reads a saved run without firing; Baseline v2 and the
  prod-config bracket are pinned runs. `gate_v1_proto.py` is retired in place as the
  ADR-0037 record.

### 7. Cutover

Not decided here — the cutover is its own ticket (#18) and its own go: re-enable GitHub
Actions on this fork for the amd64 image, the orchestrator's `FINGERPRINT_VERSION` **4 → 5**
in the same commit as the deploy (the FINGERPRINT rule — v4 changes route output), engine and
orchestrator deployed together, warm rollback on the previous image tags inside the 30 d cache
TTL. The twin/parallel sidecar stays a **start-time** structure; if it ever becomes a
build-time `TaggedValue` that is a tile rebuild and ships with the tiles.

## Consequences

- **Fork-maintenance surface grows again** — a spatial index over the whole tileset, a
  Suurballe implementation, and a selection stage with ten interacting knobs, against
  ADR-0032/0033's engine-first trade. Bounded by 53 gurka tests, by every knob remaining a
  knob, and by the ledger lines (`identity:`, `ranking:`, `pair-select:`, `pair-leg:`) that
  explain any served slot from the log alone.
- **Memory is the one prod number this record cannot state.** v3's budget baseline is 400 MiB
  peak RSS on the 3.7 GB box (ADR-0037 §6). v4 adds 7 MiB at start and holds the pair pass's
  graph per in-flight request (37 MB p50, 117 MB at 300 km); the rig measured 1.56 GiB peak
  per container at three workers. **The cutover must re-run ADR-0037 §6's checkpoint** —
  engine peak RSS across prod-smoke plus a fresh 300 km c0.8 K = 12 fill, with stated
  headroom — before the deploy is called good.
- **Latency regresses by design**: p50 1.22× of prod pooled, p95 1.14×. Bank serves are
  untouched; the cost lands on the first rider into a cold cell.
- **`FINGERPRINT_VERSION` 4 → 5 invalidates every cached loop and banked candidate** at
  cutover; old-generation entries age out on TTL + LRU and are the rollback path.
- **The rebase hazard stands**: do not rebase past `49cd28bc5` until upstream #6303 lands
  (PR #6214 rewrites the `EdgeCost` return line the curvature term lives on).
- Backend Serving Context glossary (orchestrator `CONTEXT.md`) already carries Retrace,
  Spike (narrowed), Ring, Served Surface, Served-Surface vs Bank Distinctness, Gate v2 and
  Baseline v2; no new term is minted here.

## Considered and rejected

- **A new construction algorithm.** The delta survey priced arc-orienteering metaheuristics,
  length-bounded packing, exact local-optimality tests and Greedy Faces out; nothing in the
  product landscape was worth adopting (BRouter merged a v3-shaped rewrite in July 2026 and
  reverted it on ~3× latency). The residual was an identity and objective problem.
- **Suurballe as the *constructor*.** Measured: 92.4 % of returns still need a `route_leg`
  repair, bridging is dead (stubs), and tree-shaped networks have no pair at all — the rescue
  pass is not optional. Keeping it as a selector is what buys the latency and the cleanliness
  at once.
- **Twin-folded junctions** (the structurally exact disjointness) — the union-find chains
  through whole neighbourhoods of parallel village streets and every folded junction becomes
  a gap the ride cannot cross.
- **Interim v3.1 patches** for the rig-confirmed audit findings (ratification Q7) — they
  folded into v4 instead.
- **The literal rung 1** ("release parallels, keep twins barred") — the parallel leash is a
  multiplicative cost factor that never blocks anything, so that rung cannot turn "no route
  home" into a route; it buys a second exhaustive A\*. Kept behind an off-by-default knob so
  the claim stays measurable.
- **The pair-keyed K × K sharing filter alone** (P2's) — nearly inert (0.018 of bank overlap)
  because it judges the pair while the rider gets the repair.
- **ADR-0040's built-loop filter as the distinctness instrument** — reaches the bar (1.081×)
  only at 468/552 fills and 31 attempts per request. Its verdict stands.
- **A latency prototype before the build** — would have delayed a shippable engine for an
  unmeasured lever; the debt line carries it instead.
- **F03** (the return leg's inadmissible A\* heuristic, 1.9–2.9× at high curviness) — real but
  rare (1.7 % of return legs); a ledger line, not a v4 mechanism.
- **F05's shortcut hole** — closed by accident because the round-trip bypasses hierarchy
  limits. Pinned by gurka G7, deliberately not "fixed".

## References

- Build: `docs/curvagen/research/2026-09-07-v4-build.md`; gate output
  `tools/loopqual/results/p2-1-d/gate-v2.{md,json}`.
- Prototypes: `2026-09-06-p1-road-identity.md`, `2026-09-06-p1-1-road-identity-iteration.md`,
  `2026-09-06-p2-suurballe-whole-loop.md`, `2026-09-07-p2-1-distinctness-at-selection.md`,
  `2026-09-06-same-road-vs-neighbour.md`.
- Evidence: `2026-09-05-loop-algorithms-delta-survey.md`,
  `2026-09-05-loopqual-blind-spots.md`, `2026-09-05-v3-correctness-optimality-audit.md`,
  `2026-09-05-audit-rig-confirmation.md`, `2026-09-05-prod-served-loops-reading.md`,
  `2026-09-06-defect-atlas-v2.md`; the calibration set under
  `tools/loopqual/results/census-v2-b4f514d7f/calibration/` (local rig).
- Suurballe, J. W. & Tarjan, R. E., *A quick method for finding shortest pairs of disjoint
  paths*, Networks 14 (1984) 325–336.
- Surface doc for the orchestrator: `docs/curvagen/extension-api.md` (the FINGERPRINT rule).

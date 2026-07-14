# loopqual — loop-quality eval harness (curvagen wayfinder #48)

The canonical meter for the round-trip v3 effort: re-run it after each
prototype/fix to prove movement. Promoted from the ticket #47 defect-atlas
scripts (`../defect-atlas/`, kept intact as #47's artifact); the spike /
lollipop / geometric-reuse detectors are the atlas detectors unchanged
(hand-validated in `curvagen/docs/research/2026-07-13-round-trip-defect-atlas.md` §3,
and re-verified here: field-identical output on all 2 602 atlas loops).

**This harness reports numbers only. Gate thresholds (what counts as
pass/fail) are ticket #49's decision and are deliberately NOT baked in.**

## Setup (one-time)

```bash
cd tools/loopqual
python3 -m venv venv && ./venv/bin/pip install pyyaml
```

## Usage

```bash
# Engine mode — fork /route with the roundtrip sub-message, prod-verbatim costing:
./loopqual run --engine http://localhost:8003 --corpus corpus-v1.yaml \
    --out results/my-run/ --engine-note "curvagen-valhalla:<tag> (<commit>)"

# Serving mode — orchestrator app DTO (POST /round-trip), e.g. prod or a local orchestrator.
# Geometry-only metrics compute from the app DTO polyline; optional --trace-engine
# (a local fork on the same tileset) re-enables edge_reuse_way:
./loopqual run --engine https://api.curvagen.cc --serving \
    --corpus corpus-smoke-v1.yaml --out results/prod-smoke/ \
    --trace-engine http://localhost:8003

# Compare two runs (markdown delta table; exit nonzero only on parse errors):
./loopqual compare results/baseline/report.json results/candidate/report.json
```

Flags: `--workers N` (default 3 engine / 1 serving), `--no-way` (skip the
/trace_attributes pass), `--trace-engine URL` (way-reuse endpoint override).

Outputs in `--out`:

- `responses/*.json` — raw responses + request + meta (the resume cache: rerunning
  skips files that exist; delete the dir to re-fire).
- `loops.jsonl` — one record per loop with every per-loop metric below.
- `report.json` — `{metrics: v1, params, run, failures, aggregates}`; the input to `compare`.
- `report.md` — human tables (per-curviness headline, by origin/distance/slot, failures).

**Determinism:** the engine is deterministic per seed, analysis iterates files in
sorted order, and aggregates are pure functions of `loops.jsonl` — same corpus +
same engine ⇒ identical `loops.jsonl` and identical `aggregates` (only the `run`
provenance block varies). Request parallelism does not affect results.

## Corpus format

YAML/JSON: `id`, global `k` / `avoid_motorways`, `origins: {name: {lat, lon}}`,
`cells: [{curviness, origins: all|[names], distances_m: [...], seeds: [...]}]`,
`way_reuse_seeds: [...]` (the seed subset for the `/trace_attributes` pass).
Jobs enumerate cells → origins → distances → seeds, in file order.
`corpus-v1.yaml` is the atlas corpus verbatim (232 requests, K=12);
**do not edit a corpus in place — a changed corpus is a new corpus id.**

## Metric spec v1

`METRICS_VERSION = "v1"` lives in `metrics.py` together with every parameter
below; any change to a formula, parameter, or input handling bumps the version.
Every `report.json`/`report.md` carries the version — compare runs only within
one version.

### Inputs

| Mode | Input | Seam (turnaround) |
|---|---|---|
| engine | fork `/route` round-trip response: 2 legs per route, polyline6 shape (1e-6) | exact: leg0/leg1 boundary (`seam_source: leg_boundary`) |
| serving | orchestrator `/round-trip` app DTO: one combined 3D polyline per route (1e5 coords + cm elevation), no legs | derived: grid point farthest (great-circle) from the start (`seam_source: farthest_point`) — seam-relative fields are approximate |

Both inputs are snapped to the same **1e-5° (~1.1 m) integer grid** (banker's
rounding) with consecutive duplicates dropped before any metric is computed.
The serving polyline is already exactly on that grid (the orchestrator encodes
at 1e5 with `round_ties_even`), so geometry-only metrics are comparable across
modes for the same route.

### Per-loop metrics

| Metric | Formula | Parameters | Input | Caveats |
|---|---|---|---|---|
| `spike_count` | number of merged exact-mirror palindrome intervals (`p[i−1]==p[i+1]`, extended while `p[i−w]==p[i+w]`, overlaps merged) with one-way stub length ≥ min | min stub 30 m | grid points, both modes | dual-carriageway U-turns are direction-distinct geometry → not counted (undercount); they surface in `shadow_frac` instead |
| `max_stub_km` | longest one-way stub among the loop's spikes, km | — | grid | 0 when no spike |
| `spike_len_fraction` | `2 × Σ stub_m / loop length` — fraction of the ride spent on out-and-back pavement | — | grid | aggregate percentiles reported over spiked loops only |
| `spike_ge_30m` / `spike_ge_500m` | per-loop booleans: any spike ≥ 30 m / any stub ≥ 500 m | 30 m, 500 m | grid | — |
| spike classes | apex at seam → `seam_uturn`; interval covers seam (±2 idx) → `seam_wrapped`; else `mid_fwd`/`mid_ret` | slack 2 idx | grid + seam | classes are exact in engine mode; indicative only with a derived seam (serving) |
| `edge_reuse_geom` | fraction of loop length on segments whose **undirected** 1e-5 grid key occurs >1× (both traversals count; pure out-and-back = 1.0) | 1e-5 grid | grid, both modes | geometric analog of the fork's both-direction leash marking; parallel carriageways NOT merged |
| `edge_reuse_way` | fraction of length on OSM ways traversed in >1 non-contiguous run — exact ADR-0033 / retired `eval_routes.py::edge_reuse_frac`, way-id runs via `/trace_attributes` (map_snap) | trace on raw (un-snapped) points, polyline6 | needs a fork `/trace_attributes` endpoint; computed for `way_reuse_seeds` only | 300 km loops exceed the 16 000-point trace limit (error 153) → `null`; reads slightly higher than `edge_reuse_geom` (counts full run lengths of partially re-ridden ways) |
| `lollipop_stem_fraction` | `(stem_out + stem_back) / loop length`; stem_out = maximal leg0 *prefix* within corridor radius of leg1 (gaps ≤ 120 m tolerated), stem_back = same for the leg1 *suffix* vs leg0; stems < 150 m zeroed | radius 40 m, gap 120 m, min stem 150 m | grid + seam split | corridor is point-proximity (grid-hash + haversine) — counts same-road and parallel-street shadowing alike |
| `is_lollipop` | `lollipop_stem_fraction > 0.10` | 0.10 | — | the atlas/ticket threshold |
| `bulb_count` | maximal unshared leg0 runs ≥ 500 m | 500 m | grid + seam split | classic lollipop = 1; theta shapes ≥ 2 |
| `shadow_frac` | fraction of leg0 length within corridor radius of leg1 anywhere | 40 m | grid + seam split | upper bound of cross-leg corridor sharing (stem + mid-loop + seam retrace) |
| `rejoin_return_frac` | `stem_back / leg1 length` — how much of the return leg shadows home | — | grid + seam split | — |
| `compactness` | isoperimetric quotient `IQ = 4πA / P²`; A = \|shoelace area\| of the grid polygon in a local equirectangular projection at the loop's mean coordinate, P = geometric loop length + closing gap | — | grid, both modes | **new in v1** (no atlas prior). 1.0 = circle, 0 = out-and-back; self-intersecting (theta) loops cancel signed area → understated (read with `bulb_count`); single cos(mean-lat) ⇒ ~2 % area error at 300 km span |
| `distance_error` | `\|declared − requested\| / requested`; declared = engine `summary.length` (km→m) or serving `paths[0].distance` (m) | — | response field | the fork keeps ±18 % harvest band and no distance correction (ADR-0033 trade) — expect ~0.18 mean at v0 |
| `curviness_retention` | served `curvinessUsed / requested curviness`, where the response carries the field | — | serving DTO only (engine trips carry no curviness field → `null`) | at HEAD the orchestrator **echoes** the request value (`handlers.rs::build_candidates`) ⇒ trivially 1.0; the metric exists to catch future serving-layer degradation (bank/fallback serving a different effective curviness) |

Aggregate blocks (per slice: overall, by curviness, and origin/distance/slot
within each curviness) report: loop fractions for the booleans, `stub_p50/p90/max`
over all detected spikes in the slice, spike-class totals, means/percentiles for
the fraction metrics, and `n` for the nullable ones. Percentiles use the atlas
convention `sorted(vals)[min(n−1, int(q·n))]`.

## Baseline v1

`results/baseline-v1-fix44/report.json` — engine `curvagen-valhalla:fix44`
(HEAD-equivalent to fork `1ac9231f1`), Serbia tiles 2026-06-13, `corpus-v1.yaml`.
Headline (curviness 0.5): spike 43.4 % ≥30 m / 35.6 % ≥500 m, stub p50 1 732 m,
lollipop 4.0 %, edge_reuse_geom 0.121, edge_reuse_way 0.100 @100 km,
compactness 0.218, distance_error 0.182. Full numbers + provenance:
`curvagen/docs/research/2026-07-13-loop-quality-harness.md`.

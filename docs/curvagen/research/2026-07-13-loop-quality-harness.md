# loopqual: the Canonical Loop-Quality Eval Harness — Metric Spec v1 and Baseline v1

- **Date:** 2026-07-13 (wayfinder ticket [#48](https://github.com/Lazark0x/curvagen/issues/48))
- **Scope:** promote the ticket #47 defect-atlas scripts into the canonical loop-quality eval harness the round-trip v3 effort gates on: a versioned metric spec (v1), a deterministic CLI (`loopqual run` / `loopqual compare`) that meters both the fork engine and the serving path, a validation run proving it reproduces the [defect atlas](./2026-07-13-round-trip-defect-atlas.md) headline numbers, a prod serving-path smoke, and the consolidated **Baseline v1** — the current reading future work must beat. **This doc is the meter and the reading only: gate thresholds (what delta counts as pass/fail) are ticket #49's decision and are deliberately not baked into the harness or this doc.**
- **Method:** harness lives in the fork checkout at `curvagen-valhalla/tools/loopqual/` (Python 3 + own venv, PyYAML only; nothing committed). Detectors are the atlas detectors ported unchanged and re-verified field-identical on all 2 602 atlas loops before any engine time was spent; new metrics (`compactness`, `distance_error`, `curviness_retention`) defined in §2. Validation ran the full `corpus-v1.yaml` (232 requests, K=12) twice against the same engine the atlas used — **local docker image `curvagen-valhalla:fix44` (built 2026-07-13 01:56), verified HEAD-equivalent to fork commit `1ac9231f1`** (atlas §1 probes; pinned-seed fingerprint re-checked before this run: Belgrade 100 km seed 7 K=3 → 93.13/118.026/126.915 km, exact match), container `curvagen-valhalla-atlas` on port 8003, Serbia tileset `Backend/data` (2026-06-13, same as prod), started for the run and **stopped after**; the long-running `:latest` on :8002 was not touched. Prod (`api.curvagen.cc`) received **4 requests total** (§4).

## TL;DR verdict

**The harness reproduces the atlas exactly — zero drift on every shared metric — and is deterministic end-to-end: two independent full corpus runs (3-way-parallel firing) produced byte-identical `loops.jsonl`, identical aggregates across all 1 448 compared scalars, and 232/232 identical raw engine payloads.** Spike 43.4 % ≥30 m / 35.6 % ≥500 m, stub p50 1 732 m, lollipop 4.0 %, `edge_reuse_geom` 0.121, `edge_reuse_way` 0.100 at the 100 km slice, the same 14 failed requests — all match the atlas to the decimal (§3), which is the expected outcome (seeded engine + unchanged detectors), so the meter itself adds no noise. New v1 readings on the same corpus: **`compactness` (isoperimetric quotient) mean 0.218** — the average served loop encloses ~22 % of the area a circle of its length would — and **`distance_error` mean 0.182** (p90 0.395), quantifying the ±18 % harvest band + no-correction trade of ADR-0033. The serving path is metered too: 4 prod smoke requests in `--serving` mode all parsed and computed (12 loops, K=3 shape), the way metric runs against a local trace engine on the app-DTO polylines, and `curviness_retention` reads 1.0 by construction today because the orchestrator echoes the request value (`Backend/orchestrator/crates/api/src/handlers.rs:427`) — it is a tripwire for future serving-layer degradation, not a current signal. Two operational gotchas captured: Cloudflare 403s (error 1010) the default Python user agent — the harness now sends its own UA — and repeat identical prod requests may replay memoized bytes (bank/memo semantics), so smoke corpora use fresh distinct seeds.

---

## 1. The harness

`curvagen-valhalla/tools/loopqual/` — CLI `loopqual`, modules `metrics.py` (the versioned spec), `runner.py`, `compare.py`, corpora `corpus-v1.yaml` / `corpus-smoke-v1.yaml`, `README.md` (full spec + usage), `venv/`. The #47 material at `tools/defect-atlas/` is left intact.

```bash
# engine mode: fork /route with the roundtrip sub-message, prod-verbatim costing
./loopqual run --engine http://localhost:8003 --corpus corpus-v1.yaml \
    --out results/my-run/ --engine-note "curvagen-valhalla:<tag> (<commit>)"

# serving mode: orchestrator app DTO (POST /round-trip) — prod or local orchestrator
./loopqual run --engine https://api.curvagen.cc --serving \
    --corpus corpus-smoke-v1.yaml --out results/prod-smoke/ \
    --trace-engine http://localhost:8003     # optional: re-enables edge_reuse_way

# per-metric delta table between two runs; exit nonzero only on parse errors
./loopqual compare results/baseline/report.json results/candidate/report.json
```

- Outputs per run: `responses/*.json` (raw response + request + meta; the resume cache), `loops.jsonl` (one record per loop, every §2 metric), `report.json` (`{metrics: v1, params, run, failures, aggregates}` — the `compare` input), `report.md` (human tables: per-curviness headline, by origin/distance/slot, failures).
- Engine-mode requests reproduce the prod serving contract byte-for-byte: costing is a verbatim port of `Backend/orchestrator/crates/domain/src/costing.rs:17-41` (incl. the half-even `round2` and `int()` truncation semantics), `avoid_motorways` both branches. Serving-mode requests are the frozen iOS DTO (`startPoint` `[lon, lat]`, `distance`, `curviness`, `avoidMotorways`, `seed` — `crates/domain/src/dto.rs:52-66`).
- **Determinism:** responses are disk-cached (re-run = resume), analysis iterates files in sorted order, aggregates are pure functions of `loops.jsonl`; request parallelism (default 3 workers engine / 1 serving) does not affect results. Proven in §3.
- `compare` prints baseline/candidate/Δ/Δ % for every numeric aggregate leaf, warns on metric-version mismatch, and takes **no** pass/fail stance (that's #49).
- `corpus-v1.yaml` is the atlas corpus verbatim: 8 origins × {20, 50, 100, 200, 300} km × seeds {7, 11, 23, 42, 101} × c0.5, plus 8 × {50, 200} km × {7, 11} × c0.8, K=12, `way_reuse_seeds: [7, 42]` — 232 requests / ~2 600 loops; a changed corpus is a new corpus id.

## 2. Metric spec v1 (summary — normative copy in the harness README / `metrics.py`)

`METRICS_VERSION = "v1"` and every parameter live in `metrics.py`; each `report.json`/`.md` carries the version, and runs are comparable only within one version. Both input shapes are snapped to the same 1e-5° (~1.1 m) integer grid (banker's rounding) before any metric — the serving polyline is already exactly on that grid (the orchestrator encodes at 1e5 with `round_ties_even`, `crates/domain/src/polyline.rs:98-114`), so geometry metrics are cross-mode comparable. Engine mode has the exact turnaround (leg boundary); serving mode (no legs in the DTO) derives the seam as the point farthest from the start — seam-relative fields are flagged approximate via `seam_source`.

| Metric | Definition (exact formula in `metrics.py`) | Parameters | Input | Key caveat |
|---|---|---|---|---|
| `spike_count` | merged exact-mirror palindrome intervals (`p[i−1]==p[i+1]` extended outward), stub ≥ min | min stub 30 m | grid, both modes | dual-carriageway U-turns not exact mirrors → slight undercount (they show in `shadow_frac`) |
| `max_stub_km` | longest one-way stub in the loop | — | grid, both | 0 if spike-free |
| `spike_len_fraction` | 2·Σstub / loop length (share of ride on out-and-back pavement) | — | grid, both | percentiles reported over spiked loops |
| `spike_ge_30m` / `spike_ge_500m` | per-loop booleans | 30 m / 500 m | grid, both | — |
| spike classes | `seam_uturn` (apex at seam) / `seam_wrapped` (interval covers seam) / `mid_*` | ±2 idx slack | grid + seam | exact in engine mode; indicative with derived seam |
| `edge_reuse_geom` | length fraction on **undirected** 1e-5 grid segments occurring >1× (out-and-back = 1.0) | 1e-5 grid | grid, both | parallel carriageways not merged |
| `edge_reuse_way` | length fraction on OSM ways traversed in >1 non-contiguous run — exact ADR-0033 / retired `eval_routes.py::edge_reuse_frac`, via `/trace_attributes` map_snap | `way_reuse_seeds` subset | raw points + a fork trace endpoint (`--trace-engine` in serving mode) | 300 km loops exceed the 16 000-point trace limit (error 153) → null; reads slightly above the geometric metric |
| `lollipop_stem_fraction` | (maximal leg0 *prefix* + leg1 *suffix* inside the cross-leg corridor) / loop length | radius 40 m, gap 120 m, min stem 150 m | grid + seam | counts same-road and parallel-street shadowing alike |
| `is_lollipop` | `lollipop_stem_fraction > 0.10` | 0.10 | — | the ticket/atlas threshold |
| `bulb_count` | unshared leg0 runs ≥ 500 m | 500 m | grid + seam | theta shapes read ≥2 |
| `shadow_frac`, `rejoin_return_frac` | leg0-near-leg1 anywhere; stem_back / leg1 | 40 m | grid + seam | corridor upper bound / early-rejoin share |
| `compactness` | **new in v1** — isoperimetric quotient `4πA/P²`; A = \|shoelace area\| in a local equirectangular projection at the loop's mean coordinate, P = loop length + closing gap | — | grid, both | 1.0 = circle, 0 = out-and-back; self-intersecting loops cancel signed area → understated (read with `bulb_count`); ~2 % area error at 300 km span |
| `distance_error` | \|declared − requested\| / requested (engine `summary.length`; serving `paths[0].distance`) | — | response field | fork keeps ±18 % band, no distance correction (ADR-0033 trade) |
| `curviness_retention` | served `curvinessUsed` / requested | — | serving DTO only (engine trips carry no such field → null) | orchestrator currently **echoes** the request (`handlers.rs:427`) ⇒ 1.0 by construction; a tripwire for future bank/fallback degradation |

Aggregates (overall, per curviness, and origin/distance/slot inside each curviness): loop fractions for booleans, stub p50/p90/max over detected spikes, class totals, means/percentiles for fraction metrics, `n` for nullable ones; percentile convention `sorted(vals)[min(n−1, int(q·n))]` (atlas-compatible).

## 3. Validation: harness vs atlas, and determinism

Full `corpus-v1.yaml` against the §Method engine (fired 3-wide, 99 s wall; analysis ~2 min; way pass 28 s; request latency p50 0.83 s / p95 4.22 s):

| Headline (curviness 0.5 unless noted) | Atlas #47 | loopqual fresh run | Drift |
|---|---|---|---|
| loops with ≥1 spike (stub ≥30 m) | 43.4 % | 43.37 % | 0 |
| loops with a stub ≥500 m | 35.6 % | 35.62 % | 0 |
| stub p50 / p90 / max | 1 732 m / 8 581 m / 39 572 m | 1 732.2 / 8 580.6 / 39 571.7 m | 0 |
| lollipop rate (stem >10 %) | 4.0 % | 4.03 % | 0 |
| `edge_reuse_geom` mean | 0.121 | 0.1213 | 0 |
| `edge_reuse_way` @100 km / overall | ~0.100 / 0.173 | 0.1001 (n=192) / 0.1730 (n=726) | 0 |
| c0.8: spike / stub p50 / worst stub | 47.0 % / 2 059 m / 88 496 m | 47.03 % / 2 059.4 / 88 496.4 m | 0 |
| failed requests (HTTP 500, code 499) | 14/232, listed cells | 14/232, **identical cells** | 0 |
| served surface slots 0–2: spike / ≥500 m / ≥2 km / lollipop / reuse>0.3 | 44.4 / 36.9 / 20.0 / 3.6 / 8.2 % | 44.39 / 36.90 / 19.96 / 3.57 / 8.20 % | 0 |

No drift anywhere (the ≥1 pp explanation clause is moot): detectors are the atlas code ported unchanged — verified field-identical on all 2 602 atlas loops before the fresh run — and the engine is deterministic per seed. **Determinism proof:** a second full corpus run produced a byte-identical `loops.jsonl`, `aggregates` equal on all 1 448 compared scalars (`loopqual compare` output: every Δ = 0), identical failures, and 232/232 identical raw engine response payloads — under parallel firing. Same corpus + same engine ⇒ identical numbers, as required for a gating meter.

## 4. Prod smoke (`--serving`)

4 requests total against `https://api.curvagen.cc/round-trip` (sequential, fresh distinct seeds, ≤100 km, one c0.8 cell; `corpus-smoke-v1.yaml`): **all 200, 3 routes each (direct-serve K=3, `handlers.rs:80`) → 12 loops, every geometry metric computed from the app-DTO 3D polyline**, `/health` probe recorded (`status: ready`). `edge_reuse_way` computed for all 12 via `--trace-engine http://localhost:8003` (local fork, same tileset — zero prod load). Latency p50 0.96 s / p95 1.35 s. The meter reads real prod defects live: e.g. Niš 100 km slot 1 carries an 8.9 km stub; Zlatibor c0.8 100 km slot 2 served 152.6 km (+52.6 % distance error); `curviness_retention` = 1.0 on all 12 (the §2 echo caveat). Operational notes: (i) Cloudflare's Browser Integrity Check 403s (error 1010) the default `Python-urllib` UA — the harness sends `loopqual/1.0.0 (curvagen loop-quality harness)`; (ii) repeat identical requests may replay memoized bytes and seedless requests consume the bank (ADR 0036 cache layers) — smoke numbers are a parse/computability check, **not** a quality baseline. Do not run the full corpus against prod (live 3.7 GB box).

## 5. Baseline v1 — the numbers to beat

Engine `curvagen-valhalla:fix44` = HEAD `1ac9231f1`, Serbia tiles 2026-06-13, `corpus-v1.yaml`, metrics v1. Machine-readable baseline for `loopqual compare`: `curvagen-valhalla/tools/loopqual/results/baseline-v1-fix44/report.json` (per-loop detail in `loops.jsonl`, human tables in `report.md`). **Thresholds/gate: ticket #49. This table is the meter's current reading.**

| Metric (v1) | c0.5 (n=2 232) | c0.8 (n=370) |
|---|---|---|
| spike_ge_30m loop fraction | **43.37 %** | 47.03 % |
| spike_ge_500m loop fraction | **35.62 %** | 39.46 % |
| stub p50 / p90 / max (m) | 1 732 / 8 581 / 39 572 | 2 059 / 9 059 / 88 496 |
| spike_len_fraction p50 / p90 (spiked loops) | 4.49 % / 23.32 % | 3.54 % / 16.78 % |
| spike classes (seam_uturn / seam_wrapped / mid) | 483 / 485 / 0 | 79 / 95 / 0 |
| lollipop fraction (stem >10 %) | **4.03 %** | 2.70 % |
| stem_frac p90 / bulbs p50 (lollipop loops) | 0.033 / 2 | 0.035 / 2 |
| edge_reuse_geom mean / p50 / p90 / max | **0.1213** / 0.044 / 0.306 / 0.942 | 0.1343 / 0.055 / 0.349 / 0.988 |
| edge_reuse_way mean (seeds {7,42}; ≤200 km) | **0.1748** (n=601); by distance 0.275 / 0.196 / **0.100** / 0.095 (20/50/100/200 km) | 0.1644 (n=125) |
| compactness (IQ) mean / p50 / p90 | **0.2182** / 0.223 / 0.384 | 0.2335 / 0.237 / 0.402 |
| distance_error mean / p50 / p90 / max | **0.1818** / 0.137 / 0.395 / 1.041 | 0.2036 / 0.126 / 0.524 / 1.267 |
| curviness_retention | n/a in engine mode (serving reads 1.0 today — §2) | n/a |
| request failures (HTTP 500 / error 499) | **14/232 = 6.0 %** of corpus requests (K=12 shape) | — |
| served surface (slots 0–2, c0.5) | spike 44.4 %, ≥500 m 36.9 %, ≥2 km stub 20.0 %, lollipop 3.6 %, reuse>0.3 8.2 %, IQ 0.222 | — |

Terrain/distance texture (c0.5, matches atlas): worst origin Vlasina (spike 62.4 %, lollipop 23.2 %, reuse 0.336, IQ 0.145), cleanest Belgrade (35.0 %, 0 %, 0.029); spike rate falls with distance (49.5 % @20 km → 32.8 % @300 km) while severity grows; lollipops are a ≤50 km phenomenon (18.3 % @20 km, 0 % @≥100 km); distance_error peaks at 100 km (0.238 mean). Full slices in `report.md`/`report.json`.

## 6. Follow-ups this harness hands to the next tickets

- **#49 (gate):** pick thresholds/deltas over the Baseline v1 scalars; `loopqual compare` already emits the per-metric delta table it needs.
- **Fix work (v3 prototypes):** re-run `./loopqual run` on the same corpus + candidate engine build, then `compare` against `results/baseline-v1-fix44/report.json`. Turnaround hardening should move `spike_*`; hard-exclude+fallback should move `edge_reuse_*`; rejoin penalties should move `lollipop_*`/`shadow_frac`; any distance-correction revisit shows in `distance_error`; `compactness` is the catch-all shape KPI the atlas didn't have.
- The 499 request-killer (atlas §8) stays visible as the harness's failure census (14/232 baseline) — a serving-reliability KPI alongside the geometry ones.

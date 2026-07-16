# R2 — config & tile-locality latency levers

_R2 research (curvagen wayfinder map #66 — latency optimization). 2026-07-15._
_Anchor: current serving config `Backend/data/valhalla.json` + `Backend/docker-compose.yml`. Base-independent identification; every Δ is **measured on the 3.8.2 base (post-K1) in P1/audits** — this doc lists levers, expected effect, and how to measure, it does not measure._

## TL;DR — ranked lever shortlist

Surveyed against the map's driver: **buy RSS/latency headroom so `xcand` ships under 1.10×.** Top candidates:

| # | Lever | Current | Expected | Risk |
|---|---|---|---|---|
| 1 | **thor hierarchy `expand_within_distance` for the round-trip pool** | level-1 = **100 km** (`unidirectional_astar`), level-0 = unlimited | latency ↓↓ — biggest per-request expansion surface on long loops | curvy routes need local roads; may cost quality |
| 2 | **Reserved label pools + `clear_reserved_memory:false`** | dijkstras **4M** / bidir_dijkstras **2M**, kept warm | **RSS ↓↓ = direct box-budget for xcand** (the driver) | cutting reservation may add per-request re-alloc latency, esp. under xcand's wider search |
| 3 | **`server_threads`** | **2** | tail-latency ↓ under bank-fill K=12 fan-out (12 reqs queue through 2 slots) | RSS ↑ per thread; needs free box cores |
| 4 | **Bounding circles (R1 handoff)** | absent (3.7.0 tiles) | loki correlation ↓ | needs a Balkans **3.8.2 tile rebuild** to populate |
| 5 | **loki `search_cutoff`** | **35 km** | loki candidate search ↓ | rural start-point correlation may need the range |
| 6 | **bidir_astar `threshold_delta`** | **420** | A* terminates earlier ↓ | may drop alternate quality on the return leg |
| 7 | **Drop elevation from tiles** | `build_elevation=True` | smaller tiles → faster reads + less RAM | **CONDITIONAL** — only if prod runs `prefer_elevation=0` |
| 8 | **Pin tar in RAM (tmpfs / `madvise`)** | mmap + page cache | cold-tile ↓; hedges eviction when xcand grows RSS | consumes RAM we're trying to free |

**Framing:** the two levers that serve the *driver* most directly are **#2 (reserved pools → free RSS)** and **#1 (round-trip hierarchy expansion → cut p50)**. #3–#8 are supporting. None is free of a quality/RSS tradeoff — that's why each is a P1/audit measurement, not a config edit to just ship.

## Current serving baseline

**`mjolnir` (tiles):** `tile_extract` = `valhalla_tiles.tar` (367 MB) **and** `tile_dir` both set → engine runs in **mmap tar-extract mode** (the fast path; `max_cache_size` 1 GB is *moot* in extract mode). `use_lru_mem_cache=false`, `use_simple_mem_cache=false` — OS page cache handles tile residency. `concurrency=2` is **build-time only** (irrelevant to serving).

**`loki`:** `search_cutoff=35000`, `minimum_reachability=50`, `use_connectivity=true`.

**`thor`:** reserved label pools — astar 2M, **bidir_astar 1M, dijkstras 4M, bidir_dijkstras 2M**; `clear_reserved_memory=false` (pools kept between requests → persistent RSS). `bidirectional_astar.threshold_delta=420`, `extended_search=false`. Hierarchy `expand_within_distance`: bidir_astar {0:∞, 1:20 km, 2:5 km}, **unidirectional_astar {0:∞, 1:100 km, 2:5 km}**.

**`service_limits`:** `hierarchy_limits.allow_modification=false` (per-request hierarchy tuning **disabled** — levers must be config-global). `max_distance_disable_hierarchy_culling=0` (culling always on).

**Deployment (`compose`):** engine `server_threads=2`, tiles built with `build_elevation/admins/time_zones=True`, `build_tar=True`, `serve_tiles=True`. No memory limit / tmpfs on the engine container. Orchestrator bank-fill K up to 12.

**Box budget baseline (from ADR-0037):** RSS peak 400 MiB, 2.7 GB free, zero swap.

## Lever inventory by layer

### thor / pathfinding — the biggest latency surface

- **Round-trip hierarchy expansion (#1).** The round-trip is a **`Dijkstras` subclass** (`roundtrip_expansion.cc` fills `bdedgelabels_`), so it reads the **`unidirectional_astar` / dijkstras** hierarchy limits, **not** `bidirectional_astar`. That block's level-1 `expand_within_distance` is **100 km** (vs bidir's 20 km) and level-0 is unlimited — i.e. the loop search can expand across local + arterial roads over a very wide radius before hierarchy culling kicks in. **P1 must first confirm which hierarchy block the round-trip expansion actually consults**, then measure capping level-0/level-1. This is the single biggest per-request node-count lever; the tension is that curvy routes *want* local roads, so capping trades quality for speed — measure on the loopqual gate.
- **`threshold_delta` 420 (#6).** How far past the best connection bidir A* keeps searching. The round-trip return leg uses bidir A*; lowering terminates sooner. Measure p50 vs reuse/quality.
- **`extended_search=false`** — already optimal; keep.

### RSS / box-budget — serves the driver directly

- **Reserved label pools (#2).** `max_reserved_labels_count_dijkstras=4M` and `_bidir_dijkstras=2M` with `clear_reserved_memory=false` reserve and **retain** big edge-label arrays per worker (≈ label-struct × count × `server_threads`). The round-trip's `bdedgelabels_` draws from this family. Balkans loops never need millions of labels → rightsizing frees baseline RSS = **headroom the ratchet can spend on xcand**. Caveat: xcand *widens* the search (more labels used per request); cutting the reservation too far forces per-request re-alloc → latency. Measure RSS **and** p50 across a sweep of pool sizes; find the knee.

### loki / correlation

- **`search_cutoff` 35 km (#5).** Max radius for candidate-edge search at the start point. Lowering shrinks the search; risk is a rural start that needs the range. Measure loki-stage ms (P1) + failure rate.
- **Bounding circles (#4, R1 handoff).** 3.8.2's faster candidate search (`has_bounding_circles()`), **baked into tiles at build** → needs a Balkans 3.8.2 rebuild. Confirm whether `/route` correlation invokes the BC path (changelog names only `/locate` + `/tile`).

### Tile loading / packaging / locality

- **mmap tar mode is already active** — no change needed; `max_cache_size` can be dropped for clarity (no serving effect in extract mode).
- **Pin the tar in RAM (#8).** With 2.7 GB free the 367 MB tar is page-cached after warmup; explicit `tmpfs` mount or `madvise(MADV_WILLNEED)` guarantees residency and hedges against eviction once xcand raises RSS. Trade: tmpfs consumes RAM unconditionally, competing with lever #2's freed budget.
- **Drop elevation from tiles (#7).** `motorcyclecost` rewards `weighted_grade()` **only when `prefer_elevation>0`** (default 0, off). If production never sends `prefer_elevation` (check the orchestrator's route request params), `build_elevation=True` bakes an unused per-edge grade field → building **without** elevation shrinks tiles (faster reads, less RAM) at zero quality cost. **Conditional on prod not using scenic/elevation reward.**
- **Region extract (R1 config axis).** 3.8.x `valhalla_build_extract --region` (#5964/#6172) is the mechanism to (re)build the Balkans extract; tighter regional tiling reduces the mmap footprint.
- **Data-locality (admins.sqlite 11 MB, timezones.sqlite 120 MB, elevation_data/).** Mostly **build-time**; at serving, non-time-dependent motorcycle round-trips barely touch tz/admin. Low serving lever — do not prioritize.

### Concurrency

- **`server_threads=2` (#3).** Bank-fill fans out up to K=12 loop requests; with 2 engine threads they queue 6-deep, inflating p95 under fan-out. Raising to the box's free core count cuts queueing (RSS ↑ per thread — interacts with #2). Confirm box core count before sizing.

## Handoffs

- **P1 (profile):** measure levers #1, #2, #3, #5, #6 on the 3.8.2 baseline. **First confirm which hierarchy_limits block the round-trip Dijkstras expansion reads** (#1) and which reserved pool `bdedgelabels_` draws from (#2) — those two determine the real knob names.
- **K1 (rebase/tiles):** rebuild Balkans on 3.8.2 for bounding circles (#4); decide elevation inclusion (#7) once prod `prefer_elevation` usage is confirmed.
- **Synthesis:** #2 (RSS) and #1 (p50) are the two levers most likely to fund xcand; the rest are supporting entries in the headroom ledger.

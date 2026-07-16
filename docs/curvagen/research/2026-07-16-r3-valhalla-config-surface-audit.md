# R3 — full Valhalla config-surface audit

_R3 research (curvagen wayfinder map #66 — latency optimization). 2026-07-16._
_Chartered because S1's shipping decision was resting on an **incomplete config survey**: R2 (#68) was driver-scoped ("top candidates vs the map's driver"), not exhaustive, and the **contention levers vs the CONTENDED ratchet** were never measured (P1 ran workers=1 = no contention; X1 held config at default). This doc reads the **whole** config surface, enumerates every knob, and resolves — exhaustively — "is there anything left to tune in Valhalla config?"_

Sources (primary, in trust order):
- **`scripts/valhalla_build_config`** — the canonical generator. Its `config` dict + `help_text` are the ground-truth enumeration of *every* key, default, and help string. (`docs/docs/python/config.md` just documents this script.)
- **Live prod config** `Backend/data/valhalla.json` (what actually serves).
- **Source of truth for the off-config levers**: `src/valhalla_service.cc`, the orchestrator (`Backend/orchestrator/crates/api/src/handlers.rs`, `crates/domain/src/costing.rs`), the loopqual rig (`tools/loopqual/runner.py`).
- Cross-checked against what **R2/P1/A1/A2/X1/F1** actually touched.

---

## TL;DR — exhaustive answer: **nothing left to tune in Valhalla config.**

1. **No per-request-p50 lever remains.** R2's shortlist was *complete* for the p50 axis — every thor/loki knob that could move single-request latency was in it, and A1/A2/X1/F1 falsified the live ones. The rest of the surface (matrix, trace, narrative, one-way-alternates, cache-mode, build-time, request-ceiling, observability knobs) is **inert, build-gated, or non-binding** for the round-trip serving path. The full walk is the ledger below.

2. **The contended ratchet — the actual gate — is not a `valhalla.json` axis at all.** Its master lever is **worker thread count**, which is a **CLI positional arg to `valhalla_service`** (`valhalla_service.cc:201`, defaulting to `hardware_concurrency()`), wired from the deploy env var `VALHALLA_THREADS` — *off the config surface entirely*. And on the prod box (**Hetzner x86-64, 2 vCPU, 3.7 GB RAM / 0 swap**, ADR-0023) it is **CPU-oversubscription-bound**: `server_threads=2` spawns 2 loki + 2 thor + 2 odin = **6 threads on 2 cores**, and the orchestrator fans out up to **K=12** concurrent requests at it. No config knob adds cores. The RSS levers (reserved pools, drop-elevation) are **slack** — RAM is not the constraint (400 MiB peak / 3.7 GB), so freeing it buys nothing for a CPU-bound ratchet.

3. **Two R2 shortlist entries are now falsified** (source-checked, not re-argued):
   - **R2 #7 (drop elevation) — FALSIFIED.** Prod costing sends **`prefer_elevation: 0.3`** (`costing.rs:38`), *not* 0, and the orchestrator calls **`/height`** on the serve path (`handlers.rs:463`) to build the 3D polyline + ascend/descend. Elevation is load-bearing for **both routing and the response payload** — dropping it is a feature/quality regression, not a free RSS win.
   - **R2 #3 (raise `server_threads`) — dead on this box.** Off-config, and raising past 2 on a 2-vCPU box deepens oversubscription rather than adding parallelism.

4. **The "contention-lever sweep" fog collapses — but exposes a real, unmeasured hole.** There is no config lever to sweep (points 2–3). *However*, the gate's "contended ratchet" (`--workers 3` = **3 concurrent client requests**, `runner.py:208`, on a many-core dev machine) is a **mild** contention regime that does **not** represent prod's **severe** one (K≈12 concurrent through 2 threads on 2 vCPU). The gate almost certainly **understates** prod's contended xcand cost. So S1 is resting on a number measured in the wrong regime.

**Net for the map:** config is exhausted. The specifiable graduate of the contention fog is **not** "sweep config knobs" (dead) — it is **re-measure the xcand ratchet in the prod contention regime** (→ new ticket **C1**, blocks S1). The full win under 1.10× remains reachable only via **lever #3** (harvest/bucketing rewrite, backlog) or the out-of-scope hardware line — R3 does not change that, but it removes config as a candidate and hardens the prod-regime risk.

---

## The complete config ledger

Every top-level section of the canonical surface, walked. Classification axes from the ticket: **(a)** round-trip serving **p50** · **(b)** the **contended ratchet** · **(c)** **RSS / box-budget**. Verdict vocabulary: **LIVE** (a real lever) · **FALSIFIED** (tested/proven inert as a lever) · **INERT** (structurally does nothing on this serving path/mode) · **BUILD** (tile-build-time, needs a rebuild + serving effect only indirect) · **CEILING** (a request limit, not a cost knob) · **OBS** (observability).

### `thor` — the pathfinding stage (the whole p50 game: astar+astar_fb = 56% of p50)

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `unidirectional_astar…expand_within_distance` L1 | 100 km | a | **FALSIFIED** (A1): round-trip harvest hardcodes its pruning (`kFullExploreRadiusM=90000`, per-request `max_meters_`), never reads this. R2 #1 dead. |
| `bidirectional_astar.threshold_delta` | 420 | a | **FALSIFIED** (A1): sweeping it left output byte-identical; return bidir A* is penalty-bound. R2 #6 dead. |
| `bidirectional_astar.hierarchy_limits` | {0:∞,1:20k,2:5k} | a | **FALSIFIED** (A1): hierarchy L1→10 km byte-identical. |
| `max_reserved_labels_count_{astar,bidir_astar,dijkstras,bidir_dijkstras}` | 2M/1M/4M/2M | c | **LIVE but SLACK**: rightsizing frees RSS (R2 #2), but RAM isn't the bind (400 MiB/3.7 GB) → buys nothing for the CPU-bound ratchet. Cutting too far adds per-request re-alloc under xcand's wider search (A1). |
| `clear_reserved_memory` | false | c | Keep false (pools warm). Setting true trades RSS↓ for alloc-churn↑ under load — wrong trade on a RAM-slack / CPU-bound box. |
| `extended_search` | false | a | Optimal; keep (R2). |
| `source_to_target_algorithm`, `costmatrix.*` (check_reverse_connection, allow_second_pass, max_iterations, hierarchy_limits…) | defaults | — | **INERT for round-trip**: the round-trip fires one native `roundtrip` thor action (`handlers.rs:157`), a `Dijkstras` subclass — it never enters CostMatrix. Matrix-only knobs. |
| `bidirectional_astar.alternative_cost_extend`, `alternative_iterations_delta` | 1.2 / 100000 | — | **INERT for round-trip**: these govern `/route` *alternates* (one-way path, out of scope). The round-trip harvests candidates internally, not via bidir-A* alternates. |

**thor verdict: structurally locked.** Every live p50 knob was tested and falsified (A1/A2/X1). The reserved pools are the only remaining non-inert knob and they are an RSS lever with no CPU payoff. Nothing to tune.

### `loki` — correlation stage (F1 measured it at **0.9 ms**)

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `service_defaults.search_cutoff` | 35 km | a | **NEGLIGIBLE** (R2 #5): shrinks a stage that is already 0.9 ms (F1); no meaningful headroom, and risks rural-start correlation failure. |
| `minimum_reachability`, `node_snap_tolerance`, `street_side_*`, `heading_tolerance` | defaults | a | Correlation-quality knobs, sub-ms effect. Non-levers. |
| `use_connectivity` | true | — | Startup connectivity-map build; not per-request. |
| `actions`, `mvt_*` | defaults | — | **INERT**: endpoint registration / vector-tile serving; round-trip touches neither. |

### `mjolnir` — tile build + tile cache

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `max_cache_size` | 1 GB/thread | c | **INERT in extract mode**: both `tile_extract` and `tile_dir` are set → engine mmaps the tar; the OS page cache handles residency (R2). This 1 GB is moot. |
| `use_lru_mem_cache`, `use_simple_mem_cache`, `lru_mem_cache_hard_control`, `global_synchronized_cache` | all false | c | **INERT in extract mode** (tiles mmap'd, not held in an in-process cache). |
| `max_concurrent_reader_users` | 1 | — | **INERT**: threads for network tile fetch via curl; tiles are local, no `tile_url`. |
| `data_processing.build_bounding_circles` | **absent in prod** | a | **BUILD + DEAD**: R1's one "free-win" delta. Needs a 3.8.2 config re-merge **and** a Balkans tile rebuild — and even then only speeds `/locate`+`/tile`, not `/route`. With loki at 0.9 ms (F1) the win is < 1 ms for a full-rebuild cost. Not worth it. |
| `scan_tar` | false | — | Cold-start page-cache warm only; not steady p50. |
| `hierarchy`, `shortcuts`, `include_*`, `reclassify_links`, `id_table_size`, `data_processing.*` | defaults | — | **BUILD**: change tile *content*; rebuild-gated and quality-critical. Not serving-config levers. |

### `additional_data.elevation`

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `elevation` (tiles path) | set | c | **FALSIFIED as an RSS lever** (was R2 #7): costing sends `prefer_elevation: 0.3` (grade affects edge cost → path choice) **and** the serve path calls `/height` for the 3D polyline + ascend/descend (`handlers.rs:463`). Load-bearing; dropping it regresses routing *and* payload. |

### `httpd.service`

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `timeout_seconds` | -1 (infinite) | — | **SAFETY, not p50**: the only place to cap A2's rare 19 s fallback tail. Doesn't reduce median; a request-hygiene knob worth considering independently of the ratchet. |
| `drain_seconds`, `shutdown_seconds` | 28 / 1 | — | Lifecycle plumbing. Non-levers. |

### `service_limits` — request ceilings, not cost knobs

| Knob | Current | Axis | Verdict |
|---|---|---|---|
| `hierarchy_limits.allow_modification` | false | a | **STRUCTURAL**: per-request hierarchy tuning is **disabled** — any hierarchy lever would have to be config-global (reinforces that the falsified A1 knobs are the only handle, and they're falsified). |
| `max_distance_disable_hierarchy_culling` | 0 | a | Culling always on; raising it would *slow* things. Not a win. |
| `motorcycle.max_distance` (500 km) etc. | defaults | **CEILING** | Gate what's *allowed*, not what it *costs*. |
| `allow_hard_exclusions` | false | — | Gates request-level `exclude_*`. Unrelated to X1's fork-side hard-exclusion experiment. |

### `odin`, `meili`, `statsd`, `logging`

- **`odin`** (directions): F1 measured 0.1 ms; the round-trip sends `directions_type:"none"` (`handlers.rs:167`) → odin does near-nothing. `markup_formatter` off. **INERT/OBS.**
- **`meili`** (map-matching): the `/trace_*` path. **INERT** — the round-trip never map-matches.
- **`statsd`**, **`logging`** (incl. the prod-added `long_request` at loki 100 ms / thor 110 ms): **OBS.** No perf effect.

---

## The contended ratchet is off-config and hardware-bound

This is the crux R3 exists to resolve. Walking it explicitly:

**Where thread count comes from.** `valhalla_service.cc:201`:
```cpp
auto worker_concurrency =
    pos_args.size() < 2 ? std::thread::hardware_concurrency() : std::stoul(pos_args[1]);
```
Each of loki/thor/odin then spawns `worker_concurrency` threads (`valhalla_service.cc:217–240`). Prod wires this from `VALHALLA_THREADS:-2` in `docker-compose.yml`. **It is not a `valhalla.json` key.** So the single biggest contention lever is invisible to a config audit — and to any config sweep.

**The prod box is CPU-bound.** ADR-0023: Hetzner x86-64, **2 vCPU**, 3.7 GB RAM, 0 swap. `server_threads=2` → **6 worker threads on 2 cores** (plus the tokio orchestrator + skadi on the same box). The orchestrator's bank-fill fans out up to **K=12** concurrent round-trip/`/height` requests (`join_all`, `handlers.rs:422`; "9 concurrent /height", `cache_layer.rs:24`). Under that load the box is throughput-limited by 2 cores; adding threads only adds context-switch thrash, and RAM (400 MiB used) is nowhere near binding. **No `valhalla.json` knob relieves CPU saturation.**

**Why xcand's ratio *amplifies* under contention.** xcand widens each request's penalized return-leg A* (P1: +98 ms p50 is almost all wider expansion). Under CPU saturation that extra work is paid at a worse point on the queueing curve — each request holds a core longer → deeper queue → superlinear latency. That amplification (clean 1.19× → contended 1.32×) is intrinsic to "xcand costs more CPU × CPU-saturated box." Config cannot add cores, so config cannot damp it.

**The gate is measuring the wrong contention regime.** The loopqual "contended" number is `--workers 3` = **3 concurrent client requests** (`runner.py:208`), against an engine at dev-core concurrency, on a many-core dev machine — a *mild* regime (~matched cores). Prod is **K≈12 concurrent** through **2 threads on 2 vCPU** — a *severe* regime. The gate's 1.32× therefore likely **understates** prod's contended xcand cost. **S1's shipping decision (esp. option (b): "strength 0.2 = 1.09×") is measured on the dev regime and may exceed 1.10× at prod thread count.** This is unvalidated.

---

## Config drift (prod `valhalla.json` vs the 3.8.2 generator default)

Prod's config was generated from an older base and never re-merged with 3.8.2's generator. Missing-from-prod keys: `mjolnir.data_processing.build_bounding_circles` (3.8.2-new; R1's lever), `mjolnir.shortcut_caching`/`default_speeds_config`/`dataset_id`, `loki…mvt_cache_dir`, `statsd.host`/`tags`. Prod *adds* `logging.long_request` (observability). None of the absent keys is a serving-latency lever (all BUILD/OBS/optional). A re-merge is hygiene, not headroom.

---

## Verdict: is there anything left to tune in Valhalla config?

**No.** Exhaustively:
- **p50 axis** — every live thor/loki knob was in R2 and falsified by A1/A2/X1/F1; the rest of the surface is inert / build-gated / ceiling / observability for the round-trip path. **Zero remaining p50 config lever.**
- **contended-ratchet axis** — off-config (thread count is a CLI arg) and hardware-bound on 2 vCPU; the RSS levers are slack (RAM not the bind); drop-elevation is falsified. **Zero config lever moves the ratchet on this box.**
- **RSS axis** — reserved pools + drop-elevation are the only two, both slack/falsified; RSS isn't binding anyway. **No actionable RSS win.**

**What graduates (specifiable now):**
- **C1** — re-measure the xcand ratchet in the **prod contention regime** (client K≈12, engine `server_threads=2`, `docker --cpus 2`) so S1 decides on the number it will actually ship against, not the dev-regime `--workers 3` figure. Cheap (loopqual A/B, no rebuild). **Blocks S1.**

**What is ruled out (was fog, now dead):**
- **"Contention-lever config sweep"** — there is no `valhalla.json` knob to sweep; the ratchet is CPU/hardware-bound and its master lever is off-config.

**What stays in the fog (unchanged by R3):**
- **Lever #3** (fold distinctness into harvest/bucketing) — the sole route to the *full* win (overlap 0.463) under budget; backlog-scale. Now the *only* live technical route, config having been exhausted.
- The out-of-scope hardware line (2 vCPU → more cores directly attacks the contended ratchet) — remains out of scope (Andrey-fixed).

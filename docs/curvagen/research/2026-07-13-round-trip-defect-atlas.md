# Round-Trip Defect Atlas: Spikes, Lollipops, and Road Reuse Measured Against the Fork at HEAD

- **Date:** 2026-07-13 (wayfinder ticket [#47](https://github.com/Lazark0x/curvagen/issues/47))
- **Scope:** empirically reproduce and characterize the three loop-defect classes of the native round-trip action — **spike** (out-and-back stub), **lollipop** (sub-loop on a shared stem), **road reuse** (same road ridden more than once, either direction) — against the Valhalla fork (`curvature-costing`); confirm/refute the causal analysis of [2026-07-13-loop-route-algorithms.md](./2026-07-13-loop-route-algorithms.md) §1; measure per-defect frequency and severity across terrain × distance × candidate slot; deliver a named worst-case repro gallery.
- **Method:** all numbers come from the **local docker image `curvagen-valhalla:fix44` (built 2026-07-13 01:56), verified HEAD-equivalent to fork commit `1ac9231f1`** — i.e. they include the T9 min-separation guard + 1..16 clamp (`aee60b8c2`) and the #44 sink skip (`1ac9231f1`); provenance probes in §1. Serbia tileset = the existing `Backend/data` volume (built 2026-06-13; tile format is code-version-independent on the 3.7.0 base), same tileset as prod. Corpus: 232 requests / 2 602 loops at K=12 (bank-fill shape), prod costing from `Backend/orchestrator/crates/domain/src/costing.rs` verbatim. Prod (`api.curvagen.cc`) was touched by **2 requests total** (one calibration generate + one memoized re-read). Detectors are quick-and-dirty by design (this atlas seeds the #48 harness); scripts + raw data + gallery live in the fork checkout under `curvagen-valhalla/tools/defect-atlas/` (§11).

## TL;DR verdict

**All three defects reproduce at HEAD, at high frequency, and the §1 causal analysis is confirmed in its strong locational form: every one of the 1 142 detected spikes wraps the turnaround seam — zero mid-leg spikes in 2 602 loops.** Mechanically the picture is refined: only ~49 % of spikes are the pure `correlate_node` opposing-edge U-turn (§1's named door); ~51 % enter one step earlier, at **harvest** — the forward Dijkstra legally U-turns at dead-end nodes (`motorcyclecost.cc:409-412`), and labels whose predecessor chain already contains such a bounce land in the ±18 % band and get harvested, so the turnaround sits mid-stub and the loop carries the whole out-and-back regardless of what the return leg does. #44's zero-exit skip does not touch this class. Numbers at prod curviness 0.5: **43.4 % of loops contain ≥1 spike ≥30 m** (35.6 % have a stub ≥500 m; median stub **1.7 km**, p90 8.6 km, worst **88.5 km** — a 184 km "loop" that is 99 % out-and-back); **4.0 % are lollipops** (stem >10 % of ride) but heavily concentrated (Vlasina 23.2 %, 20 km distance 18.3 %, zero at ≥100 km); **reuse mean 0.121** (geometric both-direction), reproducing ADR-0033's way-based 0.108 at the comparable 100 km slice (0.100). Defect rates are **flat across the 12 slots** — the top-3 *served* slots spike at 44.4 % with 20 % carrying a ≥2 km stub, so banking serves the same defect density users already see; slot 0 of Golija-300 ships a 39.6 km stub **identically for all five seeds**. **Bonus finding (new, serving-level): 6.0 % of corpus requests die whole** with `499 Could not find matching edge candidate` → HTTP 500 — `TripLegBuilder::Build` runs outside the per-candidate failure contract and `correlate_node` omits one-way/shortcut arrival edges; Belgrade 20 km kills 3/5 seeds at K=12, Zlatibor 200 km 5/5, while K=3 passes everywhere — the exposed surface is exactly the **bank-fill path (`fill_k = 12`)**, and error 499 is not in the orchestrator's definitive-no-route set, so failed fills are retried, deterministically failing again per seed.

---

## 1. Engine: build provenance and calibration

The corpus must run on HEAD because two commits after the 2026-06-13 `curvagen-valhalla:latest` image change defect behavior (T9 min-separation reshapes turnaround spacing; #44 removes the worst spike class). The local image `curvagen-valhalla:fix44` (docker created `2026-07-13T01:56`, hours after `1ac9231f1` landed) was verified HEAD-equivalent by three discriminating probes (all requests: Curviness 0.5 costing, `avoid_motorways` true, seed as stated):

| Probe | `latest` (stale 2026-06-13) | `fix44` | HEAD expectation |
|---|---|---|---|
| K=20 clamp, Belgrade 100 km seed 7 | returns **20** routes | returns **16** | 1..16 clamp at `src/worker.cc:1304-1308` (T9) |
| Zlatibor 300 km K=12 seed 7 | `[286.6, 286.6, 286.6, 286.6, 286.6, …]` — the known 5× byte-identical collapse | `[286.6, 262.8, 349.4, 341.2, …]` all distinct | min-separation guard `src/thor/route_action.cc:1076-1088` (T9) |
| Pinned-seed fingerprint seed 7, 100 km, Belgrade | 93.1 / 118.0 / 126.9 km | identical | K=3 selection unchanged where turnarounds were already separated |
| **Prod cross-check** seed **424711**, Zlatibor 300 km, K=3 | — | **286.6 / 349.4 / 277.4 km** | prod `POST https://api.curvagen.cc/round-trip` returned **exactly 286.6 / 349.4 / 277.4 km** — prod runs HEAD-equivalent code on the same tileset |

Rig: `docker run -d --name curvagen-valhalla-atlas -p 8003:8002 -v …/curvagen/Backend/data:/custom_files -e use_tiles_ignore_pbf=True -e serve_tiles=True … curvagen-valhalla:fix44` (full env mirrors the long-running `latest` container; the tiles dir had already been served by fix44 the night before — `valhalla.json` mtime 2026-07-13 02:08). Requests go to local `/route` with the `roundtrip` sub-message; the costing JSON is the byte-exact output of `costing(curviness, avoid_motorways=true)` (`Backend/orchestrator/crates/domain/src/costing.rs:17-41`, incl. `reuse_penalty: 0.8` at `:36`); prod default curviness = 0.5 (`crates/domain/src/dto.rs:7-9`), direct-serve K = 3 (`crates/api/src/handlers.rs:80`), bank-fill K = `fill_k` = 12 (`crates/api/src/cache_layer.rs:461`, `BANK_FILL_K=12` live).

## 2. Corpus

8 origins (all verified to snap via `/locate`), 5 distances, 5 seeds, prod curviness 0.5 everywhere + a high-curviness 0.8 subset, K=12 (bank-fill shape → richest sample per expansion):

| Origin | lat, lon | Terrain |
|---|---|---|
| belgrade | 44.7900, 20.4500 | urban core |
| novisad | 45.2551, 19.8452 | Vojvodina flat grid |
| zlatibor | 43.7300, 19.7000 | border-clipped mountain massif (known pathological) |
| nis | 43.3209, 21.8958 | foothills |
| djerdap | 44.4650, 22.1530 | Danube gorge, one-road valleys (spike-prone) |
| vlasina | 42.7130, 22.3450 | SE sparse highlands, border-clipped |
| golija | 43.5850, 20.2290 | central sparse mountains (Ivanjica) |
| belacrkva | 44.8975, 21.4172 | SE Vojvodina sparse, near RO border |

Grid: `{8 origins} × {20, 50, 100, 200, 300 km} × {seeds 7, 11, 23, 42, 101} × c0.5` = 200 requests, plus `{8 origins} × {50, 200 km} × {seeds 7, 11} × c0.8` = 32. Outcomes: **204 × 12 routes + 14 × 11 routes + 14 × HTTP 500** (the 500s are §8) → **2 602 loops analyzed** (2 232 at c0.5, 370 at c0.8). The 11-route responses are externally indistinguishable per-candidate drops (return-leg failure `route_action.cc:1170-1180`, the #44 skip `:1164-1165`, or separation-guard exhaustion). Note the origin mix deliberately over-samples sparse/pathological terrain (5 of 8) — read per-terrain tables, not the aggregate, as "Serbia average".

## 3. Detectors and their validation

All detectors operate on the decoded polyline6 legs (`legs[].shape`, 1e-6), points snapped to a 1e-5 (~1 m) integer grid, consecutive duplicates dropped (`tools/defect-atlas/atlas.py`):

- **Spike** — exact-mirror retrace: a reversal apex `p[i-1] == p[i+1]` extended while `p[i-w] == p[i+w]`; overlapping palindromes merged; reported when the one-way stub ≥ 30 m (excludes snap jitter). Records stub length, apex position (fraction 0..1), and class by seam relation (§5). *Limitation:* a U-turn across a dual carriageway has direction-distinct geometry and is not exact-mirror — such cases surface in the corridor detector instead, so the spike counts here are a slight undercount.
- **Reuse** — undirected segment reuse: fraction of loop length on 1 m-grid segments whose undirected key occurs >1× (both traversals count; a pure out-and-back = 1.0). This is the geometric analog of the leash's both-direction marking (`route_action.cc:1132-1140`). Additionally the **exact ADR-0033 metric** (way_id runs via `/trace_attributes`, port of the retired `Backend/scripts/eval_routes.py:88-120`) was computed for the seeds {7, 42} subset — 726 loops; the 300 km loops exceed the trace shape limit (`error 153: Too many shape points … limit is 16000`) and are covered by the geometric metric only.
- **Lollipop** — cross-leg corridor at 40 m radius (grid hash + haversine): **stem** = maximal leg0 *prefix* + leg1 *suffix* within the corridor (gaps ≤120 m tolerated, min 150 m), `stem_frac` = (stem_out + stem_back)/loop; **shadow_frac** = fraction of leg0 within 40 m of leg1 anywhere; **bulbs** = unshared leg0 runs ≥500 m.

Validation on hand-inspected loops: (a) Đerdap 100 km seed 7 slot 2 — detected 4 273 m seam spike; the printed coordinates mirror exactly around the apex (`pts[1900..1898] == pts[1902..1904]`) and the apex *is* the turnaround; (b) slot 3 of the same response — palindrome interval [1742, 2268] contains the seam (index 2116) with apex at 2005: decoding shows forward leg passes the turnaround location, continues 3.7 km to a dead end, bounces, returns — the mechanism split of §5; (c) Niš 20 km seed 7 slot 4 (gallery E) — brute-force min-distance from leg0 marks to leg1 confirms a 3.5 km stem at 0–41 m separation (same road at 0 m, parallel streets at 36–41 m), matching `stem_frac 0.411`.

## 4. Spikes: frequency and severity

Prod curviness 0.5 (n = 2 232 loops), spike = stub ≥ 30 m:

- **43.4 % of loops contain ≥1 spike; 35.6 % contain a stub ≥ 500 m.** Stub length p50 = **1 732 m**, p90 = 8 581 m, max 39 572 m (n = 968 spikes).
- Spiked loops spend a median **4.3 %** and p90 **22.7 %** of the entire ride on out-and-back pavement (2× stub / loop length).
- High curviness (0.8, n = 370) is slightly worse: 47.0 % / stub p50 2 059 m / max **88 496 m** (gallery B: a 183.7 km "loop" with reuse 0.987 — one 88.5 km road ridden out and back with a small bulb at the end).

Per origin and distance (c0.5):

| Origin | n | % ≥1 spike | Distance | n | % ≥1 spike | max stub |
|---|---|---|---|---|---|---|
| vlasina | 298 | **62.4 %** | 20 km | 432 | **49.5 %** | 10.0 km |
| belacrkva | 293 | 46.1 % | 50 km | 427 | **52.0 %** | 23.3 km |
| golija | 300 | 44.3 % | 100 km | 480 | 43.1 % | 10.8 km |
| zlatibor | 240 | 44.2 % | 200 km | 420 | 40.5 % | 26.5 km |
| djerdap | 276 | 40.9 % | 300 km | 473 | 32.8 % | **39.6 km** |
| nis | 300 | 36.7 % | | | | |
| novisad | 288 | 35.4 % | | | | |
| belgrade | 237 | 35.0 % | | | | |

Spike *rate* falls with distance but *severity* grows (the worst stubs live at 200–300 km); sparse highlands (Vlasina) are the worst terrain, but even flat Novi Sad grid sits at 35 % — no terrain is clean.

## 5. Causal verdict: two doors, both at the turnaround seam

§1 of the algorithms doc predicts spikes "can only arise at the turnaround seam via the opposing-edge U-turn in `correlate_node`" (`route_action.cc:1017-1026`), with a hedge that a label harvested "mid-spur or in a dead-end tree" retraces the spur. Measured against all 1 142 spikes in the corpus:

- **Locational claim CONFIRMED, 100.0 %:** every spike's palindrome interval covers the seam (±2 indices); apex-to-seam distance ≤5 % of loop length for 1 055/1 142, ≤10 % for 1 124. **Zero mid-leg spikes** (interval strictly inside one leg) in 2 602 loops — consistent with both legs being label-optimal paths.
- **Mechanism REFINED — the split is 49/51:**
  - **`seam_uturn` (562, 49.2 %)** — apex exactly at the turnaround: the return leg *opens* by riding the opposing edge of the forward leg's last edge, exactly the `correlate_node` door (offered at `route_action.cc:1023-1025`, leash-penalized 4.2× but never forbidden). Broader boolean: **23.2 %** of *all* loops open their return with at least one retraced segment (604/2 602; 42 of them under the 30 m spike floor).
  - **`seam_wrapped` (578, 50.6 %)** — apex *before* the seam, palindrome covering it: the **harvest door**. The forward expansion U-turns at a dead end — legal, `MotorcycleCost::Allowed` rejects U-turns only `!pred.deadend()` (`src/sif/motorcyclecost.cc:409-412`) — and the bounced label's padded `path_distance` lands in the ±18 % band (`roundtrip_expansion.cc:16`) and can win node-dedup on curviness. The harvested turnaround then sits mid-stub: the loop contains the full out-and-back *before the return leg is even routed*. In the worst cases the return additionally retraces the approach road, fusing both doors into one giant palindrome (gallery A, H-neighbor slot 3 in §3(b)).
  - Apex *after* the seam (return-side bounce): **2 cases** (one 64 m, one 2.6 km) — negligible, consistent with bidir-A* optimality making return-side bounces nearly impossible.
- **Nothing outside §1's frame was found for geometry defects** — no unexplained spike class. The one genuinely unpredicted defect is *not geometric*: the request-killing 499 (§8).
- Implication for the shortlist in the algorithms doc: **turnaround hardening item (1) must include the harvest-side walk-back**, not just the `correlate_node` U-turn drop — dropping the opposing edge from the return origin fixes ≤49 % of spikes; the other half is already baked into the forward leg at harvest time. #44's guard (`route_action.cc:1161-1165`) only skips zero-exit sinks and touches neither door.

## 6. Lollipops: stems and early rejoin

- Overall (c0.5): **4.0 %** of loops have `stem_frac > 10 %`; p90 stem_frac is only 0.033 — lollipops are rarer than spikes but **concentrated**: **Vlasina 23.2 %**, Đerdap 4.3 %, Niš 3.0 %, all other origins 0.0 %; by distance: **20 km 18.3 %**, 50 km 2.6 %, **zero at ≥100 km**. The defect is a short-loop, sparse-terrain phenomenon — at 20 km in one-road terrain there is often only one way out of the start neighborhood.
- **Early-rejoin causation confirmed:** lollipop loops (n = 100 across both curviness levels) spend mean **19.8 %** (p50 18.0 %) of their return leg shadowing home along the outbound corridor; 96/100 have the shared corridor as a return-suffix (`stem_back > 0`), 87/100 also as an outbound-prefix — i.e. the stem is precisely "rejoin early, shadow home". Their reuse mean is **0.588 vs 0.105** for non-lollipops — stems are mostly same-road riding, with parallel-street shadowing mixed in (gallery E shows both: 0 m and 36–41 m corridor stretches).
- Median bulb count for lollipop loops is 2 — multi-bulb ("theta") shapes are common, not just the textbook single-bulb pop.

## 7. Road reuse: the leash residual

- Geometric both-direction reuse (c0.5, all 2 232 loops): **mean 0.121**, p50 0.044, p90 0.306, max 0.942. High curviness: mean 0.134, max 0.988.
- Exact ADR-0033 way-run metric (`/trace_attributes`, seeds {7, 42}, 726 loops ≤200 km): mean 0.173 overall, but **0.100 at the 100 km slice — reproducing the ADR-0033 corpus figure 0.108** (that corpus was not skewed toward short sparse loops). Paired way-vs-geometric difference is +0.017 mean — the two metrics agree; way-based reads slightly higher by counting full run lengths of partially re-ridden ways.
- Distance is the dominant axis: way-reuse mean **0.275 at 20 km → 0.196 (50) → 0.100 (100) → 0.095 (200)**; geometric continues to 0.042 at 300 km. Terrain second: Vlasina 0.336 vs Belgrade 0.028 (geometric, c0.5). The 4.2× soft leash (`reuse_factor = 1 + 0.8 × 4.0`, `motorcyclecost.cc:59-60,385-386`; prod `reuse_penalty 0.8`, `costing.rs:36`) loses exactly where §1 predicted: sparsity (only road home) and short targets.

## 8. New serving defect: one poisoned candidate kills the whole request (HTTP 500)

**14/232 corpus requests (6.0 %) failed entirely** with `{"error_code":499,"error":"Unknown: Could not find matching edge candidate","status_code":500}`. Census: belgrade 20 km — seeds 7, 23, 42 (3/5); belgrade 50 km — 7, 23; djerdap 50 km — 7, 23 (+ c0.8 seed 7); novisad 20 km — seed 11; **zlatibor 200 km — all 5 seeds**.

Mechanism (code + K-sweep):

- `correlate_node` adds, per auto-accessible **outbound** edge, that edge plus its opposing inbound edge — and skips shortcuts and non-auto-accessible edges (`route_action.cc:1017-1026`, filter at `:1020`). If the forward leg **arrives** at the harvested node via an edge whose opposing outbound is not auto-accessible (one-way arrival) or is a shortcut, the arrival edge is **absent** from the turnaround Location.
- `TripLegBuilder::Build` → `RemovePathEdges` then throws `std::logic_error("Could not find matching edge candidate")` (`src/thor/triplegbuilder.cc:442-447`) when trimming the forward leg's destination.
- The two `Build` calls sit in the serialization loop **outside any per-candidate try/catch** (`route_action.cc:1198-1213`) — the #44 failure contract covers only `route_return` (`:1168-1177`) — so one poisoned candidate 500s all 12.
- K-sweep, Belgrade 20 km seed 7: K=1..8 OK, **K=9 → 500**, K=10..11 OK, **K=12 → 500** — bucketing (`buckets = want`) reshuffles the chosen set per K, and the request dies iff a poisoned turnaround is chosen. **K=3 passes all 10 corpus cells that fail at K=12** (both Belgrade-20 and Zlatibor-200 across all seeds).

Prod exposure: the direct-serve path (K=3, `handlers.rs:163`) is mostly shielded by luck of selection; the **Bank Fill path sends `fill_k = 12`** (`cache_layer.rs:460-461`) — for these (start, distance) cells bank fills fail deterministically per seed. Error 499 is not in the orchestrator's definitive-no-route set (by design — transient-vs-definitive gate), so the failure is retried, not cached; Shuffle's fresh seeds re-roll the dice. This is a #44-family bug one layer up: the sink skip checks `edges().empty()` (`route_action.cc:1164`) but a node with *other* exits and a missing *arrival* edge passes it. Fix shape (for a follow-up ticket): add the arrival edge (as end-node PathEdge) to `correlate_node` from the harvested label, and/or wrap the per-loop `Build` pair in the same skip-this-candidate contract as the return leg.

## 9. Slot analysis

Response index = engine rank (loops are stable-sorted by curviness-per-km before serialization, `route_action.cc:1190-1191`); sector-vs-backfill provenance is not observable in the response. Per slot (c0.5):

- Spike rate is **flat 35–51 %** across slots 0–10, with slot 11 worst (56.6 % spike, 8.6 % lollipop) and slot 3 second (51.3 %). Reuse is flat 0.09–0.15.
- **The served surface is not shielded:** slots 0–2 (what direct-serve returns and what the bank serves first) have **44.4 % spike rate, 36.9 % with ≥500 m stub, 20.0 % with ≥2 km stub, 3.6 % lollipop, 8.2 % with reuse >0.3** (n = 561).
- Curviness ranking actively *promotes* one defect flavor: a spur bounce adds curvy switchback kilometers to `curviness_per_km`, so bounced-harvest loops rank well — the flagship being **Golija 300 km slot 0** (the top-served route): a 287.5 km loop carrying a **39.6 km stub**, and because the top pick is curviness-ranked it is **identical for all five seeds** (seed rotation only permutes within top-M per sector; the winner here dominates its sector).
- The T9 min-separation guard did its job on *distinctness* (no byte-identical groups at K=12 in the corpus responses; stale-image Zlatibor-300 showed 5×286.6 km — §1 table) but distinct turnarounds in the same valley still produce highly-overlapping defective loops.

## 10. Worst-case gallery

Nine named repros, saved as GeoJSON (loop LineString + turnaround + spike apexes, repro params in properties). All: K=12, `avoid_motorways` true, engine of §1. Files live **in the fork checkout** under `curvagen-valhalla/tools/defect-atlas/gallery/` (paths below relative to that dir):

| # | File | Repro (origin, target, curv, seed, slot) | Why it's here |
|---|---|---|---|
| A | `A-golija300-slot0-39km-stub.geojson` | golija, 300 km, 0.5, seed 7 (any of the 5), slot 0 | **served-first** loop, 39.6 km stub (13.8 % of 287 km ride out-and-back), seed-invariant |
| B | `B-vlasina200-c08-88km-stub-reuse099.geojson` | vlasina, 200 km, 0.8, seed 11, slot 6 | worst stub in corpus: 88.5 km; loop is 98.7 % reuse — an out-and-back with a bulb |
| C | `C-zlatibor200-c08-44km-stub.geojson` | zlatibor, 200 km, 0.8, seed 7, slot 4 | 44.4 km stub in the massif that also 500s at c0.5 (§8) |
| D | `D-djerdap50-c08-lollipop-stem050-reuse099.geojson` | djerdap, 50 km, 0.8, seed 11, slot 11 | worst lollipop: stem = 49.9 % of ride, reuse 0.988, plus 26.6 km stub |
| E | `E-nis20-lollipop-stem041.geojson` | nis, 20 km, 0.5, seed 7, slot 4 | prod-curviness short-urban lollipop, stem 41 % (hand-verified corridor 0–41 m) |
| F | `F-vlasina20-stem030-reuse083.geojson` | vlasina, 20 km, 0.5, seed 101, slot 1 | short sparse: stem 30 %, reuse 0.834, **no spike** — pure lollipop/reuse case |
| G | `G-novisad300-27km-stub-flatland.geojson` | novisad, 300 km, 0.5, seed 101, slot 4 | 27.2 km stub in *flat grid* terrain — spikes are not mountain-only |
| H | `H-djerdap100-clean-seam-uturn-4km.geojson` | djerdap, 100 km, 0.5, seed 7, slot 2 | textbook pure `seam_uturn`: return opens with a 4.3 km retrace of the forward tail |
| I | `I-vlasina200-c05-26km-stub.geojson` | vlasina, 200 km, 0.5, seed 11, slot 5 | 26.5 km stub + reuse 0.762 + 5 bulbs at **prod curviness** |
| — | (crash, no geojson) | belgrade, 20 km, 0.5, seed 7, **K=12** → HTTP 500/499; same at K=9; fine at K≤8, 10, 11 | §8 request-killer repro |

## 11. Reproduction: scripts, data, engine

Everything lives in the **fork checkout** at `curvagen-valhalla/tools/defect-atlas/` (kept as seed material for the #48 harness; nothing committed):

- `run_corpus.py` — fires the §2 grid at `localhost:8003`, writes `responses/*.json` (raw engine JSON + request + meta; resumable). 232 files, ~5 min wall.
- `atlas.py` — decoders + the §3 detectors (`Loop`, `find_spikes`, `reuse_ratio`, `corridor_stats`, `analyze_loop`).
- `analyze.py` → `loops.jsonl` (2 602 per-loop records, ~1 min); `failures.json` (the 14 dead requests).
- `way_reuse.py` → `way_reuse.jsonl` (ADR-0033 metric via `/trace_attributes`, seeds {7,42} subset).
- `aggregate.py` — prints every table in this doc from `loops.jsonl`.
- `make_gallery.py` — regenerates `gallery/*.geojson`.
- Engine: `docker start curvagen-valhalla-atlas` (container left stopped; image `curvagen-valhalla:fix44`, port 8003, tiles bind-mount `curvagen/Backend/data`). The long-running stale `curvagen-valhalla` container on :8002 was not touched.
- venv: `tools/defect-atlas/venv` (python3 + shapely installed; the shipped detectors ended up dependency-free).

Open follow-ups this atlas motivates: (i) extend shortlist item 1 (turnaround hardening) to the harvest door — walk back bounced labels, not only drop the `correlate_node` opposing edge (§5); (ii) ticket the 499 request-killer with the K-sweep repro (§8) — it gates bank-fill reliability today; (iii) #48 harness should adopt the seam-classified spike detector and the stem/rejoin metrics as regression KPIs, with gallery A/B/D/E as pinned fixtures.

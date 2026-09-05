# Defect atlas v2 — the field defect census on the prod-equivalent engine

- **Date:** 2026-09-06 (curvagen-valhalla [#5](https://github.com/Lazark0x/curvagen-valhalla/issues/5), child of the field-defect census map [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); unblocked by [#9](https://github.com/Lazark0x/curvagen-valhalla/issues/9); blocks [#8](https://github.com/Lazark0x/curvagen-valhalla/issues/8) and [#6](https://github.com/Lazark0x/curvagen-valhalla/issues/6))
- **Scope:** `corpus-v2` (552 requests, K=12, **6 622 loops**) fired at the prod-equivalent engine `valhalla-curvature:prod-equivalent-b4f514d7f`, metered with the pinned harness (`tools/loopqual`, metrics **v1.3**) and the D1–D5 blind-spot detectors, then **classified by mechanism** and read per corpus block, per curviness, per Road Preference and per served slot. Plus: the engine's own per-request Fallback/Second-Via/Defect-Gate ledger for 320 requests, way-id evidence for the F02 class, and the F03 (return-leg optimality) measurement at corpus scale.
- **Method:** engine mode only (exact `legs[0]/legs[1]` seam; serving mode is dead against contract 3.7.1 — `runner.py::build_request` and `Loop.from_serving_route` are both stale). The `/trace_attributes` way pass ran against the **same** engine (`--trace-engine http://localhost:8003`), not `--no-way`. **Nothing under `tools/loopqual/` was modified** — the corpus, `metrics.py` and `runner.py` are the pinned instruments; every new script lives in `~/.curvagen-scratch/`, and the only writes into the repo are this run's own output directory `tools/loopqual/results/census-v2-b4f514d7f/`. Zero production traffic. `valhalla-local` (:8002) was never addressed; the census used its own containers on :8003 and :8004, removed at close.
- **Artifacts:** `tools/loopqual/results/census-v2-b4f514d7f/` (`loops.jsonl`, `report.{json,md}`, `responses/`, `gallery-census-v2.html`, `labeling-sample.jsonl`, `labeling-sample-key.jsonl`) and `~/.curvagen-scratch/censusv2*.{jsonl,json}` — full list in Appendix B.

## TL;DR verdict

**The rider-visible residual is one mechanism, and it is the one no meter and no gate can see: the return rides the other carriageway.** Every gated meter reads ~zero on 6 622 fresh loops, and the census reproduces production's headline number to within 0.1 pp.

1. **The run passes every absolute Gate v1.3 threshold at every curviness**, and one gate input is now identically zero: `spike_ge_30m` **0.0 %** and `max_stub_km` **0.000 on all 6 622 loops** — the v3 Defect Gate plus bounce rejection has eliminated the exact-mirror palindrome as a class (the engine's own ledger shows it rejecting seam-stub builds in 33.8 % of requests). `is_lollipop` 0.7 %, `edge_reuse_geom > 0.30` 2.4 %, mean `edge_reuse_geom` 0.034. In the two rider-shaped blocks those last two read **0.0 % / 0.1 %** and **0.011–0.027**.
2. **The detectors read the opposite, and the number matches production.** `D1b` (near-mirror ≥ 500 m unseen by any meter) fires on **64.1 %** of the demand-cell block and **61.1 %** of the Vračar block; production's 111 served loops read **64.0 %**. Mean unseen near-mirror per loop is **6 629 m** (demand) and **5 755 m** (Vračar) against production's **6 614 m**. The rig reproduces the field defect.
3. **The mechanism is F02, proven by way ids, not inferred from geometry.** On 40 loops carrying ≥ 2 km of mid-route near-mirror at a 5–30 m offset, **99.0 % of the flagged metres are on a different OSM way** than the traversal they mirror (40/40 loops ≥ 96 %); a same-pavement control reads 1.7 % (median). This is F02 — the return leaves the turnaround onto the opposite carriageway of the same physical road, which `route_leg`'s edge-id exclusion, the leash, the Defect Gate, `find_spikes` and `edge_reuse_geom` are all structurally blind to.
4. **Mechanism ranking by ride-fraction:** near-mirror / parallel carriageway (F02) **36.8 % of loops / 3.80 % of all ride km** · fallback-like heavy reuse 11.8 % / 1.25 % · same-pavement retrace mid-route (F01) 5.3 % / 0.71 % · intra-leg ring (F22) 5.6 % / 0.21 % · exempt-zone stem 12.5 % / 0.13 % · seam residue 0.5 % / 0.01 % · passes-home figure-8 0.1 % / 0.01 %. In the Vračar block F02 alone is **52.8 % of loops and 13.35 % of every kilometre ridden**.
5. **corpus-v1 measures a different defect.** With motorways avoided (v1's pin) 80.4 % of the near-mirror mass is same-pavement; in rider mode it is **11–15 %**, and 85–86 % of the mass is mid-route parallel carriageway. Road Preference, not distance or curviness, is the axis that decides which defect a corpus can see: `D1b ≥ 2 km` reads **56.9 % (a0) vs 15.5 % (a1)**.
6. **F20 confirmed, and it cuts both ways.** Same-pavement retrace falls monotonically with served rank — `D1 ≥ 500 m` **40.7 → 33.1 → 26.0 → 19.8 %** across slots 0–2 / 3–5 / 6–8 / 9–11 (production: 60.7 / 31.6 / 26.2 at ranks 4–6 / 7–9 / 10–12) — as do intra-leg rings (13.7 → 3.1 %) and F01 (7.6 → 3.7 %). So the **direct-serve slots are the worst** on every axis the harvest score can see. The near-mirror class runs the *other* way (41.2 → 56.7 %) — it costs the score nothing, so the ranking neither promotes nor demotes it.
7. **The engine ledger, read for the first time:** **25.6 % of all served candidates are Fallback Loops** (98.4 % of requests produce at least one, mean 3.07 of 12). Second Via **never fired** in 320 Belgrade requests. The D4 geometric proxy reads 23.3 % on the same loops and separates fallback-heavy from fallback-free requests 49.2 % vs 3.3 % — but `D1b` does **not** separate them (64.7 % vs 70.0 %), so the near-mirror residual is the primary path's own doing, not a Fallback artefact.
8. **F03 is real and rare.** With `service_limits.min_linear_cost_factor` lowered and a decoy `linear_cost_factors` edge 200 km away (lever proven to land by a positive control), **98.3 % of the 900 Vračar return legs are byte-identical** to the stock engine's. The inadmissible A\* heuristic changes 1.7 % of return legs.

---

## 1. Run provenance and cost

| | |
|---|---|
| Engine | `valhalla-curvature:prod-equivalent-b4f514d7f` (`658afa439b87`, arm64, fork commit `b4f514d7f` = prod's `:amd64`) |
| `/status` | `{"version":"3.8.2","tileset_last_modified":1785932567}` (tiles 2026-08-05, `curvagen-orchestrator/data`, mounted read-only) |
| Container | `rt-census-engine` on **:8003**, `server_threads=2` (`valhalla-local` on :8002 untouched) |
| Corpus | `tools/loopqual/corpus-v2.yaml`, id `corpus-v2`, sha256 `bf40ee110d001a0f69f5b74b32db0112523d00312a8e8c7ab69bc8bf5433c83a` |
| Mode / workers | engine mode, 3 workers, way pass via `--trace-engine http://localhost:8003` |
| Requests | **552 fired, 552 OK, 0 failed** |
| Loops | **6 622** (2 short of 6 624: `vlasina_d50_c0.5_s11` and `…_s101` returned 11 candidates each) |
| Latency | **p50 1.305 s, p95 3.487 s** (corpus-v1 on the b1 engine: 0.959 / 3.134) |
| Wall time | firing **280 s** · analysis **243 s** · way pass **90 s** (2 292 filled, 396 trace-failed = the 300 km loops over the 16 000-point limit) = **10.2 min** |
| Blocks | C = corpus-v1 verbatim **2 782** loops · A = Vračar field cell **1 200** · B = production demand cells **2 640** |

Firing took **2.8×** corpus-v1's 99 s, close to the predicted 2.4×. Determinism was re-verified independently: 40 requests re-fired one at a time later in the session returned candidate length vectors identical to the saved responses (0 mismatches).

---

## 2. Baseline v2 — the candidate reading

This is the table Gate v2 will reference. Every v1.3 meter and every v2 magnitude, overall and per block.

### 2.1 v1.3 meters and v2 magnitudes

| meter | ALL (n=6 622) | block C v1 (2 782) | block A Vračar (1 200) | block B demand (2 640) |
|---|---|---|---|---|
| mean ride km | 101.4 | 138.8 | 40.2 | 89.8 |
| `spike_ge_30m` | **0.0 %** | 0.0 % | 0.0 % | 0.0 % |
| `spike_ge_500m` | **0.0 %** | 0.0 % | 0.0 % | 0.0 % |
| `spike_count` mean | 0.000 | 0.000 | 0.000 | 0.000 |
| `max_stub_km` max | **0.000** | 0.000 | 0.000 | 0.000 |
| `spike_len_fraction` mean | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| `edge_reuse_geom` mean | 0.0342 | 0.0587 | 0.0273 | 0.0114 |
| `edge_reuse_geom` p90 | 0.0960 | 0.1591 | 0.1043 | 0.0417 |
| `edge_reuse_geom > 0.30` | **2.4 %** | 5.6 % | 0.1 % | 0.1 % |
| `edge_reuse_way` mean | 0.0546 | 0.1007 | 0.0434 | 0.0232 |
| `is_lollipop` | **0.7 %** | 1.8 % | 0.0 % | 0.0 % |
| `lollipop_stem_fraction` mean | 0.0048 | 0.0114 | 0.0000 | 0.0000 |
| `bulb_count` mean | 2.14 | 1.72 | 2.60 | 2.37 |
| `shadow_frac` mean | 0.148 | 0.093 | 0.257 | 0.156 |
| `shadow_frac` p90 | 0.446 | 0.262 | 0.570 | 0.405 |
| `rejoin_return_frac` mean | 0.0047 | 0.0112 | 0.0000 | 0.0000 |
| `compactness` mean | 0.235 | 0.244 | 0.193 | 0.244 |
| `compactness < 0.10` | 21.8 % | 14.8 % | **41.7 %** | 20.1 % |
| `distance_error` mean | 0.163 | 0.226 | 0.115 | 0.118 |
| `distance_error` p90 / max | 0.283 / **8.353** | 0.425 / 8.353 | 0.239 / 0.555 | 0.223 / 0.598 |
| `seam_frac` mean | 0.474 | 0.451 | 0.503 | 0.486 |
| `curviness_geom_clean` mean | 270.7 | 347.0 | 258.7 | 195.7 |
| — **v2** unseen near-mirror m (mean / p50 / p90) | 4 230 / 490 / 16 168 | 1 295 / 174 / 2 380 | 5 755 / 3 059 / 16 168 | **6 629 / 3 051 / 19 653** |
| — as a share of the ride (mean / p90) | 0.068 / 0.244 | 0.017 / 0.031 | **0.142 / 0.393** | 0.089 / 0.266 |
| — same-pavement both-ways m (mean) | 3 030 | 5 367 | 1 536 | 1 248 |
| — intra-leg self-overlap m (mean, F01) | 955 | 1 809 | 305 | 351 |
| — intra-leg self-overlap ≥ 500 m | 10.3 % | 14.1 % | 9.8 % | 6.6 % |
| — passes home mid-ride (≤ 200 m) | **0.1 %** | 0.1 % | 0.0 % | 0.0 % |
| — `shadow_frac_loop` (whole loop) mean | 0.141 | 0.083 | 0.252 | 0.150 |
| — seam within 1 km of the ride's farthest point | **22.7 %** | 17.0 % | 33.6 % | 23.8 % |

`curviness_retention` is `null` in engine mode by construction (engine trips carry no curviness field), so the meter is untestable here and remains a serving-layer drift alarm.

### 2.2 D1–D5 detector prevalence

Predicates are **verbatim** from the served-loop reading's `compare_det.py`, so these rows are directly comparable to §6.

| detector fires on | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| D1 retrace: ≥ 1 km ridden both ways (same pavement) | 28.3 % | 31.6 % | 32.2 % | 23.0 % |
| D1 longest one-way same-pavement run ≥ 500 m | 29.9 % | 33.4 % | 33.6 % | 24.6 % |
| D1 longest one-way same-pavement run ≥ 2 km | 20.1 % | 26.9 % | 14.3 % | 15.4 % |
| **D1b near-mirror (r = 25 m) unseen by any meter ≥ 500 m** | **49.6 %** | 30.9 % | **61.1 %** | **64.1 %** |
| D1b near-mirror unseen ≥ 2 km | 37.6 % | 10.4 % | 59.3 % | 56.4 % |
| D1c near-mirror U-turn at the seam ≥ 200 m | 2.2 % | 2.9 % | 3.2 % | 1.1 % |
| D2 exempt-zone shared corridor ≥ 5 % of the ride | 2.5 % | 5.5 % | 0.0 % | 0.5 % |
| D2 reuse hidden by the exemption ≥ 1 km | 6.9 % | 15.9 % | 0.0 % | 0.5 % |
| D3 ≥ 1 near-rejoin ring (IQ ≥ 0.15, ≥ 800 m) | 64.1 % | 46.6 % | 80.4 % | 75.2 % |
| D3 ring inside a single leg | 9.4 % | 10.9 % | 2.9 % | 10.7 % |
| D3 bulb-class ring (≤ 25 % of loop) ≥ 2 km | 40.6 % | 26.0 % | 39.9 % | 56.4 % |
| D3b ≥ 1 transversal self-crossing | 66.7 % | 56.5 % | 66.0 % | 77.8 % |
| D3b figure-8 (crossing lobe ≥ 25 % of loop) | 22.2 % | 16.6 % | 32.7 % | 23.2 % |
| D4 reuse outside the exemption ≥ 100 m (Fallback proxy) | 28.5 % | 35.6 % | 30.5 % | 20.0 % |
| D5 two prominent distance lobes | 2.6 % | 5.1 % | 0.9 % | 0.8 % |
| D6 cross-leg shared corridor ≥ 25 % of the ride | 22.1 % | 9.6 % | 49.2 % | 23.0 % |
| D6 stem zeroed by the 120 m gap but ≥ 1.5 km at gap 1 km | 20.1 % | 11.7 % | 8.8 % | 34.2 % |
| **D6c seam is NOT the ride's farthest point (> 1 km off)** | **77.3 %** | 83.0 % | 66.4 % | 76.2 % |
| — reference: `spike_ge_30m` | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| — reference: `is_lollipop` | 0.7 % | 1.8 % | 0.0 % | 0.0 % |
| — reference: `edge_reuse_geom > 0.30` | 2.4 % | 5.6 % | 0.1 % | 0.1 % |

### 2.3 The offset spectrum — what the near-mirror mass actually is

| mean lateral offset of the near-mirror run | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| < 5 m (same pavement) | 39.4 % | **80.4 %** | 15.4 % | 11.3 % |
| 5–15 m | 34.0 % | 9.9 % | 45.2 % | 51.8 % |
| 15–30 m | 26.6 % | 9.7 % | 39.5 % | 36.9 % |
| > 30 m | 0.0 % | 0.0 % | 0.0 % | 0.0 % |

| zone × pavement (share of all flagged near-mirror metres) | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| inside the Start Exemption | 2.6 % | 3.1 % | 0.1 % | 3.3 % |
| at the seam, same pavement | 14.1 % | 26.8 % | 6.3 % | 5.5 % |
| at the seam, parallel | 0.2 % | 0.2 % | 0.4 % | 0.1 % |
| **mid-route, same pavement** | 24.6 % | 52.5 % | 9.0 % | 5.0 % |
| **mid-route, parallel (≥ 5 m)** | **58.5 %** | 17.5 % | **84.2 %** | **86.0 %** |

### 2.4 Way-id proof of the F02 class

`/trace_attributes` (`map_snap`, `edge.way_id`) on 40 loops carrying ≥ 2 km of mid-route near-mirror at ≥ 5 m offset, and a 14-loop same-pavement control:

| class | loops | flagged run metres | **different OSM way** | same way |
|---|---|---|---|---|
| parallel (5–30 m offset) | 40 | 1 213.7 km | **99.0 %** (40/40 loops ≥ 96 %, median 100 %) | 1.0 % |
| same-pavement control (< 5 m offset) | 14 | 798.6 km | 46.2 % aggregate, **median 1.7 %** | 53.8 % |

The control's aggregate is dominated by four `vlasina / 50 km / c0.8` loops whose single flagged run is 84–98 km long, where the run's partner index has to be interpolated and the way lookup becomes meaningless; the other 9 of 14 read ≤ 3 % different-way. The parallel-class result does not depend on that interpolation — every partner in the corridor is on the other carriageway either way.

### 2.5 The run against Gate v1.3 itself

`gate_v1_proto.py results/b1-x02 results/census-v2-b4f514d7f` — **every absolute threshold passes, at every curviness**:

| gate | c0.5 | c0.7 | c0.8 | c1.0 |
|---|---|---|---|---|
| 1 `spike_ge_500m == 0` | 0.00 % PASS | 0.00 % PASS | 0.00 % PASS | 0.00 % PASS |
| 2 `spike_ge_30m ≤ 2 %` | 0.00 % PASS | 0.00 % PASS | 0.00 % PASS | 0.00 % PASS |
| 3 reuse mean @20 km ≤ 0.12 | 0.1074 PASS | — | — | — |
| 3 reuse mean @50 km ≤ 0.10 | 0.0502 PASS | 0.0304 PASS | 0.0948 PASS | 0.0348 PASS |
| 4 reuse mean @100/200/300 km ≤ 0.05 | 0.0243 / 0.0220 / 0.0245 PASS | 0.0054 PASS | 0.0351 PASS | — |
| 5 `reuse > 0.30` ≤ 8 % | 3.05 % PASS | 0.00 % PASS | 7.55 % PASS | 0.32 % PASS |
| 6a lollipop ≤ 2 % | 1.05 % PASS | 0.00 % PASS | 1.30 % PASS | 0.00 % PASS |
| 6b lollipop worst cell ≤ 55 % | 48.3 % (`vlasina`-50 km) PASS | 0.0 % PASS | 20.8 % PASS | 0.0 % PASS |
| 7 `dist_err` mean / p90 | 0.169 / 0.292 PASS | 0.108 / 0.196 PASS | 0.301 / 0.536 PASS | 0.135 / 0.265 PASS |

The five reported FAILs are all **relative** gates (8 curviness-held, 9 bank distinctness, 10 latency) measured against `results/b1-x02` — a *different corpus* on a *different engine* and *different tiles*. They are not quality signals here and are quoted only so nobody re-derives them as one; no like-for-like corpus-v2 baseline exists yet, and this run **is** that baseline.

**The finished sentence for the grilling:** this engine passes Gate v1.3 on every absolute threshold at every curviness, on a corpus built from what riders actually ask for, while 64.1 % of the demand-cell loops carry ≥ 500 m of retrace that no gate input can see.

---

## 3. Mechanism classification

Every loop is assigned the **dominant** mechanism by the ride metres attributable to it. The magnitudes are disjoint by construction — intra-leg vs cross-leg undirected reuse, and exempt vs seam vs mid zone — so no metre is counted twice. Two classes are qualitative overrides taken first because their metre mass understates them: seam residue and passes-home. A loop is `clean` when no magnitude reaches 500 m *and* no detector fires; `other` when a detector fires but no mechanism carries 500 m (overwhelmingly D3/D3b, which fire on 64–67 % of all loops).

Each cell is **share of loops / share of that population's total ride kilometres**.

| mechanism | signature | **n (ALL)** | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|---|---|
| **near-mirror / parallel carriageway (F02)** | anti-parallel riding at 5–30 m offset, mid-route, different OSM way | **2 436** | **36.8 % / 3.80 %** | 14.1 % / 0.71 % | **52.8 % / 13.35 %** | **53.4 % / 6.88 %** |
| fallback-like heavy reuse | same undirected pavement in **both** legs, outside the exemption (soft-leash return) | 782 | 11.8 % / 1.25 % | 20.3 % / 1.93 % | 8.2 % / 0.77 % | 4.5 % / 0.25 % |
| same-pavement retrace mid-route (F01) | undirected segment ridden twice **inside one leg**, outside the exemption | 348 | 5.3 % / 0.71 % | 8.3 % / 1.01 % | 3.1 % / 0.43 % | 3.0 % / 0.28 % |
| intra-leg ring / near-rejoin (F22) | closed ring (IQ ≥ 0.15, ≥ 800 m) wholly inside one leg | 369 | 5.6 % / 0.21 % | 4.1 % / 0.22 % | 3.5 % / 0.31 % | 8.1 % / 0.19 % |
| exempt-zone stem | cross-leg corridor inside the 1.5 km Start Exemption | 827 | 12.5 % / 0.13 % | 20.4 % / 0.15 % | 0.0 % / 0.00 % | 9.8 % / 0.13 % |
| seam residue | near-mirror U-turn at the turnaround (**never** an exact mirror — `find_spikes` is 0 everywhere) | 32 | 0.5 % / 0.01 % | 0.4 % / 0.00 % | 0.9 % / 0.05 % | 0.4 % / 0.01 % |
| passes-home figure-8 (F06) | ride within 200 m of the start between 10 % and 90 % of its length | **4** | **0.1 % / 0.01 %** | 0.1 % / 0.01 % | 0.0 % | 0.0 % |
| other (detector fires, < 500 m of any mechanism) | | 1 506 | 22.7 % / 0.06 % | 28.6 % / 0.05 % | 24.0 % / 0.13 % | 15.9 % / 0.05 % |
| clean | | 318 | 4.8 % | 3.7 % | 7.4 % | 4.7 % |

Per-block counts: **C** (2 782) other 797 · exempt stem 568 · fallback-like 564 · F02 391 · F01 231 · F22 113 · clean 104 · seam 10 · home 4. **A** (1 200) F02 634 · other 288 · fallback-like 99 · clean 89 · F22 42 · F01 37 · seam 11. **B** (2 640) F02 1 411 · other 421 · exempt stem 259 · F22 214 · fallback-like 119 · clean 125 · F01 80 · seam 11.

At **request** level (the loop a rider is handed first, slot 0 of 552 requests): F02 133 · fallback-like 94 · F01 66 · exempt stem 61 · F22 59 · clean 20 · other 119.

**"Fallback-like" is a geometry class, not the engine's tag.** F12 stands: the Fallback flag never leaves the engine, so no per-candidate ground truth exists on the wire. It *does* exist in the log, and this census read it for the first time — but only per request. Capturing `docker logs -f` during the 3-worker main run was **not** enough to attribute a `roundtrip: N candidate(s) fell back to the soft leash` line to a request: with three concurrent requests the INFO lines interleave and the response line carries only a connection id and a byte count. Re-firing the 320 block-A/B requests **one at a time** and slicing the log stream between them does attribute them exactly, and determinism per seed (F18) makes that valid for the saved loops (40/40 re-fires byte-identical). The result is §7.5; the per-candidate mapping remains unavailable without the wire change (M11).

**F01's leg attribution confirms the delta survey's prediction.** The intra-leg self-overlap lives in the **forward** leg:

| F01 signature | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| ≥ 500 m of overlap in leg0 (forward) | **9.2 %** | 11.5 % | 9.8 % | 6.5 % |
| ≥ 500 m of overlap in leg1 (return) | 1.3 % | 3.1 % | 0.0 % | 0.1 % |
| mean metres, leg0 / leg1 | 806 / 149 | 1 456 / 353 | 303 / 1 | 348 / 3 |
| a ring sits in the same leg as the overlap | 51.4 % | 55.7 % | 33.9 % | 53.4 % |

A 5.4× forward-leg excess is exactly F01: the harvest Dijkstra labels a homeward edge through a legal reversal (a ring, a roundabout, a triangle junction), and nothing downstream rejects a non-simple chain. Half of those chains carry a visible ring at the apex; the other half reverse at a junction too small for the ring detector's 800 m floor.

### 3.1 By curviness and by Road Preference

| mechanism | c0.5 (4 198) | c0.7 (780) | c0.8 (384) | c1.0 (1 260) |
|---|---|---|---|---|
| near-mirror / parallel carriageway (F02) | 35.5 % / 3.45 % | **57.3 % / 8.29 %** | 12.2 % / 0.38 % | 35.8 % / 4.84 % |
| fallback-like heavy reuse | 14.3 % / 1.34 % | 5.1 % / 0.26 % | **20.3 % / 2.81 %** | 5.1 % / 0.41 % |
| same-pavement retrace mid-route (F01) | 5.9 % / 0.78 % | 3.2 % / 0.21 % | 5.5 % / 1.04 % | 4.4 % / 0.45 % |
| intra-leg ring (F22) | 4.5 % / 0.20 % | 5.8 % / 0.17 % | 4.7 % / 0.16 % | **9.4 % / 0.39 %** |
| exempt-zone stem | 12.5 % / 0.10 % | 5.3 % / 0.08 % | 29.4 % / 0.26 % | 11.7 % / 0.27 % |

c0.8 exists only in block C (motorways avoided, mountain-heavy), so its column is a terrain effect, not a curviness effect. Within rider mode the near-mirror class peaks at **c0.7**.

| mechanism | motorways allowed (a0, 3 540) | motorways avoided (a1, 3 082) |
|---|---|---|
| near-mirror / parallel carriageway (F02) | **53.1 % / 7.67 %** | 18.1 % / 1.14 % |
| fallback-like heavy reuse | 5.5 % / 0.33 % | **19.0 % / 1.89 %** |
| same-pavement retrace mid-route (F01) | 3.1 % / 0.31 % | 7.7 % / 0.98 % |
| exempt-zone stem | 7.3 % / 0.11 % | 18.4 % / 0.14 % |

**This is the single most consequential split in the census.** 93 % of production demand rides `a0`. corpus-v1 pins `a1` on every job and therefore measures a defect population in which the dominant rider-visible mechanism is **2.9× rarer and carries 6.7× less ride** — while the classes v1 *does* see (fallback-like reuse 3.5×, F01 2.5×, exempt-zone stem 2.5×) are all over-weighted. §5.3 shows this is terrain, not costing.

---

## 4. Served slot

Slots 0–2 are the direct serve (K=3); a Bank tap serves the three lowest remaining ranks, i.e. slots 3–5.

| detector / meter | slots 0–2 (direct serve) | slots 3–5 (first Bank tap) | slots 6–8 | slots 9–11 |
|---|---|---|---|---|
| n | 1 656 | 1 656 | 1 656 | 1 654 |
| D1 same-pavement run ≥ 500 m | **40.7 %** | 33.1 % | 26.0 % | **19.8 %** |
| D1 same-pavement run ≥ 2 km | **31.8 %** | 19.9 % | 15.8 % | 12.8 % |
| D1b unseen near-mirror ≥ 500 m | **41.2 %** | 47.8 % | 52.8 % | **56.7 %** |
| D1b unseen near-mirror ≥ 2 km | 29.8 % | 31.8 % | 40.6 % | 48.1 % |
| D3 ring inside a single leg | **13.7 %** | 8.5 % | 7.4 % | 7.9 % |
| D3b figure-8 (lobe ≥ 25 %) | **11.8 %** | 22.0 % | 27.7 % | 27.1 % |
| D4 Fallback proxy | 27.4 % | **34.2 %** | 28.6 % | 23.6 % |
| D6 cross-leg corridor ≥ 25 % | 17.9 % | 15.6 % | 23.7 % | **31.1 %** |
| `edge_reuse_geom` mean | 0.0378 | **0.0396** | 0.0345 | 0.0247 |
| `is_lollipop` | 1.1 % | 0.8 % | 0.6 % | 0.4 % |
| `compactness < 0.10` | **13.3 %** | 19.7 % | 25.1 % | **29.1 %** |
| unseen near-mirror m (mean) | 3 575 | 2 759 | 4 411 | **6 176** |
| intra-leg overlap m (mean, F01) | **1 088** | 1 066 | 996 | 670 |
| `curviness_geom_clean` mean | **338.0** | 283.9 | 242.9 | 217.8 |

| mechanism | slots 0–2 | slots 3–5 | slots 6–8 | slots 9–11 |
|---|---|---|---|---|
| near-mirror / parallel carriageway (F02) | 25.8 % / 3.11 % | 31.6 % / 2.39 % | 41.7 % / 4.10 % | **48.1 % / 5.63 %** |
| fallback-like heavy reuse | 13.2 % / 1.18 % | **15.7 % / 1.56 %** | 10.6 % / 1.43 % | 7.7 % / 0.85 % |
| same-pavement retrace mid-route (F01) | **7.6 % / 0.91 %** | 4.6 % / 0.82 % | 5.1 % / 0.61 % | 3.7 % / 0.49 % |
| intra-leg ring (F22) | **9.4 % / 0.24 %** | 5.6 % / 0.27 % | 4.3 % / 0.25 % | 3.1 % / 0.09 % |
| seam residue | 0.0 % | 0.1 % | 0.2 % | **1.6 %** |
| clean | **8.5 %** | 3.6 % | 4.9 % | 2.2 % |

**F20 is confirmed and sharpened.** The harvest-chain curviness sort promotes exactly the chains F01 describes: same-pavement retrace, intra-leg rings, and the highest `curviness_geom_clean` all peak in slots 0–2 and decay monotonically. But the promotion is *selective*: the near-mirror/F02 class and cross-leg corridor sharing both **increase** with rank, because a parallel-carriageway return contributes no curvy kilometres to the harvest score. Slot 0 is not uniformly worse — it is worse on everything the ranking can see and better on the one thing it cannot.

The slot-0 population is **bimodal**, which is why it holds both the highest same-pavement retrace *and* the highest clean rate (8.5 % vs 2.2 % at slots 9–11): the curviness sort puts the F01 chains and the genuinely good curvy loops in the same three slots, and the deep bank holds what is left — mostly F02, mostly not clean.

---

## 5. The Vračar deep-dive

Block A, `vracar` = cell **(44.798, 20.472)**, rider mode (motorways allowed), 25/30/40/50/60 km × c{0.5, 0.7, 1.0} × 5 seeds × K=12 = 900 loops; plus `vracar_amw`, the same 25–60 km × c0.5 with motorways avoided (300 loops), as the Road Preference control.

### 5.1 By distance rung

| detector | 25 km | 30 km | 40 km | 50 km | 60 km |
|---|---|---|---|---|---|
| D1 same-pavement run ≥ 500 m | 23.3 % | 27.2 % | **46.7 %** | 37.2 % | 28.9 % |
| D1 same-pavement run ≥ 2 km | 0.0 % | 5.6 % | **27.2 %** | 16.1 % | 19.4 % |
| **D1b unseen near-mirror ≥ 500 m** | 55.0 % | 62.2 % | **64.4 %** | 60.0 % | 58.9 % |
| D1b unseen near-mirror ≥ 2 km | 52.8 % | 57.8 % | **63.3 %** | 59.4 % | 58.3 % |
| D2 exempt-zone corridor ≥ 5 % | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| D3 ≥ 1 near-rejoin ring | 78.3 % | 80.0 % | 86.7 % | 77.2 % | 77.8 % |
| D3 ring inside a single leg | 2.2 % | 2.2 % | **6.7 %** | 3.3 % | 2.8 % |
| D3b ≥ 1 self-crossing | 61.7 % | 71.1 % | 75.0 % | 58.3 % | 65.6 % |
| D4 Fallback proxy | 23.3 % | 32.8 % | **42.2 %** | 28.3 % | 22.2 % |
| D5 two lobes | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 3.9 % |
| D6 cross-leg corridor ≥ 25 % | 52.2 % | 53.9 % | 46.7 % | 38.9 % | 44.4 % |
| — `edge_reuse_geom > 0.30` | 0.0 % | 0.0 % | 0.6 % | 0.0 % | 0.0 % |
| — `is_lollipop` / `spike_ge_30m` | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| unseen near-mirror m (mean) | 3 028 | 4 141 | 6 242 | 6 267 | **8 024** |
| unseen near-mirror / ride (mean) | 0.120 | 0.143 | **0.158** | 0.124 | 0.141 |
| `compactness < 0.10` | 46.7 % | 45.0 % | **49.4 %** | 36.7 % | 30.6 % |
| mean ride km / `distance_error` mean | 26.2 / 0.105 | 29.0 / 0.122 | 40.2 / 0.123 | 49.5 / 0.096 | 57.0 / 0.136 |

**40 km is the hard rung.** It carries the peak of every retrace detector and of the Fallback proxy, and the lowest compactness. 25 km is the mildest on same-pavement retrace (D1 ≥ 2 km is literally 0.0 %) yet still fires D1b on 55 % of loops — even a 25 km city loop rides 12 % of its length on the mirror carriageway.

### 5.2 By curviness (rider mode)

| detector | c0.5 (300) | c0.7 (300) | c1.0 (300) |
|---|---|---|---|
| D1 same-pavement run ≥ 500 m | 36.7 % | 31.7 % | 29.7 % |
| **D1b unseen near-mirror ≥ 500 m** | **70.0 %** | 63.3 % | 47.0 % |
| D1b unseen near-mirror ≥ 2 km | **68.7 %** | 60.3 % | 46.0 % |
| D3 ≥ 1 near-rejoin ring | 82.7 % | 81.7 % | 75.7 % |
| D3 ring inside a single leg | 2.0 % | 2.7 % | **5.7 %** |
| D3b figure-8 | **36.3 %** | 31.3 % | 28.7 % |
| D6 cross-leg corridor ≥ 25 % | **56.7 %** | 51.0 % | 34.0 % |
| D6 stem zeroed by the 120 m gap | 15.7 % | 6.7 % | 3.3 % |

Turning curviness **up** reduces the near-mirror defect (70.0 → 47.0 %) and increases intra-leg rings (2.0 → 5.7 %): a curvier objective pulls the loop off the boulevards onto the smaller street net, where the parallel-carriageway trap does not exist but the ring detour does. c0.5 is prod's default and 87 of 163 real requests.

### 5.3 The Road Preference control at the field cell

| detector | `vracar` a0 (rider mode, 900) | `vracar_amw` a1 (control, 300) |
|---|---|---|
| D1b unseen near-mirror ≥ 500 m | 60.1 % | 64.0 % |
| D1b unseen near-mirror ≥ 2 km | 58.3 % | 62.3 % |
| D1 same-pavement run ≥ 2 km | 13.7 % | 16.3 % |
| D6 cross-leg corridor ≥ 25 % | 47.2 % | 55.3 % |
| `edge_reuse_geom > 0.30` | 0.1 % | 0.0 % |

**At the field cell the Road Preference makes almost no difference** — 60.1 % vs 64.0 %. The a0/a1 split in §3.1 is therefore a *terrain* proxy (block C's mountains vs Belgrade's grid), not a costing effect. In central Belgrade both Road Preferences ride the same dual carriageways, because that is what the city is made of.

### 5.4 Worst loop per rung × curviness

Named as block/cell/distance/curviness/seed/slot. "mech m" = ride metres attributed to the dominant mechanism.

| rung | curviness | seed/slot | ride km | dominant mechanism | mech m | `edge_reuse_geom` | unseen near-mirror m | spike? |
|---|---|---|---|---|---|---|---|---|
| 25 km | c0.5 | s101/slot9 | 24.5 | F02 near-mirror | 11 274 | 0.000 | 11 274 (46 % of ride) | no |
| 25 km | c0.7 | s101/slot9 | 24.5 | F02 near-mirror | 11 274 | 0.000 | 11 274 | no |
| 25 km | c1.0 | s101/slot10 | 24.5 | F02 near-mirror | 11 274 | 0.000 | 11 274 | no |
| 30 km | c0.5 | s23/slot7 | 33.5 | F02 near-mirror | 14 606 | 0.011 | 14 606 | no |
| 30 km | c0.7 | s23/slot6 | 33.5 | F02 near-mirror | 14 606 | 0.011 | 14 606 | no |
| 30 km | c1.0 | s11/slot3 | 29.4 | F02 near-mirror | 15 302 | 0.091 | 15 345 (52 %) | no |
| 40 km | c0.5 | s11/slot10 | 36.1 | F02 near-mirror | 22 801 | 0.053 | 21 095 (58 %) | no |
| 40 km | c0.7 | s101/slot10 | 43.4 | F02 near-mirror | 20 906 | 0.000 | 21 217 | no |
| 40 km | c1.0 | s101/slot11 | 41.4 | F02 near-mirror | 21 465 | 0.000 | 21 465 | no |
| **50 km** | **c0.5** | **s42/slot2** | **52.9** | **F02 near-mirror** | **28 171** | **0.000** | **28 171 (53 %)** | **no** |
| 50 km | c0.7 | s23/slot9 | 55.8 | F02 near-mirror | 21 870 | 0.000 | 21 870 | no |
| 50 km | c1.0 | s11/slot11 | 56.9 | F02 near-mirror | 21 281 | 0.000 | 21 281 | no |
| 60 km | c0.5 | s101/slot5 | 52.2 | F02 near-mirror | 25 459 | 0.000 | 25 459 | no |
| 60 km | c0.7 | s101/slot6 | 52.2 | F02 near-mirror | 25 459 | 0.000 | 25 459 | no |
| 60 km | c1.0 | s23/slot10 | 54.8 | F02 near-mirror | 21 281 | 0.000 | 21 281 | no |

**Fifteen rungs, fifteen F02.** The bolded row is the one to argue about: `A / (44.798, 20.472) / 50 km / c0.5 / s42 / slot 2` is a **direct-serve** candidate (K=3 reads slots 0–2) that rides 28.2 km of its 52.9 km on the opposite carriageway of the outbound road, with `edge_reuse_geom` **0.000**, zero spikes, zero stem — it reads the *best possible value* on every per-loop meter Gate v1.3 consumes. Two more Vračar loops break 55 % of the ride: `A / 60 km / c0.7 / s101 / slot 11` (58.9 % — classified seam residue because its near-mirror mass sits across the turnaround) and `A / 40 km / c0.5 / s11 / slot 10` (58.4 %).

---

## 6. The rig vs the 111 real served loops

Comparing the census's rider-shaped blocks against the production cache reading (`2026-09-05-prod-served-loops-reading.md` §5.1). Production is serving-mode geometry with a derived seam; census rows marked ¹ are the ones that depend on the seam and are therefore only approximately comparable.

| detector | **production served (111)** | census block B demand (2 640) | census block A Vračar (1 200) | census block C v1 (2 782) | b1-x02 corpus-v1 (2 779) |
|---|---|---|---|---|---|
| D1 ≥ 1 km ridden both ways | 25.2 % | 23.0 % | 32.2 % | 31.6 % | 31.1 % |
| D1 one-way run ≥ 500 m | 37.8 % | 24.6 % | 33.6 % | 33.4 % | 32.9 % |
| D1 one-way run ≥ 2 km | 8.1 % | 15.4 % | 14.3 % | 26.9 % | 25.9 % |
| **D1b unseen near-mirror ≥ 500 m** | **64.0 %** | **64.1 %** | 61.1 % | 30.9 % | 32.5 % |
| **D1b unseen near-mirror ≥ 2 km** | **51.4 %** | **56.4 %** | 59.3 % | 10.4 % | 8.8 % |
| D1c seam near-mirror ≥ 200 m ¹ | 3.6 % | 1.1 % | 3.2 % | 2.9 % | 2.7 % |
| D2 exempt corridor ≥ 5 % | 1.8 % | 0.5 % | 0.0 % | 5.5 % | 7.2 % |
| D3 ≥ 1 near-rejoin ring | 82.9 % | 75.2 % | 80.4 % | 46.6 % | 49.6 % |
| D3 bulb-class ring ≥ 2 km | 71.2 % | 56.4 % | 39.9 % | 26.0 % | 24.8 % |
| D3b ≥ 1 self-crossing | 84.7 % | 77.8 % | 66.0 % | 56.5 % | 62.1 % |
| D4 Fallback proxy | 37.8 % | 20.0 % | 30.5 % | 35.6 % | 35.2 % |
| D6 cross-leg corridor ≥ 25 % ¹ | 39.6 % | 23.0 % | 49.2 % | 9.6 % | 11.6 % |
| — `spike_ge_30m` | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| — `is_lollipop` | 0.0 % | 0.0 % | 0.0 % | 1.8 % | 0.3 % |
| — `edge_reuse_geom > 0.30` | 0.0 % | 0.1 % | 0.1 % | 5.6 % | 7.1 % |
| **unseen near-mirror m (mean)** | **6 614** | **6 629** | 5 755 | 1 295 | 1 217 |
| **… as a share of the ride (mean)** | **0.131** | 0.089 | **0.142** | 0.017 | 0.017 |
| … p90 share | 0.420 | 0.266 | 0.393 | 0.031 | 0.029 |
| mean ride km | 61.4 | 89.8 | 40.2 | 138.8 | — |

**The rig reproduces production's headline to within 0.1 pp** (64.1 % vs 64.0 %) and its absolute magnitude to within 0.2 % (6 629 m vs 6 614 m). The **share of ride** differs (0.089 vs 0.131) purely because block B's mean loop is 89.8 km against production's 61.4 km; block A, whose 40.2 km mean brackets the field cell, reads **0.142** — production's 0.131 sits between the two blocks, exactly where a 61 km mean should put it.

Where the rig reads **lower** than production: `D1 ≥ 500 m` (24.6 % vs 37.8 %), `D3b` (77.8 % vs 84.7 %), `D4` (20.0 % vs 37.8 %), `D6` (23.0 % vs 39.6 %). Three explanations, in order of confidence: (a) production's 111 loops are **108 bank candidates at ranks 4–12**, and this census shows same-pavement retrace concentrating in the *low* slots — the cache's rank mix is not the corpus's; (b) the two seam-dependent rows (D1c, D6) are measured against a derived seam in production and an exact one here, and the census now measures that error directly (**77.3 % of loops have their farthest point more than 1 km from the true seam**); (c) n = 111 over 14 prefixes.

Where the rig reads **higher**: `D1 ≥ 2 km` (15.4 % vs 8.1 %) and `D1b ≥ 2 km` (56.4 % vs 51.4 %) — both consistent with the longer mean loop.

The corpus-v1 slice is where the two readings diverge completely, and it re-confirms the prod-cache task's warning: block C reads 30.9 % on the axis production reads 64.0 %. **A census run only on corpus-v1's terrain and Road Preference under-reports the rider's defect by 2.1×.** Block C's `belgrade` origin alone reads 71.3 % / 46.0 %, matching b1-x02's belgrade slice (70.1 % / 39.9 %) — the terrain, not the engine generation, is what carries it.

---

## 7. What did not reproduce, and what surprised us

1. **The exact-mirror spike is extinct.** `spike_ge_30m` is 0.0 % and `max_stub_km` is exactly 0.000 across **all 6 622 loops**, in every block, every distance, every curviness, every slot. The engine's own ledger explains why: the Defect Gate rejects at least one seam-stub build in 33.8 % of requests. Gate v1's headline meter is now a tautology on this engine — it can only ever certify what the engine already refuses to emit. Every "seam residue" case in §3 is a *near*-mirror U-turn, at a 5–30 m offset, which `find_spikes` cannot see by construction.
2. **F06 / Second Via did not reproduce at all.** Passes-home fires on **0.1 %** of loops (4 of 6 622, all in block C `djerdap`), and the engine ledger shows **Second Via rebuilt 0 loops in 320 Belgrade requests**. The mechanism is real (the audit's G4 pinned it in gurka) but it is gated behind an over-stem trigger that never fires where riders ride — Belgrade's `lollipop_stem_fraction` is 0.0000. F06 should be reclassified from "major, rider-visible" to "structurally unreachable on production demand".
3. **F07 (long first edge) did not reproduce**, as the rig-confirmation report already predicted: block A/B `D2 reuse hidden by the exemption ≥ 1 km` reads 0.0 % / 0.5 % — there are no 2–5 km access edges in a city.
4. **The Start Exemption is not the problem anyone thought.** `exempt-zone stem` classifies 12.5 % of loops but carries **0.13 % of ride kilometres**, and D2's ≥ 5 % test fires on 0.0 % of Vračar loops. The blind-spots doc's M4 proposal (re-spec the exemption as `min(1500, 0.05 × target)`) is defensible on principle but would buy essentially nothing on rider-shaped demand; the exemption's cost is concentrated in the 20 km rung of the v1 corpus (25.2 % at d20 vs ~0 % everywhere above 40 km).
5. **Fallback Loops are far more common than anyone assumed, and mostly harmless.** The ledger says **25.6 % of all served candidates fell back to the soft leash** and 98.4 % of requests produce at least one. The geometric proxy (D4) reads 23.3 % in aggregate — a good aggregate calibration for M11 — and separates fallback-heavy from fallback-free requests 49.2 % vs 3.3 %. But the near-mirror detector does **not** separate them (64.7 % vs 70.0 %): the rider-visible residual is not caused by the Fallback path.
6. **The ranking promotes *and* demotes.** The expectation from F20 was "slot 0 is worst". Slot 0 is worst on same-pavement retrace, intra-leg rings and F01 — and simultaneously the **cleanest** on the near-mirror axis (41.2 % vs 56.7 % at slots 9–11) and the most likely to be clean outright (8.5 % vs 2.2 %). A curviness-ranked bank is a bank sorted by *one* defect axis.
7. **The near-rejoin ring detector is not gate-grade as specified.** `near_rejoin_rings`' overlap guard tests only the two endpoint indices, so one physical near-rejoin is reported as a nest of dozens of shifted rings (a 52.6 km Belgrade loop returns 19 rings of 25.7–26.2 km, all the same feature). Counts and totals are inflated; only the per-side **maximum** is usable. D3 as currently written fires on 64.1 % of all loops and mostly detects that a round trip is round.
8. **Increasing curviness *reduces* the dominant defect.** c0.5 → c1.0 at the field cell takes D1b from 70.0 % to 47.0 %. The intuition that a curvier ask produces messier loops is backwards for this mechanism.
9. **All 12 candidates of a request are not 12 independent measurements of this defect.** The mean number of distinct `retrace_unseen_m` values among a request's 12 candidates is 8.18, and 2 of 552 requests return one value for all twelve (every candidate shares the same outbound corridor and the same mirrored return). At request level, **91.5 % of requests hand out at least one candidate with ≥ 500 m of unseen near-mirror**, but only 0.7 % have all twelve.

---

## 8. Inputs for Gate v2

### 8.1 Which meters separate defective from clean

"Defective" = a dominant mechanism carrying ≥ 500 m (n = 4 798); "clean" = no detector fires at all and no mechanism reaches 500 m (n = 318). The AUC is partly circular for the three meters that define the classes (marked †) and should be read as a ranking of the *others* against them.

| meter | mean on defective | mean on clean | AUC |
|---|---|---|---|
| **D1b unseen near-mirror m** † | 5 812 | 33 | **0.922** |
| **D3b transversal crossing count** | 1.71 | 0.00 | **0.920** |
| **D1b unseen near-mirror / ride** † | 0.094 | 0.001 | **0.915** |
| **`shadow_frac_loop`** (whole-loop, ungated today) | 0.189 | 0.008 | **0.909** |
| D3 ring count | 17.85 | 0.00 | 0.880 |
| **`compactness`** (already metered, ungated) | 0.196 | 0.398 | **0.861** (inverted) |
| D1 max same-pavement run m | 1 869 | 7 | 0.750 |
| ring inside one leg m | 781 | 0 | 0.632 |
| **`edge_reuse_geom`** (the current gate 3/5 input) | 0.047 | 0.000 | **0.663** |
| F01 intra-leg overlap m † | 1 318 | 1 | 0.554 |

**`edge_reuse_geom` — the meter Gate v1 actually gates on — is the weakest separator of every meter in the table that is not itself part of the class definition** (0.663 against 0.909, 0.880, 0.861 and 0.750). Two of the meters that beat it, `shadow_frac` promoted to the whole loop and `compactness`, are **already computed by `metrics.py` today and simply not gated**; they cost nothing to add.

### 8.2 Threshold prevalence curves

Share of loops at or above each threshold. Pick a gate form by reading the block the gate is meant to protect (B is production demand).

**M3 `retrace_unseen_m` (D1b, r = 25 m) — the axis v3 never moved**

| threshold | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| 500 m | 49.6 % | 30.9 % | 61.1 % | 64.1 % |
| 1 km | 41.9 % | 18.4 % | 60.4 % | 58.3 % |
| **2 km** | **37.6 %** | 10.4 % | 59.3 % | 56.4 % |
| 5 km | 28.1 % | 8.2 % | 44.2 % | 41.8 % |
| 10 km | 16.1 % | 3.9 % | 20.2 % | 26.9 % |

**M3f `retrace_unseen / ride`** — the length-normalised form, which is what a rider experiences

| threshold | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| 0.02 | 39.2 % | 13.0 % | 60.9 % | 56.8 % |
| 0.05 | 31.4 % | 7.6 % | 59.3 % | 43.8 % |
| **0.10** | **23.5 %** | 4.5 % | 48.4 % | 32.1 % |
| 0.20 | 13.5 % | 2.2 % | 33.1 % | 16.4 % |
| 0.30 | 7.8 % | 1.3 % | 20.0 % | 9.1 % |

**M1 `max_retrace_run_m` (D1, r = 10 m)** · **M6 max ring inside one leg** · **M9 `shadow_frac_loop`** · **M13 `compactness`** · **M4 exempt corridor** · gate 5 `edge_reuse_geom`

| threshold | ALL | C | A | B | | threshold | ALL | C | A | B |
|---|---|---|---|---|---|---|---|---|---|---|
| M1 ≥ 500 m | 29.9 % | 33.4 % | 33.6 % | 24.6 % | | M9 ≥ 0.10 | 37.5 % | 19.8 % | 63.3 % | 44.4 % |
| M1 ≥ 2 km | 20.1 % | 26.9 % | 14.3 % | 15.4 % | | M9 ≥ 0.25 | 22.1 % | 9.6 % | 49.2 % | 23.0 % |
| M1 ≥ 4 km | 9.8 % | 20.8 % | 1.4 % | 2.0 % | | M9 ≥ 0.40 | 11.7 % | 6.4 % | 28.2 % | 9.7 % |
| M6 ≥ 800 m | 20.8 % | 15.7 % | 14.2 % | 29.2 % | | M13 < 0.15 | 31.0 % | 21.8 % | 55.5 % | 29.6 % |
| M6 ≥ 1.5 km | 10.5 % | 11.3 % | 12.3 % | 8.9 % | | M13 < 0.10 | 21.8 % | 14.8 % | 41.7 % | 20.1 % |
| M6 ≥ 2.5 km | 6.9 % | 5.6 % | 9.3 % | 7.2 % | | M13 < 0.05 | 13.5 % | 9.1 % | 27.3 % | 11.7 % |
| M4 ≥ 0.05 | 2.5 % | 5.5 % | 0.0 % | 0.5 % | | reuse > 0.30 | 2.4 % | 5.6 % | 0.1 % | 0.1 % |
| M4 ≥ 0.15 | 0.4 % | 1.0 % | 0.0 % | 0.0 % | | reuse > 0.10 | 9.8 % | 15.1 % | 11.5 % | 3.3 % |

### 8.3 Recommendations to take into the Gate v2 grilling

1. **Make M3/M3f blocking, and set the ratchet on block B, not on block C.** A ratchet fitted to corpus-v1 lands at 10.4 % (M3 ≥ 2 km) and would certify a build that serves 56.4 % on rider demand. The corpus a gate ratchets against must include blocks A and B.
2. **Retire `spike_ge_30m` as a gate input and keep it as a regression canary.** It reads 0.0 % on 6 622 loops; it can only detect a regression, never a defect.
3. **Gate the two free meters first.** `shadow_frac_loop ≥ 0.25` (AUC 0.909) and `compactness < 0.05` (AUC 0.861 inverted) need no new detector, and both would have caught the §5.4 worst-case rows that every current gate passes.
4. **Do not gate D3 or D3b as specified.** They fire on 64–67 % of all loops, and D3's ring count is inflated by the nested-ring bug (§7.7). Fix the overlap guard and re-derive M6 from the per-leg maximum before proposing a threshold.
5. **Re-anchor the vocabulary.** The product's *Spur* and *Backtracking* both say "or nearly the same road"; `Spike` is defined as an exact mirror. The census's dominant mechanism is exactly the gap between those two definitions — 99 % of it is a different OSM way, and 100 % of it is invisible to every meter whose definition contains the word "same".
6. **The labeling task decides two thresholds this census cannot.** `labeling-sample.jsonl` (180 loops, class-blind) carries the strata that matter: is a 12–15 m parallel-carriageway retrace a defect to a rider (sets M3's threshold), and does a self-crossing bother anyone (decides whether D3b ever becomes blocking).

---

## 9. Inputs for the direction grilling

Mechanisms ranked by the share of every kilometre ridden that they account for, on rider-shaped demand (blocks A + B), with the fix shape from the audit:

| rank | mechanism | loops | ride-fraction (A / B) | fix shape | cost of the fix |
|---|---|---|---|---|---|
| **1** | **F02 parallel-carriageway retrace** | 52.8 % / 53.4 % | **13.35 % / 6.88 %** | exclusion by *physical road* (shape buffer, or way-id + bearing) instead of edge id — `route_action.cc:1625-1635`, `:1641`, `:2109-2114` | touches every exclusion site; needs a way→edge index at request time |
| 2 | fallback-like heavy reuse | 8.2 % / 4.5 % | 0.77 % / 0.25 % | narrow the Fallback's exclusion drop (`:1702-1715`) instead of dropping *all* hard exclusions; 25.6 % of served candidates are Fallbacks today | contained, but changes serve rates |
| 3 | F01 ring-reversal harvest chains | 3.1 % / 3.0 % | 0.43 % / 0.28 % | reject chains revisiting an undirected edge key at harvest (`roundtrip_expansion.cc:119-133`) | O(chain), cheap; **5.4× concentrated in the forward leg**, and 7.6 % of slot 0–2 loops |
| 4 | F22 intra-leg ring detour | 3.5 % / 8.1 % | 0.31 % / 0.19 % | by design (the objective pays for rings up to 2.3× the straight edge at c0.8) — a product decision, not a bug | objective change |
| 5 | exempt-zone stem | 0.0 % / 9.8 % | 0.00 % / 0.13 % | re-spec the exemption as `min(1500, 0.05 × target)` | one line; buys ~nothing on demand |
| 6 | seam residue (near-mirror U-turn) | 0.9 % / 0.4 % | 0.05 % / 0.01 % | the seam-window verdict is exact-mirror only; widen it to the near-mirror test | one detector swap |
| 7 | F06 passes-home figure-8 | 0.0 % / 0.0 % | 0.00 % | — | **do not spend v4 budget here** |

The one-line version for the grilling: **v4 has to decide what a road is.** Every other mechanism in this table is a rounding error next to the one where the engine believes the opposite carriageway is a different road, and 93 % of demand rides in the city where that belief is wrong.

Second-order, for the same conversation: **the ranking is single-axis** (F20). Whatever v4 does about F02, a bank sorted by harvest-chain curviness will keep promoting the loops that score well on the one defect axis the score can see. The census measures the price: slots 0–2 are 2.1× worse than slots 9–11 on same-pavement retrace and 1.4× *better* on near-mirror.

---

## 10. F03 — return-leg optimality at corpus scale

The return leg is a bidirectional A\* whose heuristic is `AStarCostFactor() = kSpeedFactor[top_speed] × min_linear_cost_factor_`, which is not a lower bound under the fork's curvature discount. The public lever is a `linear_cost_factors` decoy that drives `min_linear_cost_factor_` to ~0, making the search Dijkstra-exact — inert unless `service_limits.min_linear_cost_factor` (prod ships **1**) is lowered.

**Setup.** A second container `rt-census-f03` on :8004, same image, same tiles, with `min_linear_cost_factor` = `0.00001`. Decoy: the 97-point shape of a 2.0 km road at the corpus-v1 `nis` origin, **199.8 km from the field cell** (a 25–60 km loop harvests at most 1.2 × target/2 = 36 km, so the decoy is unreachable), factor `0.0001`.

**Positive control (the lever must be proven to land).** The cost factor multiplies *cost*, not seconds, so a decoy on the only available path changes nothing observable — the first attempt read identical times on both engines and looked like a dead lever. Laying the decoy on an **alternative** instead is decisive:

| | route served |
|---|---|
| primary route, no decoy | 4.747 km |
| the alternative used as the decoy road | 7.733 km |
| **clamp = 1 (:8003, prod config) with the decoy** | **4.747 km** — factor clamped away, route unchanged |
| **clamp = 1e-5 (:8004) with the decoy** | **7.733 km** — the decoy road is nearly free and wins |

**Equivalence.** :8004 *without* a decoy reproduces :8003 exactly on 10/10 spot-checked requests (all 12 candidates, both legs, shape + length + time identical), so the config change alone is inert.

**Result** — 75 Vračar requests × 12 = **900 loops**:

| | |
|---|---|
| return legs byte-identical between stock and Dijkstra-exact | **98.33 %** (885/900) |
| forward legs identical | 99.11 % |
| return legs that changed | **15** |
| length delta on the changed ones (mean / median) | +1.14 km / −0.01 km |
| elapsed-seconds delta on the changed ones | 9 of 15 shorter, 6 longer |

**Read.** F03 is real — where the two disagree, the stock return is provably sub-optimal under the fork's own cost, because the exact search is Dijkstra. But it touches **1.7 % of return legs** on rider-shaped requests. The *magnitude* of the cost gap is still not measurable through the public API: `/route` exposes elapsed seconds and length, not the curvature-discounted cost the search minimises, which is why the seconds delta splits both ways. Eight forward legs also changed, i.e. a changed return propagates into distance correction and the built-loop set.

**Recommendation:** F03 stays a *major* finding for correctness bookkeeping and a **low** priority for v4. Folding the minimum achievable factor into `AStarCostFactor()` costs latency on 100 % of requests to fix 1.7 % of return legs.

---

## 11. Caveats

1. **The 6 622 loops are 552 requests.** Candidates within a request share the outbound corridor; the effective independent sample for the F02 class is closer to 552 than to 6 622 (mean 8.18 distinct near-mirror readings per 12 candidates; 2 requests return one value for all twelve). Request-level prevalence is quoted where it matters (§7.9).
2. **The mechanism classifier is a decision rule, not a ground truth.** Its magnitudes are disjoint and its thresholds (500 m, 200 m home radius, 250 m seam stub) are hand-set. `labeling-sample.jsonl` exists precisely to test it against a human.
3. **The AUC column in §8.1 is partly circular** for the three meters that define the defective class (marked †). It ranks the *other* meters against a defect definition, not against a rider.
4. **Planimetric geometry cannot separate a mountain switchback pair from a dual carriageway** — except that here it did, by way id (§2.4), on the blocks where it matters. The block C mountain origins' near-mirror share remains an upper bound.
5. **`near_rejoin_rings` over-counts** (§7.7). Every D3 row in this document is the pinned prototype's output, unchanged, so it stays comparable to the prod reading — but its counts and totals are inflated and only the maxima were used for classification.
6. **The engine ledger covers blocks A and B only** (320 of 552 requests) and is per *request*, not per candidate: "25.6 % of candidates fell back" is exact, but which slot fell back is not recoverable from the log. It was captured by re-firing each request **serially** against the same engine and slicing the container's log stream between requests; the 3-worker main run's lines interleave and cannot be attributed. Determinism makes the re-fire valid (40/40 spot-checks identical).
7. **Block C is not b1-x02.** Block C is corpus-v1 verbatim on the *prod-equivalent* engine and 2026-08-05 tiles; `b1-x02` was an experimental `xcand 0.2` build on 2026-07-14 tiles. Differences between the two columns in §6 mix engine, tiles and nothing else — they are not a controlled comparison.
8. **Two requests returned 11 candidates** (`vlasina_d50_c0.5_s11`, `…_s101`); no request failed and no response was retried.
9. **396 of 2 688 way-pass traces failed** — the 300 km loops over `/trace_attributes`' 16 000-point limit (error 153). `edge_reuse_way` is `null` there, as in every previous run.
10. **`curviness_retention` is untestable in engine mode** and reads `null` on every loop.
11. **Open hygiene item, not actioned here.** The prod-cache task scheduled the read-only production cache copy (`~/.curvagen-scratch/prod-cache/cache.db*`, mode 0700) for deletion **at census close**, with the date recorded on curvagen-valhalla#9. This census is that close. The copy was read but not modified by this task and has **not** been deleted — deleting it and recording the date on #9 is the human's call, and the ROPA draft at `~/.curvagen-scratch/prod-cache/ropa.diff` is still unapplied.

---

## Appendix A — exact commands

Engine (prod-equivalent, arm64, `b4f514d7f`; `valhalla-local` on :8002 never addressed):

```bash
docker run -d --name rt-census-engine -p 8003:8003 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  -e use_tiles_ignore_pbf=True -e serve_tiles=True -e server_threads=2 \
  valhalla-curvature:prod-equivalent-b4f514d7f \
  bash -lc "sed 's|tcp://\*:8002|tcp://*:8003|' /custom_files/valhalla.json > /tmp/valhalla-8003.json && exec valhalla_service /tmp/valhalla-8003.json 2"
curl -s localhost:8003/status
# {"version":"3.8.2","tileset_last_modified":1785932567,...}

# the F03 engine: same image, same tiles, service_limits clamp lowered
docker run -d --name rt-census-f03 -p 8004:8004 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  -e use_tiles_ignore_pbf=True -e serve_tiles=True -e server_threads=2 \
  valhalla-curvature:prod-equivalent-b4f514d7f \
  bash -lc "sed -e 's|tcp://\*:8002|tcp://*:8004|g' \
     -e 's|\"min_linear_cost_factor\": 1,|\"min_linear_cost_factor\": 0.00001,|' \
     /custom_files/valhalla.json > /tmp/valhalla-8004.json && exec valhalla_service /tmp/valhalla-8004.json 2"
```

The census run (nothing under `tools/loopqual/` was written except its own output dir):

```bash
cd /Users/xenix/Projects/curvagen-valhalla/tools/loopqual
./loopqual run --engine http://localhost:8003 --corpus corpus-v2.yaml \
  --out results/census-v2-b4f514d7f/ \
  --engine-note "valhalla-curvature:prod-equivalent-b4f514d7f (b4f514d7f)" \
  --trace-engine http://localhost:8003
# 552 requests -> 6622 loops; fired 280s, analysed 243s, way pass 90s
```

Detectors — the pinned blind-spot prototypes, unchanged, exactly as the blind-spots doc Appendix A and the served-loops doc §8 ran them:

```bash
cd ~/.curvagen-scratch
R=/Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/census-v2-b4f514d7f
python3 lqbs_measure.py  $R censusv2.jsonl        # D1 (r=10/25/40) + D2 + D3 + D5   [121 s]
python3 lqbs_rings.py    $R censusv2_rings.jsonl  # D3 rings, iq>=0.15               [ 26 s]
python3 lqbs_xing.py     $R censusv2_xing.jsonl   # D3b crossings                    [ 17 s]
python3 lqbs_stemgap.py  $R censusv2_stem.jsonl   # D6 stem-gap sweep + shadow_loop  [ 18 s]
python3 lqbs_lobe.py     $R censusv2_lobe.jsonl   # D5 / D6c seam-is-apex            [ 10 s]
```

Census-only scripts (new, in the scratch dir):

```bash
python3 census_mech.py $R censusv2_mech.jsonl      # intra/cross-leg reuse split, passes-home,
                                                   # offset x zone spectrum, ring-by-leg   [64 s]
python3 census_agg.py [v13|det|mech|slot|curv|vracar|cell|dist|ledger]   # every table above
python3 census_extra.py                            # offset spectrum, F01 leg attribution,
                                                   # threshold curves, separation/AUC
python3 census_ways.py censusv2_ways.jsonl 40 15   # way-id proof of the F02 class

# the pinned Gate v1.3 instrument, unmodified (absolute thresholds only are meaningful:
# the baseline is a different corpus on a different engine and tileset)
cd /Users/xenix/Projects/curvagen-valhalla/tools/loopqual
python3 gate_v1_proto.py results/b1-x02 results/census-v2-b4f514d7f

# the engine's own ledger: one request at a time, slicing the container log between requests
nohup docker logs -f --timestamps rt-census-engine > census-engine.log 2>&1 &
tools/loopqual/venv/bin/python census_ledger.py \
  tools/loopqual/corpus-v2.yaml $R ~/.curvagen-scratch/census-engine.log \
  censusv2_ledger.jsonl --blocks A,B --verify 40     # 320 requests, 458 s, 40/40 deterministic

# F03
tools/loopqual/venv/bin/python census_f03.py censusv2_f03.json 100

# gallery + labeling sample (writes only into the run's own output dir)
python3 census_gallery.py
```

---

## Appendix B — artifacts

| Path | What |
|---|---|
| `tools/loopqual/results/census-v2-b4f514d7f/responses/` | 552 raw responses + request + meta (the resume cache) |
| `tools/loopqual/results/census-v2-b4f514d7f/loops.jsonl` | 6 622 per-loop v1.3 metric records |
| `tools/loopqual/results/census-v2-b4f514d7f/report.{json,md}` | harness aggregates + provenance (`compare` input) |
| **`tools/loopqual/results/census-v2-b4f514d7f/gallery-census-v2.html`** | **40-case Leaflet gallery**, filterable by mechanism; ride + near-mirror runs + rings + start/seam/spike/home markers; uses `../leaflet.{js,css}` |
| **`tools/loopqual/results/census-v2-b4f514d7f/labeling-sample.jsonl`** | **180 loops, class-blind**, shuffled (seed 20260906); block, cell, distance, curviness, seed, slot, ride km, response file, empty `label`/`notes` |
| `tools/loopqual/results/census-v2-b4f514d7f/labeling-sample-key.jsonl` | the same 180 with `stratum` + `mechanism` + `mech_m` — **keep separate from the labelling** |
| `~/.curvagen-scratch/censusv2.jsonl` | D1/D2/D3/D5 per loop (`lqbs_measure.py`) |
| `~/.curvagen-scratch/censusv2_{rings,xing,stem,lobe}.jsonl` | D3 / D3b / D6 / D6c per loop |
| `~/.curvagen-scratch/censusv2_mech.jsonl` | census-only mechanism signals per loop |
| `~/.curvagen-scratch/censusv2_ledger.jsonl` | 320 requests × the engine's Fallback / Second-Via / Defect-Gate / correction counts |
| `~/.curvagen-scratch/censusv2_ways.jsonl` | way-id evidence, 54 loops |
| `~/.curvagen-scratch/censusv2_f03.json` | F03 summary + 900 per-loop return-leg diffs |
| `~/.curvagen-scratch/census_{mech,agg,extra,ways,ledger,gallery,f03}.py` | the census scripts |
| `~/.curvagen-scratch/census-{run,engine}.log`, `ledger.log`, `ways.log`, `f03.log` | run logs |

Labeling-sample strata (pool sizes): F02 near-mirror 2 436 · other 1 506 · slots 0–2 1 656 · slots 3–5 1 656 · fallback-like 782 · exempt-zone stem 827 · F22 ring 369 · F01 348 · clean-by-all-meters 318 · seam residue 32 · passes-home 4. Sixteen loops per stratum plus a class-blind top-up to 180.

**Primary sources:** `tools/loopqual/{metrics.py,runner.py,corpus-v2.yaml,README.md}`, `src/thor/route_action.cc`, `src/thor/roundtrip_expansion.cc`, `src/sif/{dynamiccost.cc,motorcyclecost.cc}`, `src/worker.cc`, `src/loki/route_action.cc`, and the four inputs this census inherits — `2026-09-05-prod-served-loops-reading.md` §5/§8, `2026-09-05-loopqual-blind-spots.md` §8/§9/Appendix A, `2026-09-05-audit-rig-confirmation.md` §3 F03/§6, `2026-09-05-v3-correctness-optimality-audit.md` F01/F02/F03/F04/F06/F08/F20/F22.

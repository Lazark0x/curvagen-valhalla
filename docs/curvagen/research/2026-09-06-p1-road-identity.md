# P1 — road identity (twin/parallel sidecar), harvest hygiene, clean-first ranking

- **Date:** 2026-09-06 (curvagen-valhalla [#10](https://github.com/Lazark0x/curvagen-valhalla/issues/10), P1 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); blocks [#11](https://github.com/Lazark0x/curvagen-valhalla/issues/11))
- **Scope:** a **throwaway prototype** on branch `proto/v4-p1`, cut from `curvature-costing` @ `c23ff378f` (serving code = `b4f514d7f`), per the `proto/v3-shortlist` precedent. Six mechanisms: a twin/parallel road-identity sidecar built at engine start; tiered identity applied to hard exclusion / leash / rejoin / xcand; F01 non-simple harvest chains rejected; F04 rejoin map built over the physical junction; Second Via deleted; hard-exclude successes ranked before Fallback Loops. Nothing pushed; no production traffic; `valhalla-local` (:8002) never addressed.
- **Method:** build in the warm `valhalla-fork-test:audit` container (arm64, `Release`), serve on :8003 against the same read-only Serbia tiles as the census (`curvagen-orchestrator/data`, `tileset_last_modified 1785932567`), and fire `corpus-v2` (552 requests, K=12) with the census's own run line — engine mode, 3 workers, way pass on. **Three full corpus runs**: `p1-road-identity` (twins + parallels), `p1-twins-only` (the cheaper variant), and `census-v2-control` — a same-session re-run of the pinned baseline engine, because the ratchet's latency bar is inside the harness's own noise floor. The control **reproduced the census exactly** (6 622 loops, every v1.3 meter delta 0.000), which makes it a valid before column and re-confirms F18 determinism. Nothing under `tools/loopqual/` was modified except the runs' own output dirs; every new script lives in `~/.curvagen-scratch/p1/`.

## TL;DR verdict

**The mechanism works, and it is the largest single move any round-trip change has made.** F02 — the census's whole rider-visible residual, "the return rides the other carriageway" — goes from **37.0 % of loops and 3.81 % of every ride kilometre to 3.2 % / 0.05 %**. On the two rider-shaped blocks the headline detector falls by an order of magnitude:

| | census (control) | P1 | change |
|---|---|---|---|
| **D1b ≥ 500 m, Vračar** | 61.1 % | **6.4 %** | **−89.5 %** |
| **D1b ≥ 500 m, demand** | 64.1 % | **9.2 %** | **−85.6 %** |
| D1b unseen metres/loop, Vračar | 5 755 m | **171 m** | −97.0 % |
| D1b unseen metres/loop, demand | 6 629 m | **92 m** | −98.6 % |
| **D1 same-pavement run ≥ 500 m, Vračar** | 33.6 % | **14.6 %** | **−56.5 %** |
| **D1 same-pavement run ≥ 500 m, demand** | 24.6 % | **11.3 %** | **−54.1 %** |
| F01 intra-leg self-overlap, mean | 955 m | **39 m** | −95.9 % |
| loops classified `clean` | 4.8 % | **8.5 %** | +77 % |

The provisional gate's defect bars are cleared with room to spare. **Three of its other bars are not**, all marginally and all in the same direction — the exclusion is *wider*, so the network offers fewer distinct ways home:

1. **Bank distinctness (Gate v1.3 ratchet 9) fails at every curviness** — `bank_overlap_mean` 1.06–1.10× base, `near_dup > 0.6` +9 to +11 pp. This is the honest cost of narrowing the option space and it is P1's clearest open question.
2. **Latency p50 1.114×** the same-session control (1.362 s → 1.517 s), against a 1.10× bar with a ±12 % noise floor. The engine's own stage ledger attributes +85 ms/request: +48 ms harvest (the F01 forest pass), +45 ms A\* (wider exclusion), −11 ms recovered from deleting Second Via.
3. **Fills**: 545 of 552 requests still fill K=12; 7 return 11 (the control returns 11 on 2). Failures **0**.

Two smaller regressions, both confined to block C (mountains, motorways avoided, not rider-shaped demand): `is_lollipop` at c0.8 1.30 % → 3.91 %, and curviness retention at c0.8 **0.946×** — just under the 0.95 bar. c0.8 exists only in block C.

**The cheaper variant does not help.** `p1-twins-only` (parallel tier off) costs *identical* latency (p50 1.519 s) and *identical* distinctness, and leaves **more** defect behind (D1b ≥ 500 m: ALL 12.2 % vs 9.4 %, demand 11.9 % vs 9.2 %). The parallel tier is free; keep it on.

**The ranking change is doing real work.** Every one of the twenty worst remaining loops sits in slot 10 or 11 — the deep bank, which K=3 direct serve never reads. In the direct-serve slots 0–2, D1b ≥ 500 m falls **41.2 % → 8.8 %** and mean unseen metres **3 575 → 119**.

---

## 1. The sidecar — design, cost, counts

`thor::RoadTwinIndex` (`valhalla/thor/road_twin_index.h`, `src/thor/road_twin_index.cc`, 506 lines) is computed once per process in the thor worker's constructor (`src/thor/worker.cc:88-93`) and logged at INFO. Later workers hit a cached instance keyed by tileset location + radii — keyed rather than a plain singleton because gurka builds many tilesets in one process.

**Pass 1** walks `GraphReader::GetTileSet()` and samples every *canonical* directed edge — the one with `DirectedEdge::forward() == true`, i.e. the one carrying the stored `EdgeInfo` shape, which every undirected segment has exactly one of. Shortcuts, transit lines and non-auto edges are skipped. Shapes are sampled at 25 m (endpoints always in, capped at 12) into `int32` micro-degrees, and each segment is inserted into a 120 m spatial bin over its sample cells.

**Pass 2** gathers each segment's 1-ring bin neighbourhood, filters on chord collinearity (±30°), and applies two geometric tests to each candidate pair:

- **COVER** — the share of one segment's samples within radius `r` of the other's polyline (≥ 0.70);
- **SPAN** — the along-neighbour extent those samples project onto, as a share of the shorter segment (≥ 0.60).

`r = thor.roundtrip_twin_radius_m` (30) qualifies a **twin**; otherwise `thor.roundtrip_parallel_radius_m` (80) qualifies a **parallel**. Results are emitted as sorted CSR maps (keys, offsets, values) keyed by canonical id; the samples and bin grid are freed.

**The span test is the whole detector.** Cover alone called **422 677 pairs over 380 709 segments (39.6 % of the tileset)** twins — because two *consecutive* short edges, or two edges crossing at a junction, are collinear and have every sample within 30 m of the shared node. Their samples all project onto the same point of the neighbour, so their span is ~0. With the span test the counts become plausible:

| | Serbia tileset (211 tiles) |
|---|---|
| segments sampled (canonical, drivable, non-shortcut) | **960 188** |
| shape samples | 3 992 472 |
| pair tests | 5 499 037 |
| **twins** (r ≤ 30 m) | **81 288 pairs / 115 001 keyed segments (12.0 %)** |
| **parallels** (30–80 m) | **181 288 pairs / 215 223 keyed segments (22.4 %)** |
| **build time** (3 cold starts) | **4 227 / 4 419 / 4 554 ms** |
| retained memory | **7 MiB** (twins + parallels) · **2 MiB** (twins only) |
| peak transient memory | ~106 MiB (samples + bin grid, freed) |

**Startup, not lazy.** 4.5 s of one-time cost against a 250 MB tileset is cheap enough that the per-request path never pays; the lazy-per-tile variant the ticket allows was not needed and is not implemented.

**Orientation note.** The ticket phrases the twin test as "bearing difference ~180° ± tolerance". Physically that is right — the return rides the twin anti-parallel to the forward leg — but OSM's storage direction for a carriageway is arbitrary, so at index time the same physical pair presents as 0° or 180° depending on which way the way was drawn. The index therefore qualifies on **collinearity** and stores *segment* identity; the call sites exclude **both directions** of a twin, exactly as `route_leg` already does for a corridor edge and its opposing edge. That is what makes the anti-parallel ride impossible.

**Known false-positive class, measured but not separated.** Planimetric geometry cannot tell a mountain switchback pair from a dual carriageway (defect atlas v2 §11.4). Two hairpin arms running 25 m apart over most of their length *are* flagged as twins, so the return is barred from the other arm. That is the honest source of block C's two regressions (§7).

---

## 2. Mechanisms changed

| # | mechanism | where | what changed |
|---|---|---|---|
| 1 | sidecar build | `src/thor/worker.cc:88-93`; `src/thor/road_twin_index.cc:88-377` | built at engine start, logged at INFO; five config knobs (§9) |
| 2 | **tiered identity** | `src/thor/route_action.cc:1498-1568` | twins get the corridor's own treatment — leashed everywhere, hard-excluded beyond the Start Exemption; parallels join the soft leash |
| 3 | parallel rejoin tier | `route_action.cc:1605-1607` | a parallel carries the same progress grade as the corridor stretch it shadows |
| 4 | **F04** rejoin map | `route_action.cc:1583-1604` | built with `for_each_junction_edge` — the physical junction, all hierarchy levels (was: the corridor node's own level only) |
| 5 | xcand keys on twins | `route_action.cc:1932-1955` | a later candidate riding a committed loop's opposite carriageway pays the same surcharge |
| 6 | **F01** harvest hygiene | `src/thor/roundtrip_expansion.cc:85-89, 170-174, 190-286`; `valhalla/thor/roundtrip_expansion.h:52-63` | `ScanBand` rejects chains that revisit an undirected segment or its twin |
| 7 | **Second Via deleted** | `route_action.cc:1021-1028` (the gravestone) | mechanism, six constants, `stem_fraction()`, the ledger line, G4 and two gurka tests |
| 8 | **ranking** | `route_action.cc:1993-2003` | hard-exclude successes before Fallback Loops, then curviness, stable |
| 9 | ledger | `route_action.cc:1971-1977, 2007-2015` | `roundtrip identity:` and `roundtrip ranking:` at INFO |

Unchanged, deliberately: the seam Defect Gate, the fallback decision, the Start Exemption radius, `kFullExploreRadiusM`, the distance correction, the attempt budget, `ignore_hierarchy_limits_` (F05's accident, still pinned by G7).

### F01 — one forest pass, not one walk per candidate

The audit's fix shape ("reject chains that revisit an undirected edge key at harvest") is O(chain) *per candidate*, and the first implementation of exactly that cost **+1 100 ms per 50 km Belgrade request** (harvest 62 → 646 ms, scan 23 → 1 120 ms measured on the rig): the flex band alone puts tens of thousands of labels in range, and every one of them walks a ~250-edge chain.

Because `predecessor(k) < k` always — a label is created after its predecessor settles — the settled labels form a forest, so **one DFS carrying the canonical ids of the current root-to-node path in a small hash map** answers "does this edge already appear above me" in O(1) per label, and the flag inherits down the chain. That is `ComputeChainSimplicity` (`roundtrip_expansion.cc:204-286`): O(labels), run once per expansion, cached across the primary and the widened `ScanBand`. Cost measured at corpus scale: **+48 ms per request** in `harvest` (89 → 137 ms mean over 552 requests). Rejects: **41 116 chains per request** on average, 22.7 M over the corpus.

---

## 3. Gurka

`gurka_roundtrip_audit` **12 of 13 pass** (was 5 of 11 + 2 new):

| test | before | after |
|---|---|---|
| **G1** `RingReversal` (F01) | FAIL — `legs[0] = AB BC CD DE EF FD CD`, `CD` ×2 | **PASS** — `legs[0] = AB BC CD FD`, edge-simple |
| **G1b** `RingReversalArm` (F01+F20) | FAIL — reversal chain 0.48 beat the clean arm 0.42 | **PASS** — the clean arm `BH` wins the slot |
| **G2** `DualCarriageway` (F02) | FAIL — `legs[1] = BC CD DA`, return rode the twin, 0 m closest | **PASS** — `legs[1] = BE EF FA`, the fresh road |
| **G3** `RejoinMixedClass` (F04) | FAIL — graded return byte-identical to the ungraded control | **PASS** — the graded return takes the wide detour |
| G4 `SecondViaFigure8` (F06) | FAIL | **deleted with the mechanism** |
| G5 control / G5 `LongFirstEdge` (F07) | PASS / PASS (did not reproduce) | PASS / PASS |
| G6 `RestrictedTurnBounce` (F08) | PASS (not reproduced as posed) | PASS |
| **G6b** `…NoWayRound` (F08) | FAIL — 999.99 m mid-return mirror served | **FAIL (unchanged)** — F08 is out of P1's scope |
| G7 `ShortcutPin` (F05) | PASS | **PASS** — 0 of 28 served edges are shortcuts |
| F03 `ReturnHeuristic` | inconclusive at gurka scale | unchanged |
| **P1a** `ParallelTier` (new) | — | **PASS** — control (tier off) takes the 50 m parallel `QP`; tier on takes the fresh `EF` |
| **P1b** `TwinOnlyWayHome` (new) | — | **PASS** — twin excluded, cell still serves via the Fallback, no 442 |
| **P1c** `CleanFirstRanking` (new) | — | **PASS** — slot 0 = the hard-exclude success (`QA`), slot 1 = the Fallback (`AB` ×2) |

Two G-test assertions were repaired rather than satisfied, both structurally degenerate as written; both are recorded in the file:

- **G1 (b)** asserted the D-E-F ring appears *inside `legs[0]`* — which was only true because the harvest chain reversed on it. It now asserts the ring appears in the served **ride** (it does, once, across the seam).
- **G2 (b)** measured the closest return-to-forward vertex outside the Start Exemption disc, which can only ever read 0 m: the turnaround node belongs to both legs (the rig-confirmation report recorded exactly this, "0 m — the shared turnaround node"). A 100 m seam disc now comes out of the measurement too.

`gurka_motorcycle_roundtrip` **22/22** after deleting the two Second Via tests. `gurka_roundtrip_distinctness` **3/3**.

---

## 4. corpus-v2, before → after

Baseline column = `census-v2-control`, the same-session re-run of `b4f514d7f`; it reproduces the pinned census `census-v2-b4f514d7f` to the metre. Detector columns are read by **byte-identical code** on both runs, including the **fixed** D3 ring counter (§5).

### 4.1 v1.3 meters and v2 magnitudes

| meter | ALL before | ALL after | block C before → after | block A Vračar | block B demand |
|---|---|---|---|---|---|
| n loops | 6 622 | 6 617 | 2 782 → 2 779 | 1 200 → 1 200 | 2 640 → 2 638 |
| `spike_ge_30m` | 0.0 % | **0.0 %** | 0.0 → 0.0 | 0.0 → 0.0 | 0.0 → 0.0 |
| `max_stub_km` max | 0.000 | 0.131 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → **0.131** |
| `edge_reuse_geom` mean | 0.0342 | **0.0216** | 0.0587 → 0.0401 | 0.0273 → **0.0124** | 0.0114 → **0.0063** |
| `edge_reuse_geom > 0.30` | 2.4 % | **1.7 %** | 5.6 → 4.0 | 0.1 → **0.0** | 0.1 → **0.0** |
| `edge_reuse_way` mean | 0.0546 | **0.0436** | 0.1007 → 0.0830 | 0.0434 → 0.0295 | 0.0232 → 0.0185 |
| `is_lollipop` | 0.7 % | 1.1 % | 1.8 → **2.6** | 0.0 → 0.0 | 0.0 → 0.0 |
| `bulb_count` mean | 2.136 | **1.644** | 1.717 → 1.483 | 2.595 → **1.856** | 2.369 → **1.718** |
| `shadow_frac` mean | 0.1478 | **0.0414** | 0.0927 → 0.0627 | 0.2571 → **0.0410** | 0.1560 → **0.0190** |
| `shadow_frac_loop` mean | 0.1405 | **0.0379** | 0.0831 → 0.0573 | 0.2518 → **0.0371** | 0.1504 → **0.0177** |
| `compactness` mean | 0.2349 | **0.2919** | 0.2441 → 0.2609 | 0.1925 → **0.3154** | 0.2444 → **0.3139** |
| `compactness < 0.10` | 21.8 % | **10.5 %** | 14.8 → 12.1 | 41.7 → **14.7** | 20.1 → **7.0** |
| `distance_error` mean | 0.1628 | 0.1515 | 0.2256 → 0.1768 | 0.1148 → 0.1539 | 0.1184 → 0.1239 |
| `distance_error` max | 8.353 | **2.317** | 8.353 → 2.317 | 0.555 → 0.822 | 0.598 → 0.685 |
| `curviness_geom_clean` mean | 270.7 | 271.2 | 347.0 → 338.4 | 258.7 → **261.3** | 195.7 → **205.0** |
| **unseen near-mirror m mean** | 4 230 | **152** | 1 295 → 200 | 5 755 → **171** | 6 629 → **92** |
| — p50 / p90 | 490 / 16 168 | **0 / 478** | 174/2 380 → 15/516 | 3 059/16 168 → **0/185** | 3 051/19 653 → **0/376** |
| — as a share of the ride, mean | 0.068 | **0.002** | 0.017 → 0.003 | 0.142 → **0.005** | 0.089 → **0.001** |
| same-pavement both-ways m mean | 3 030 | **1 560** | 5 367 → 3 077 | 1 536 → **434** | 1 248 → **474** |
| **intra-leg self-overlap m mean (F01)** | 955 | **39** | 1 809 → 88 | 305 → **0** | 351 → **5** |
| — ≥ 500 m | 10.3 % | **0.6 %** | 14.1 → 1.4 | 9.8 → **0.0** | 6.6 → **0.2** |
| passes home mid-ride (≤ 200 m) | 0.1 % | **0.0 %** | 0.1 → 0.0 | 0.0 → 0.0 | 0.0 → 0.0 |

### 4.2 D1–D6 detector prevalence

| detector | ALL before → after | block C | block A Vračar | block B demand |
|---|---|---|---|---|
| D1 ≥ 1 km ridden both ways | 28.3 → **12.7 %** | 31.6 → 19.1 | 32.2 → **10.5** | 23.0 → **7.0** |
| D1 longest same-pavement run ≥ 500 m | 29.9 → **17.4 %** | 33.4 → 24.5 | 33.6 → **14.6** | 24.6 → **11.3** |
| D1 longest same-pavement run ≥ 2 km | 20.1 → **8.1 %** | 26.9 → 16.5 | 14.3 → **1.0** | 15.4 → **2.5** |
| **D1b unseen near-mirror ≥ 500 m** | 49.6 → **9.4 %** | 30.9 → 10.9 | **61.1 → 6.4** | **64.1 → 9.2** |
| **D1b unseen near-mirror ≥ 2 km** | 37.6 → **0.8 %** | 10.4 → 0.6 | **59.3 → 2.3** | **56.4 → 0.3** |
| D1c near-mirror U-turn at the seam | 2.2 → **0.8 %** | 2.9 → 1.0 | 3.2 → 1.2 | 1.1 → 0.2 |
| D2 exempt-zone corridor ≥ 5 % | 2.5 → 2.3 % | 5.5 → 5.4 | 0.0 → 0.0 | 0.5 → **0.0** |
| D2 reuse hidden by the exemption ≥ 1 km | 6.9 → 7.4 % | 15.9 → 16.5 | 0.0 → 0.0 | 0.5 → 1.1 |
| D3 ≥ 1 near-rejoin ring (fixed counter) | 64.1 → **49.9 %** | 46.6 → 39.5 | 80.4 → **55.2** | 75.2 → **58.3** |
| D3 ring inside a single leg (fixed counter) | 20.8 → **13.0 %** | 15.7 → 7.0 | 14.2 → 8.7 | 29.2 → 21.3 |
| D3b ≥ 1 transversal self-crossing | 66.7 → **52.9 %** | 56.5 → 46.0 | 66.0 → 49.9 | 77.8 → 61.5 |
| D3b figure-8 (lobe ≥ 25 %) | 22.2 → **14.4 %** | 16.6 → 14.4 | 32.7 → **15.2** | 23.2 → 14.0 |
| D4 Fallback proxy | 28.5 → **22.0 %** | 35.6 → 25.9 | 30.5 → 22.2 | 20.0 → 17.9 |
| D5 two prominent lobes | 2.6 → 2.1 % | 5.1 → 2.1 | 0.9 → 1.8 | 0.8 → 2.3 |
| **D6 cross-leg corridor ≥ 25 %** | 22.1 → **2.7 %** | 9.6 → 5.6 | **49.2 → 2.0** | **23.0 → 0.0** |
| D6 stem zeroed by the 120 m gap | 20.1 → **2.3 %** | 11.7 → 0.6 | 8.8 → 0.0 | 34.2 → **5.2** |
| D6c seam is not the ride's farthest point | 77.3 → 75.5 % | 83.0 → 81.0 | 66.4 → 63.3 | 76.2 → 75.2 |

### 4.3 Mechanism classification after P1

Share of loops / share of that population's total ride kilometres. Before → after.

| mechanism | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| **near-mirror / parallel carriageway (F02)** | 37.0 / 3.81 % → **3.2 / 0.05 %** | 14.1 / 0.71 → 5.5 / 0.05 | **53.3 / 13.39 → 2.3 / 0.23** | **53.7 / 6.89 → 1.3 / 0.02** |
| **same-pavement retrace mid-route (F01)** | 5.3 / 0.71 % → **0.1 / 0.01 %** | 8.4 / 1.01 → 0.1 / 0.02 | 3.1 / 0.43 → **0.0 / 0.00** | 3.1 / 0.28 → **0.0 / 0.00** |
| fallback-like heavy reuse | 11.9 / 1.26 % → **15.7 / 1.29 %** | 20.3 / 1.94 → 20.6 / 1.85 | 8.4 / 0.78 → 15.8 / 1.02 | 4.5 / 0.25 → 10.4 / 0.45 |
| intra-leg ring / near-rejoin (F22) | 5.2 / 0.19 % → **10.6 / 0.24 %** | 3.8 / 0.20 → 4.6 / 0.11 | 2.8 / 0.23 → 6.4 / 0.51 | 7.8 / 0.16 → **18.9 / 0.39** |
| exempt-zone stem | 12.5 / 0.13 % → 13.6 / 0.12 % | 20.5 / 0.15 → 20.9 / 0.14 | 0.0 → 0.0 | 9.8 / 0.13 → 12.1 / 0.11 |
| seam residue | 0.5 / 0.01 % → **0.1 / 0.00 %** | 0.4 → 0.1 | 0.9 → 0.2 | 0.4 → 0.0 |
| passes-home figure-8 (F06) | 0.1 / 0.01 % → **0.0 / 0.00 %** | 0.1 → 0.0 | 0.0 → 0.0 | 0.0 → 0.0 |
| other (a detector fires, < 500 m of any mechanism) | 22.7 → **48.1 %** | 28.6 → 42.5 | 24.0 → 60.7 | 15.9 → 48.4 |
| **clean** | 4.8 → **8.5 %** | 3.7 → 5.5 | 7.4 → **14.7** | 4.7 → **8.9** |

Two mechanisms *grew*, and both are the substitution the design predicted:

- **fallback-like heavy reuse** 11.9 → 15.7 % of loops but **1.26 → 1.29 %** of ride kilometres. More loops touch the class; the metres do not move. The bank is deeper into the fallback tail because the clean pool is smaller, and clean-first ranking parks those loops in slots 9–11 rather than serving them (§4.4).
- **intra-leg ring (F22)** 5.2 → 10.6 % of loops, 0.19 → 0.24 % of ride. Where the return used to ride the mirror carriageway it now goes round the block. F22 is by design (defect atlas §9 rank 4 — "a product decision, not a bug"), and it costs 0.05 pp of ride against F02's 3.76 pp.

### 4.4 By served slot — what the ranking did

| | slots 0–2 (direct serve) | slots 3–5 | slots 6–8 | slots 9–11 |
|---|---|---|---|---|
| D1b ≥ 500 m, before → after | 41.2 → **8.8 %** | 47.8 → 6.8 % | 52.8 → 6.5 % | 56.7 → 15.6 % |
| D1b unseen m mean, before → after | 3 575 → **119** | 2 759 → 99 | 4 411 → 108 | 6 176 → 282 |
| D1 same-pavement run ≥ 500 m | 40.7 → **9.3 %** | 33.1 → 8.6 % | 26.0 → 10.4 % | 19.8 → **41.5 %** |
| `clean` | 8.5 → **14.2 %** | 3.6 → 9.1 % | 4.9 → 7.9 % | 2.2 → 2.9 % |
| F02 mechanism | 26.2 → **4.3 %** | 31.9 → 3.4 % | 41.8 → 2.7 % | 48.1 → 2.6 % |
| `curviness_geom_clean` mean | 338.0 → 326.8 (0.967×) | 283.9 → 265.9 | 242.9 → 239.9 | 217.8 → 252.1 |

The engine's own ranking ledger, summed over all 552 requests (6 617 served slots, 1 451 Fallback Loops = **21.9 %**):

| slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Fallback share | 5.3 % | 5.3 % | 5.3 % | 5.3 % | 5.3 % | 5.3 % | 5.3 % | 10.1 % | 19.6 % | 39.3 % | 68.1 % | **90.1 %** |

The flat 5.3 % floor in slots 0–7 is 29 requests whose *every* candidate fell back — the clean-first sort cannot help where nothing is clean. Everywhere else the fallbacks are pushed to the back of the bank, which is exactly the intent: a rider reading slots 0–2 sees a hard-exclude success 94.7 % of the time.

---

## 5. The D3 ring counter, fixed

The census flagged `near_rejoin_rings` as not gate-grade (defect atlas §7.7): its overlap guard tested only the two **endpoint** indices of a candidate arc while marking the whole arc used, so one physical near-rejoin came back as a nest of shifted rings — a 52.6 km Belgrade loop returned 19 rings of 25.7–26.2 km, all the same feature.

Fixed in a scratch copy (`~/.curvagen-scratch/p1/lqbs_lib.py`) by sampling twenty points along the candidate arc and rejecting it when more than half are already spoken for — O(1) per candidate, so the pass costs the same:

| | mean rings/loop | max rings on one loop | share with ≥ 1 ring | max ring metres, mean |
|---|---|---|---|---|
| census, pinned (buggy) counter | 13.17 | **739** | 64.1 % | 10 106 |
| census, fixed counter | **1.09** | **6** | 64.1 % | 8 770 |
| P1, fixed counter | **0.71** | 5 | **49.9 %** | 8 137 |

The *prevalence* row is unchanged (the guard only removes duplicates of an already-detected ring), so the census's published D3 prevalence numbers stand; its **counts and totals were inflated ~12×** and only the per-side maximum was ever usable, as §7.7 said. Every D3 number in this document — for both runs — comes from the fixed counter.

---

## 6. Provisional gate

| # | gate | bar | census (control) | P1 | verdict |
|---|---|---|---|---|---|
| 1 | D1b ≥ 500 m, **Vračar** | down ≥ 50 % | 61.1 % | **6.4 %** (−89.5 %) | **PASS** |
| 2 | D1b ≥ 500 m, **demand** | down ≥ 50 % | 64.1 % | **9.2 %** (−85.6 %) | **PASS** |
| 3 | D1 ride-fraction, **Vračar** (run ≥ 500 m) | down ≥ 50 % | 33.6 % | **14.6 %** (−56.5 %) | **PASS** |
| 4 | D1 ride-fraction, **demand** | down ≥ 50 % | 24.6 % | **11.3 %** (−54.1 %) | **PASS** |
| 5 | curviness retention, per level | ≥ 0.95× | c0.5 287.1 · c0.7 206.3 · c0.8 344.2 · c1.0 233.3 | 286.3 (0.997) · 215.1 (1.043) · **325.7 (0.946)** · 239.0 (1.024) | **FAIL at c0.8** (marginal, block C only) |
| 6 | latency p50 | ≤ 1.10× | 1.362 s | **1.517 s (1.114×)** | **FAIL** (marginal; noise floor ±12 %) |
| 6b | latency p95 | — | 3.563 s | 3.713 s (1.042×) | pass |
| 7 | fills K = 12 on every request | 552/552 | 550/552 | **545/552** (7 at 11/12) | **FAIL** (marginal) |
| 8 | failures | 0 | 0 | **0** | **PASS** |
| 9 | fallback share | report | leg counter 0.369 · D4 proxy 28.5 % | leg counter **0.327** · D4 proxy **22.0 %** · served **21.9 %** (5.3 % in slots 0–2) | **improved** |
| 10 | exempt-zone stem | report | 12.5 % of loops / 0.13 % of ride; D2 ≥ 5 % 2.5 % | 13.6 % / **0.12 %**; D2 ≥ 5 % 2.3 % | **flat** |
| 11 | Gate v1.3 absolutes (1,2,3,4,5,7) | pass | pass | **pass** at every level | **PASS** |
| 11b | Gate v1.3 **6a** lollipop ≤ 2 % | pass | c0.8 1.30 % | c0.8 **3.91 %** | **FAIL at c0.8** |
| 11c | Gate v1.3 **6b** worst cell ≤ 55 % | pass | vlasina-50 km c0.5 48.3 % | **62.7 %** | **FAIL** |
| 11d | Gate v1.3 **9** distinctness ratchet | ≤ 1.02× / +2 pp | — | `bank_overlap` **1.06–1.10×**, `near_dup>0.6` **+9…+11 pp**, at every level | **FAIL** |

Latency against the *pinned* census (`census-v2-b4f514d7f`, p50 1.305 s) reads **1.162×**; against the same-session control of the same binary on the same box, **1.114×**. The control is the honest comparison — the box drifted +4.4 % between the census session and this one.

---

## 7. What did not move, and why

1. **`spike_ge_30m` stays at 0.0 %** and `max_stub_km` stays 0.000 on all but one loop. It was already a tautology before P1 (defect atlas §7.1) and remains one. The single new 0.131 km stub is in block B.
2. **The exempt-zone stem did not move** — 12.5 → 13.6 % of loops, 0.13 → 0.12 % of ride kilometres. P1 does not touch the Start Exemption, and the census already showed the class carries almost no ride (§9 rank 5, "buys ~nothing on demand"). Confirmed.
3. **D6c (the seam is not the ride's farthest point) barely moved**: 77.3 → 75.5 %. That is a *shape* property of the harvest band, not of road identity — P2 territory.
4. **F08 (mid-return bounce) is untouched**, as designed: G6b still fails with a 999.99 m mid-return exact mirror that the seam-window verdict cannot see. It needs the Defect Gate to gate on geometry, not on the seam window.
5. **F03 (return-leg A\* admissibility) is untouched.** The census measured 98.3 % of Vračar return legs byte-identical under an exact search; it is real and rare.
6. **Block C got slightly worse in two places**, and both trace to the same false positive: a mountain switchback pair 25 m apart is geometrically a twin. Barring the other arm forces an out-and-back, so `is_lollipop` at c0.8 goes 1.30 → 3.91 % and the vlasina-50 km cell goes 48.3 → 62.7 %; curviness retention at c0.8 lands at 0.946×. c0.8 exists **only** in block C (384 loops, motorways avoided, mountain origins) and 93 % of production demand is `a0` in the city. It is a real cost, it is confined to the terrain P1 was not aimed at, and separating a switchback from a carriageway needs way ids or bearings-along-the-run — a P2 sidecar refinement, not a knob.
7. **Bank distinctness got worse everywhere** (ratchet 9). Excluding twins and leashing parallels removes ways home; with fewer distinct corridors the K candidates converge. `bank_overlap_mean` +6 to +10 %, `near_dup > 0.6` +9 to +11 pp, `common_trunk_frac_75_mean` +57 % at c0.5. The xcand penalty and the item-4 sharing filter both ship **off** in prod; this run had them off too. Turning `thor.roundtrip_xcand_penalty` on is the obvious lever and was not swept here.

---

## 8. Latency

| stage (mean ms/request, 552 requests) | baseline | P1 | Δ |
|---|---|---|---|
| harvest (expansion + primary `ScanBand` + **F01 forest pass**) | 89 | **137** | **+48** |
| widened `ScanBand` | 21 | 19 | −2 |
| rejoin map build (**+ twins, parallels, all levels**) | 3 | 7 | +4 |
| return-leg A\* (**wider exclusion**) | 622 | **667** | **+45** |
| Fallback A\* | 186 | 187 | +1 |
| Second Via | 11 | **0** | **−11** |
| seam / walk-back / TripLegBuilder | 30 | 30 | 0 |
| **engine stage total** | **962** | **1 047** | **+85 (+8.8 %)** |
| **wall p50 / p95** | **1.362 / 3.563 s** | **1.517 / 3.713 s** | **1.114× / 1.042×** |
| corpus firing wall time | 292 s | 313 s | 1.072× |

Where the money went: the F01 forest pass and the wider return search, roughly equally. Deleting Second Via pays for an eighth of it. The twins-only variant is **not** cheaper (p50 1.519 s) — the cost is the twin exclusion and F01, not the parallel tier.

---

## 9. The cheaper variant — twins only, parallels off

`thor.roundtrip_parallel_tier = false` (a config flip, no rebuild): the sidecar builds twins only (2 MiB retained, 4.23 s), and parallels never enter the leash or the rejoin map.

| | census (control) | **P1 full** | P1 twins-only |
|---|---|---|---|
| D1b ≥ 500 m, ALL | 49.6 % | **9.4 %** | 12.2 % |
| D1b ≥ 500 m, Vračar | 61.1 % | **6.4 %** | 6.8 % |
| D1b ≥ 500 m, demand | 64.1 % | **9.2 %** | 11.9 % |
| D1 run ≥ 500 m, Vračar | 33.6 % | **14.6 %** | 16.2 % |
| D1 run ≥ 500 m, demand | 24.6 % | 11.3 % | **9.6 %** |
| D6 cross-leg corridor ≥ 25 %, ALL | 22.1 % | 2.7 % | 2.7 % |
| `is_lollipop` ALL | 0.7 % | 1.1 % | 1.1 % |
| curviness retention c0.8 | 1.000× | 0.946× | 0.948× |
| `bank_overlap_mean` c0.5 | 0.511 | 0.550 (1.075×) | 0.547 (1.070×) |
| latency p50 | 1.362 s | 1.517 s (1.114×) | **1.519 s (1.115×)** |
| Gate v1.3 | — | 12 FAIL | 12 FAIL |

**Verdict: keep the parallel tier.** It costs nothing measurable in latency, distinctness or curviness, and it removes another quarter of the remaining D1b residual on the demand block.

---

## 10. Gallery

`tools/loopqual/results/p1-road-identity/gallery-p1.html` — 40 Leaflet cases, filterable by mechanism: the **20 worst remaining P1 loops** by D1b unseen near-mirror metres, followed by the **census's worst 20 on the same axis**, drawn by the same payload builder.

Every one of the twenty worst remaining loops is in **slot 10 or 11** — the deep bank, which K=3 direct serve never reads. P1's worst reads **11 193 m** of unseen near-mirror on a 39.3 km ride (28.5 %) at Vračar 50 km. The census's worst read **48 596 m on a 99.9 km ride (48.6 %)** at demand cell `(44.800, 20.460)`, **slot 5** — a first-Bank-tap candidate; and the atlas's headline Vračar case (`50 km / c0.5 / s42`, 28 171 m of 52.9 km) sat at **slot 2, a direct-serve candidate**. Population D1b metres: census mean 4 230 / p50 490 / p90 16 168 / **max 48 596**; P1 mean **152** / p50 **0** / p90 **478** / max 11 193.

| rank | cell / ask | slot | ride km | D1b m | share | mechanism |
|---|---|---|---|---|---|---|
| 1–5 | Vračar 50 km c0.5/c0.7 (a0 and a1) | 10–11 | 39.3 | 11 193 | 0.285 | F02 |
| 6 | belacrkva 300 km c0.5 | 11 | 270.2 | 6 342 | 0.023 | F02 |
| 7–8 | demand#1 150 km c0.5 | 11 | 175.1 | 6 290 | 0.036 | F22 / fallback-like |
| 9 | demand#6 100 km c0.7 | 10 | 69.5 | 4 104 | 0.059 | fallback-like |
| 10–12 | Vračar 50–60 km | 10–11 | 37.6–39.4 | 3 742–4 002 | 0.10 | F02 |
| 13–15 | novisad 20 km c0.5 | 11 | 27.1 | 3 685 | 0.136 | seam residue |
| 16–20 | Vračar 40–60 km | 10–11 | 35.8–36.4 | 3 545 | 0.10 | F02 / fallback-like |

---

## 11. Open questions

### For P2

1. **Distinctness is the bill for road identity.** The bank now draws from a smaller pool of distinct corridors. Sweep `thor.roundtrip_xcand_penalty` (ships off) and the item-4 sharing filter against the P1 engine before deciding whether the ratchet needs re-fitting or the mechanism needs a distinctness partner.
2. **Separate the switchback from the carriageway.** The 30 m planimetric test cannot, and block C pays for it. Candidates: require the pair to be different OSM ways (an EdgeInfo way-id read at build time — free); or require the *matched offsets to be roughly constant* along the run (a hairpin's diverge, a carriageway's do not).
3. **Rank the built loop, not the harvest chain.** P1 added one axis (clean-first) and it moved slot 0 hard. F20's full fix — both legs, distance error, self-overlap — is still open, and §4.4 shows slots 9–11 now carry the residue by construction, which is exactly what a bank should do only if the ranking is trusted.
4. **The F01 forest pass costs 48 ms/request.** It is O(labels) and runs once, but it computes a canonical id for every settled label. Restricting it to labels with `path_distance ≤ hi` would cut it; so would caching the canonical id in the label.
5. **F22 doubled in loop share.** Rings are now the substitute for the mirror carriageway. They carry 0.24 % of ride kilometres — but the labelling task should be asked whether a rider reads "round the block" as a defect.
6. **7 requests under-fill.** All at 11/12, five more than the control. Worth confirming they are network-forced (`vlasina`, `d44796_20437`) rather than budget-forced (F09).

### For Gate v2

7. **`shadow_frac_loop` collapsed 0.1405 → 0.0379 and `compactness` rose 0.235 → 0.292** — the two free meters the census recommended gating (AUC 0.909 and 0.861) both moved hard and in the right direction. They would have scored P1 correctly where `edge_reuse_geom` (AUC 0.663) barely moved. Gate v2 should take them.
8. **D1b's threshold now matters.** At 500 m the residual is 9.4 %; at 2 km it is 0.8 %. Whether a 500 m near-mirror is a defect to a rider is exactly what `labeling-sample.jsonl` was built to answer, and it now sets the gate's operating point rather than its existence.
9. **D3 as re-specified is usable.** With the fixed counter it reads 0.71 rings/loop and 49.9 % prevalence rather than 13.17 and 64.1 %. The census's advice ("fix the overlap guard and re-derive M6 from the per-leg maximum before proposing a threshold") is now actionable; the fix should move into `tools/loopqual` proper.
10. **The distinctness ratchet needs a decision.** As written (ratchet 9, ≤ 1.02×) it blocks P1 outright. Either it is the wrong bar for a change that deliberately narrows the option space, or distinctness is a hard product constraint and P2 owes it a mechanism.

---

## Appendix A — commands

Branch tip: **`proto/v4-p1`**, see §Appendix C. Nothing pushed; `curvature-costing` untouched.

Build (the warm audit image, one long-lived container, `docker cp` + incremental make — 1–2 min/iteration):

```bash
docker run -d --name rt-p1-build -p 8003:8003 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  valhalla-fork-test:audit bash -lc "sleep infinity"
docker cp <file> rt-p1-build:/src/valhalla/<file>       # per changed source
docker exec rt-p1-build bash -lc \
  "cd /src/valhalla && cmake -B build && make -C build -j4 valhalla_service \
     gurka_roundtrip_audit gurka_motorcycle_roundtrip gurka_roundtrip_distinctness"
# NOTE: `make` installs nothing — run build/valhalla_service, not the image's
# /usr/local/bin/valhalla_service (which is still b4f514d7f).
```

Engines (both on the census tiles; `valhalla-local` on :8002 never addressed):

```bash
# P1, :8003 — stage-timing ledger on so the identity/ranking lines are readable
docker exec rt-p1-build python3 -c "
import json; d=json.load(open('/custom_files/valhalla.json'))
d['httpd']['service']['listen']='tcp://*:8003'; d['thor']['roundtrip_stage_timing']=True
json.dump(d, open('/tmp/v8003.json','w'))"
docker exec -d rt-p1-build bash -lc \
  "exec /src/valhalla/build/valhalla_service /tmp/v8003.json 2 > /tmp/engine-p1.log 2>&1"

# twins-only variant: same binary, one config flip
docker exec rt-p1-build python3 -c "
import json; d=json.load(open('/tmp/v8003.json'))
d['thor']['roundtrip_parallel_tier']=False; json.dump(d, open('/tmp/v8003-twinsonly.json','w'))"

# baseline control, :8004 — the pinned census engine, same session
docker run -d --name rt-p1-base -p 8004:8004 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  valhalla-curvature:prod-equivalent-b4f514d7f bash -lc "...listen tcp://*:8004... && \
  exec valhalla_service /tmp/v8004.json 2"
```

Runs (nothing under `tools/loopqual/` written except each run's own output dir):

```bash
cd /Users/xenix/Projects/curvagen-valhalla/tools/loopqual
./loopqual run --engine http://localhost:8003 --corpus corpus-v2.yaml \
  --out results/p1-road-identity/ --trace-engine http://localhost:8003 \
  --engine-note "proto/v4-p1 @ <sha> (twins+parallels)"        # 313 s fire, 223 s analyse, 89 s way
./loopqual run --engine http://localhost:8004 --corpus corpus-v2.yaml \
  --out results/census-v2-control/ --no-way \
  --engine-note "b4f514d7f baseline, same-session latency control"
./loopqual run --engine http://localhost:8003 --corpus corpus-v2.yaml \
  --out results/p1-twins-only/ --no-way \
  --engine-note "proto/v4-p1 @ <sha> (twins only, parallel tier off)"

./loopqual compare results/census-v2-b4f514d7f/report.json results/p1-road-identity/report.json
python3 gate_v1_proto.py results/census-v2-control results/p1-road-identity
python3 gate_v1_proto.py results/census-v2-control results/p1-twins-only
python3 gate_v1_proto.py results/census-v2-control results/census-v2-b4f514d7f   # the before column
```

Detectors — the census scripts, **re-pointed by env and running the fixed D3 counter**, so both runs are read by byte-identical logic:

```bash
cd ~/.curvagen-scratch/p1        # lqbs_lib.py here = the census lib + the fixed overlap guard
R=.../results/p1-road-identity
python3 lqbs_measure.py  $R ~/.curvagen-scratch/p1v2.jsonl
python3 lqbs_rings.py    $R ~/.curvagen-scratch/p1v2_rings.jsonl
python3 lqbs_xing.py     $R ~/.curvagen-scratch/p1v2_xing.jsonl
python3 lqbs_stemgap.py  $R ~/.curvagen-scratch/p1v2_stem.jsonl
python3 lqbs_lobe.py     $R ~/.curvagen-scratch/p1v2_lobe.jsonl
python3 census_mech.py   $R ~/.curvagen-scratch/p1v2_mech.jsonl
# and the same six against the census run with prefix censusv2f (the re-read)

LQ_RUN=$R LQ_PREFIX=p1v2 python3 census_agg.py [v13|det|mech|slot|curv|vracar]
LQ_RUN=$R LQ_PREFIX=p1v2 python3 p1_gallery.py $R/gallery-p1.html
```

Engine ledger (3 workers interleave, so only aggregates are attributable — which is all the
fallback share and the identity counters need):

```bash
docker exec rt-p1-build bash -lc "grep -o 'roundtrip ranking:.*' /tmp/engine-p1.log" | ...
docker exec rt-p1-build bash -lc "grep -o 'roundtrip identity:.*' /tmp/engine-p1.log" | ...
docker exec rt-p1-build bash -lc "grep -o 'roundtrip timing:.*'  /tmp/engine-p1.log" | ...
```

## Appendix B — artefacts

| Path | What |
|---|---|
| `tools/loopqual/results/p1-road-identity/` | the main run: 552 responses, `loops.jsonl` (6 617), `report.{json,md}` |
| **`tools/loopqual/results/p1-road-identity/gallery-p1.html`** | **40-case gallery: 20 worst P1 + 20 worst census, same axis** |
| `tools/loopqual/results/p1-twins-only/` | the cheaper variant (parallel tier off) |
| `tools/loopqual/results/census-v2-control/` | same-session baseline re-run; reproduces the census to the metre |
| `~/.curvagen-scratch/p1/lqbs_lib.py` | the census detector lib **with the D3 overlap guard fixed** |
| `~/.curvagen-scratch/p1/{lqbs_*,census_*}.py` | the census scripts, env-parameterised (`LQ_RUN`, `LQ_PREFIX`) |
| `~/.curvagen-scratch/p1/p1_gallery.py` | the side-by-side gallery builder |
| `~/.curvagen-scratch/{p1v2,p1to,censusv2f}*.jsonl` | detector output: P1 / twins-only / census re-read |
| `~/.curvagen-scratch/p1-{run,twins,control}.log` | the three run logs |
| `~/.curvagen-scratch/p1/logs/engine-{p1,p1-twins,baseline}.log` | the three engine ledgers: sidecar build, identity counters, ranking order, stage timing |

The two containers `rt-p1-build` (the P1 build + engine, :8003) and `rt-p1-base` (the baseline, :8004) are **stopped, not removed**, so a follow-up iteration re-uses the warm `build/` without a rebuild. `valhalla-local` (:8002) was never addressed.

## Appendix C — the branch

`proto/v4-p1`, cut from `curvature-costing` @ `c23ff378f`. Two commits, both marked `proto(v4-p1):` — **`153fc4220`** (the mechanisms + gurka) and this document on top, which is the branch tip. `git log --oneline -2 proto/v4-p1` on the fork. Nothing pushed; `curvature-costing` is untouched and carries this document as an untracked file at the same path, for the serving branch to read before Andrey's go.

Touched: `valhalla/thor/road_twin_index.h` + `src/thor/road_twin_index.cc` (new), `src/thor/CMakeLists.txt`, `src/thor/route_action.cc`, `src/thor/roundtrip_expansion.cc`, `valhalla/thor/roundtrip_expansion.h`, `src/thor/worker.cc`, `valhalla/thor/worker.h`, `test/gurka/test_roundtrip_audit.cc`, `test/gurka/test_motorcycle_roundtrip.cc`.

Config surface added (all `thor.*`, all live in the prototype):

| knob | default | meaning |
|---|---|---|
| `roundtrip_road_identity` | `true` | the whole mechanism; `false` = exact v3 behaviour |
| `roundtrip_twin_radius_m` | `30` | twin qualification radius |
| `roundtrip_parallel_radius_m` | `80` | parallel qualification radius |
| `roundtrip_parallel_tier` | `true` | `false` = twins only (§9) |
| `roundtrip_simple_chains` | `true` | F01 non-simple chain rejection |

**Primary sources:** `docs/curvagen/research/2026-09-06-defect-atlas-v2.md` §3/§5/§7/§9/App. A · `2026-09-05-v3-correctness-optimality-audit.md` F01/F02/F04/F05/F06/F11/F20 · `2026-09-05-audit-rig-confirmation.md` §3/§6 · `tools/loopqual/{metrics.py,runner.py,gate_v1_proto.py,corpus-v2.yaml}` · `src/thor/{route_action.cc,roundtrip_expansion.cc,road_twin_index.cc}` · `valhalla/sif/dynamiccost.h`.

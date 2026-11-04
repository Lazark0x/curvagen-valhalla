# Shuffle variety: what makes consecutive round-trip Shuffles differ, and what it costs

- **Date:** 2026-11-04. Prompted by the K=3 Shuffle repeat finding (curvagen-meta#27, curvagen-orchestrator#67). Sits next to the in-flight apex-exclusion fix (fork PR #29 / orchestrator PR #68).
- **Question:** what makes consecutive round-trip Shuffles serve more varied loops while keeping today's route quality? The user set the quality budget during the session: p90 distance error |served − requested| / requested **< 30 %**, with the other Gate v2 meters and latency (≤ 1.10× p50) still holding.
- **Scope:** a **throwaway prototype** on branch `research/shuffle-variety`, cut from `curvature-costing` @ `5b29936f0`, which is prod. Every lever is an opt-in request field (`roundtrip.var_*`, `exclude_*`). With none of them set, the binary is byte-identical to prod: 216/216 serves matched, re-checked after every rebuild. Nothing is deployed, nothing touches `curvature-costing`, and no production traffic was used.
- **Method:** one engine at a time on the rig's Serbia tiles. The harness `tools/shuffle-variety/variety.py` emulates the orchestrator: a fresh random seed per Shuffle, K = 3, Bank Fill off, and the exact `engine-client` request shape and costing. It runs 6 consecutive Shuffles per cell over **36 cells** = 6 city-level origins (Belgrade Vračar, Novi Sad, Niš, Kragujevac, Zlatibor, Subotica) × 30/60/120 km × curviness 0.5/1.0, motorways allowed. That is 216 requests and 648 loops per run. Quality comes from the `tools/loopqual` metric bank, applied to every served loop (§2).

## Vocabulary first: a "turnaround" is not a U-turn

In this engine a **turnaround** is the loop's **apex**: the far point where the ride stops heading out and starts heading home (`Turnaround` in `thor/roundtrip_expansion.h`, harvested at ~target/2 path distance). Nothing turns around in place. Since v4 the U-turn door is dropped, and the seam is an ordinary junction (ADR-0042). "Apex exclusion" (`exclude_turnarounds`) means "don't build a loop whose far point lies within R of a far point the Rider has already been shown". This document says **apex** throughout.

## TL;DR

**Only memory makes Shuffles vary.** A stateless engine cannot stop repeating with any seed trick, because the pool the seed draws from is 1–2 viable loops per bearing sector. The lever that works is the in-flight one: the orchestrator remembers the apexes it served, and the engine drops candidates near them. Its radius is the variety↔quality dial. Every other lever either adds little on top of it or costs a meter.

Ranked by repeats among the stacks inside the budget. "Repeat" = a loop in Shuffles 2–6 whose geometry overlaps an earlier-served loop of the same session by ≥ 0.9. Today's engine scores **92.0 %** on this corpus (the known prod-rig figure). The deployed distance rotation scores **51.7 %**.

| # | stack | repeats ≥0.9 | ≥0.8 | p90 dist err | curviness c0.5 / c1.0 (today 326 / 320; best-16 ref. 292 / 299) | R1 retrace | T2 served distinctness | fallback + rescue | p50 latency | change type | FINGERPRINT |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | **apex r = 0.08·d** (PR #29/#68 as built) + **deep pick** | **6.1 %** | 20.7 % | 0.217 | 288 / 298 | 3.4 % | 0.146 | 0.2 + 3.5 % | 1.08× | engine field + orch memory | none (opt-in) |
| 2 | apex r = 0.08·d alone | 6.3 % | 20.4 % | 0.217 | 289 / 299 | 2.9 % | 0.158 | 0.3 + 4.6 % | 1.09× | engine field + orch memory | none |
| 3 | apex r = 0.10·d + deep pick | 4.6 % | 13.3 % | 0.269 | 279 / 293 | 3.7 % | 0.165 | 1.5 + 9.3 % | 1.09× | engine field + orch memory | none |
| 4 | apex r = 0.05·d + sector offset + distance rotation | 10.6 % | 29.4 % | 0.292 | 312 / 316 | 4.9 % | 0.153 | 0.8 + 1.4 % | 0.94×¹ | engine field + default change + orch | bump (offset) or opt-in |
| 5 | apex r = 0.05·d + distance rotation | 12.2 % | 29.3 % | 0.296 | 313 / 316 | 4.6 % | 0.161 | 0.2 + 1.5 % | 0.95×¹ | engine field + orch | none |
| — | today (prod) | 92.0 % | 94.6 % | 0.186 | 326 / 320 | 6.2 % | 0.153 | 0 + 0.6 % | 1.00× | — | — |
| — | distance rotation (deployed, #67) | 51.7 % | 60.7 % | 0.264 | 324 / 322 | 5.7 % | 0.158 | 0 + 0.6 % | n/m² | orch only | none |

¹ Rotation asks for a slightly different distance per Shuffle. Its latency is not comparable cell for cell, but it stays inside the bar. ² Not measured in the latency bracket (§5).

**Curviness retention, read honestly.** Rows 1–3 lose 7–12 % curviness against *today*. The loss is the price of variety, not of the lever: today's "quality" is the same 3–4 curviest loops served again and again. The engine's **own best 16 distinct loops** for the same cells (one K = 16 call, the `oracle16` reference, §2) average **292 / 299**. Rows 1–2 sit at **0.99× / 1.00×** of that reference, row 3 at 0.95× / 0.98×. No scheme that serves ~17 distinct loops in 6 Shuffles can keep today's 326 / 320. Read against today (Gate v2's T1 bar of 0.95×), only rows 4–5 pass. That is why they are listed: they are the best you can do if T1 must hold against today's repeats. For that they pay 2× the repeats and a p90 at the budget's edge (0.29).

**Recommended stack: row 1.** Ship PR #29/#68 at **r = 0.08·d** and **retire the distance rotation** once it ships. Rotation is redundant at r ≥ 0.08 (apex08 + rotation: 6.1 % vs 6.3 % alone) and pushes p90 over budget (0.317). At 0.06·d it still helps (11.3 → 6.9 %, p90 0.297). Add the **deep pick** (best of the top-32 pairs per sector, which only runs when exclusions are present): it lowers T2 and latency for free. Go to **r = 0.10·d** only together with a density-aware guard. At 0.10, the aggregate p90 (0.27) hides the sparse cells: Zlatibor p90 **0.59** with 26 % rescue/fallback loops, Subotica p90 0.40 (§4.3). The blunt guard prototyped here fixes the defects but gives back most of the gain (18–24 % repeats). If near-twins are the remaining complaint, see the **identity filter** at 0.8 (§3.4): apex08 + identity reaches **0.6 %** repeats and halves the ≥ 0.8 class (20 → 11.5 %), at T2 1.12× and fallback 1.5 %. It needs a compact sketch on the wire before it is shippable.

## 1. Why the seed barely moves the answer

Code at prod `5b29936f0`, `src/thor/route_action.cc`:

1. Harvest candidates (apexes) are bucketed into `want = num_candidates` bearing sectors: three 120° sectors at K = 3 (l. 1552). The comment at l. 1536 says "the shuffle seed rotates the bucket origin". **It does not.** Sector boundaries are fixed at 0/120/240°.
2. Per sector, the **top 8 by harvest-chain curviness** (`roundtrip_pair_shortlist`, l. 1860) get a pair evaluation. Survivors are ranked by pair score, and the seed picks `surv[(seed + b) % surv.size()]` (l. 1896). `kShuffleTopM` (l. 1846) is dead code on the pair path.
3. Empty sectors and failed builds fall through to the **backfill** (l. 1912) and the **refill queue** (l. 1952). Both are curviness-sorted and seed-independent.

The prototype ledger (`roundtrip variety: … sectors=` with per-sector reject reasons) over 814 base requests, 2 346 sectors and 18 768 shortlist slots:

| per-sector outcome at the top-8 shortlist | share |
|---|---|
| `no_pair` (no edge-disjoint way home from that junction) | 46.0 % of slots |
| `band` (pair total outside ±20 % of target) | 21.1 % |
| `fwd_illegal` / `sep` / `leg` (forward trunk shared with a chosen loop) / `twin` / `share` | 3.6 / 1.7 / 1.4 / 1.3 / 0.5 % |
| **sectors with zero survivors** (filled by the seed-independent backfill) | **39.6 %** |
| survivors per sector, mean | 1.95 |

Two measurements show the seed is nearly inert. Removing the seed's choice entirely (the `deep` pick: always the best survivor) moves repeats from **92.0 to 97.8 %**, so the seed is worth ~6 pp today. Widening the shortlist to 16/32, and even keeping only *mutually distinct* survivors (`dist`), leaves repeats at 86–96 %. The curviest candidates of a sector cluster on one curvy road and mostly have no disjoint pair. The viable distinct pool at the top of the order is 1–2 loops.

**The stateless ceiling.** Even a perfect seed lever that drew uniformly from m distinct good loops per slot repeats by birthday arithmetic. The expected share of Shuffle 2–6 loops that repeat is **81 % at m = 2, 54 % at m = 4, 32 % at m = 8, 17 % at m = 16, 9 % at m = 32**. The rig has m ≈ 1–2. Seed levers cannot get under ~30 % without a pool of a dozen good distinct loops per sector, and the network doesn't have that pool at curviness parity (§3.2). Hence memory.

## 2. Measurement

- **Engines:** `valhalla_service` built from this branch in a `valhalla24-deps:local` container (arm64, Release), served on :8005 with `server_threads=2`. Tiles are the orchestrator's `data/` (Serbia, `tileset_last_modified` 1785932567). The knobs-off binary matches the prod-HEAD binary on all 216 base serves, and every later rebuild was re-checked (72/72).
- **Repeat meter:** loops are decoded from polyline6 and gridded at 0.0003° (~30 m). Overlap is min(cover(a→b), cover(b→a)), where cover counts points within a ±1-cell neighbourhood. A loop of Shuffle n ≥ 2 is a **repeat** if it overlaps any loop served earlier in its 6-Shuffle session by ≥ 0.9; the ≥ 0.8 column catches near-twins. "Distinct /18" counts loops per session that are < 0.9 to everything before them. The meter is the same as `twins.py` in the K=12 probe. It reproduces the known **92.0 %** (today) and **51.7 %** (rotation), and reproduces PR #29's own **6.3 %** at r = 0.08·d.
- **Quality meters:** `tools/loopqual/metrics.analyze_loop` runs on every served loop (648 per run). Gate v2 (ADR-0041) is adapted to K = 3 Shuffles, with the served surface = every loop served:
  - **budget p90** `distance_error` < 0.30;
  - **R1** Retrace family share ≤ today + 1 pp (today is 6.2 % on this corpus, already above Gate v2's 5 % absolute);
  - **R2** `spike_ge_500m` = 0;
  - **R4** every serve returns 3;
  - **T1** curviness retention (`curviness_geom_clean`) ≥ 0.95×, read against today *and* against the `oracle16` reference;
  - **T2** served distinctness `max_pair_overlap_served` mean ≤ 1.10× today, near-dup > 0.6 ≤ today + 5 pp;
  - **T3** wall p50 ≤ 1.10×, from a dedicated same-session bracket (§5).

  Near-mirror metres (`am25_new_m`, T5's input) and the engine's own `provenance` (rung-2 Fallback, `rescue` builder, `gated`, `topup`) are reported alongside.
- **`oracle16` reference:** one K = 16 request per cell (`tools/shuffle-variety/oracle.py`), i.e. the engine's own best 16 distinct loops. It is the yardstick for "quality of the n-th distinct loop": 292 / 299 curviness, p90 0.317, rescue 25 %.
- **Determinism:** every variant re-fired in the latency session was **byte-identical** to its first run (9 variants × 216 serves). Excluded requests are deterministic per (seed, exclusion list).
- **Noise:** 540 later-Shuffle loops per run means ±1–1.5 pp on the repeat share (cell-clustered). Rows that differ by < 2 pp are ties.

## 3. Single levers

| lever | knob | repeats ≥0.9 | ≥0.8 | curv c0.5 / c1.0 | R1 | p90 err | T2 | fallback / rescue | verdict |
|---|---|---|---|---|---|---|---|---|---|
| today | — | 92.0 | 94.6 | 326 / 320 | 6.2 | 0.186 | 0.153 | 0 / 0.6 | — |
| **L1 apex exclusion** r = 0.025·d | `exclude_turnarounds`, `exclude_radius_m` | 32.0 | 53.9 | 321 / 326 | 5.6 | 0.186 | 0.178 | 0 / 2.6 | T2 fails |
| L1 r = 0.05·d (min-separation default) | | 17.0 | 37.6 | 309 / 314 | 4.8 | 0.189 | 0.159 | 0 / 2.3 | T1 0.947× (c0.5) |
| L1 r = 0.06·d | | 11.3 | 29.4 | 306 / 310 | 4.6 | 0.195 | 0.157 | 0.3 / 2.0 | T1 vs today fails |
| **L1 r = 0.08·d** | | **6.3** | 20.4 | 289 / 299 | 2.9 | 0.217 | 0.158 | 0.3 / 4.6 | T1 = oracle |
| L1 r = 0.10·d | | 3.5 | 12.6 | 279 / 289 | 3.9 | 0.287 | 0.188 | 2.0 / 9.0 | T2 1.23×, latency 1.14× |
| L1 r = 0.12·d | | 3.3 | 9.1 | 272 / 282 | 3.1 | 0.363 | 0.208 | 1.4 / 12.5 | over budget |
| L1 r = 0.15·d | | 4.8 | 8.7 | 269 / 279 | 3.1 | 0.503 | 0.235 | 1.5 / 18.4 | over budget, top-ups 3.4 % |
| **L2** seed-rotated sector origin | `var_sector_offset` | 83.3 | 86.7 | 322 / 326 | 6.9 | 0.186 | 0.146 | 0 / 0.6 | free, weak |
| **L3a** top-16 shortlist, seed over all | `var_shortlist` 16 | 86.1 | 90.6 | 321 / 311 | 5.9 | 0.182 | 0.130 | 0.2 / 0.8 | weak |
| L3b top-16, near-best (15 %) only | `+ var_pick_tol` 0.15 | 91.7 | 93.9 | 321 / 314 | 5.2 | 0.178 | 0.131 | 0.3 / 0.9 | inert |
| L3c top-32, mutually distinct survivors | `var_distinct` 0.5 | 89.4 | 90.2 | 313 / 312 | 6.6 | 0.172 | 0.130 | 0 / 0.8 | weak |
| L3d top-32, best survivor (no seed) | `var_shortlist` 32, `var_pick_tol` ε | 97.8 | 98.1 | 326 / 324 | 4.8 | 0.189 | **0.121** | 0 / 1.2 | quality-positive, no variety |
| **L5** 12 sectors, K spread ones first | `var_buckets` 12 | 62.0 | 68.0 | 290 / 298 | 3.4 | 0.185 | 0.104 | 0.9 / 0 | T1 fails vs today |
| **L4** identity filter, share > 0.5 | `exclude_loops`, `emit_identity` | 50.8 | 54.8 | 311 / 322 | **15.9** | 0.256 | 0.383 | 12.0 / 6.4 | R1/R2/R4/T2 fail |
| L4 share > 0.3 | `exclude_loop_frac` 0.3 | 68.7 | 72.0 | 318 / 324 | 20.0 | 0.251 | 0.433 | 15.5 / 3.3 | fails |
| L4 share > 0.8 | `exclude_loop_frac` 0.8 | 16.3 | 36.5 | 309 / 323 | 5.7 | 0.207 | 0.253 | 1.7 / 8.5 | T2 fails, R2 = 2 |
| **L6a** distance rotation (deployed) | orchestrator | 51.7 | 60.7 | 324 / 322 | 5.7 | 0.264 | 0.158 | 0 / 0.6 | in budget |
| L6b curviness micro-jitter ±0.05·⌈n/2⌉ | orchestrator | 78.1 | 83.0 | 320 / 324 | 6.3 | 0.186 | 0.155 | 0 / 1.9 | weak |
| L6c start-point jitter 300 m | orchestrator | 48.6 | 63.3 | 312 / 318 | 3.6 | 0.194 | 0.162 | 0 / 0.9 | one 442; moves the Rider's start |

### 3.1 L1, apex exclusion (the in-flight lever)

The prototype mirrors PR #29's semantics: drop harvest candidates, and Distance-Flex re-scan candidates, within R of a served apex; if the excluded run is short of K, top it up from one plain run. The radius is the dial. Measured on this harness, **0.08·d gives 6.3 %**, which matches the PR's own end-to-end figure (6.3 %, p90 20 %), and 0.10·d gives 3.5 % (PR: 4.6 %, p90 26 %).

Curviness per Shuffle shows the cost is a walk down the pool, not a defect:

| Shuffle | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| today | 324 | 321 | 323 | 323 | 322 | 325 |
| apex 0.05·d | 324 | 325 | 315 | 311 | 303 | 293 |
| apex 0.08·d | 324 | 317 | 306 | 293 | 270 | 257 |
| apex 0.10·d | 324 | 318 | 294 | 263 | 256 | 251 |
| apex 0.05·d + rotation | 324 | 322 | 316 | 313 | 309 | 302 |

Retrace (R1) *improves* with exclusion (6.2 → 2.9–4.8 %): the loops it leaves out include the curvy-but-retracing ones. Fill holds at 100 % everywhere up to 0.12·d with no top-ups (top-ups first appear at 0.15·d). The cost moves into rescue-built and fallback loops in sparse cells as the radius grows (§4.3). Latency is flat up to 0.08·d (1.09×); 0.10·d is 1.14×.

### 3.2 L2/L3/L5, the seed levers: weak, as §1 predicts

Rotating the sector origin (L2) is free and buys 9 pp. Widening or deduplicating the shortlist (L3a–c) buys 3–6 pp. All of them stay ≥ 83 %. The 12-sector spread (L5) reaches 62 %, but narrow sectors force loops into poor directions (curviness 290 / 298, fails T1 vs today). **L3d (deep pick)** is the useful by-product. Evaluating 32 per sector and serving the best survivor gives no variety alone, but it is quality-positive: T2 0.121 vs 0.153, R1 4.8 %, latency neutral. That makes it the right *companion* to exclusion, where memory supplies the variety and the pick should maximise quality.

### 3.3 L6, orchestrator-only

- Distance rotation (deployed) halves repeats and is inside the 30 % budget alone (p90 0.264). Stacked on apex ≥ 0.06·d it adds nothing and breaks the budget.
- Curviness jitter is weak (78 %) and changes the asked-for curviness.
- Start-point jitter is the strongest orchestrator-only lever (48.6 %) and even improves retrace. It is **rejected on product grounds**: the loop no longer starts where the Rider is, and one cell returned a 442 (R4).

### 3.4 L4, identity filter (served loops by road identity)

The engine emits each loop's road identity: the canonical edge ids beyond the Start Exemption, via `loop_keylen`. The orchestrator returns them on the next Shuffle, and the engine rejects candidates whose twins-aware `shared_fraction` with a served loop exceeds the threshold, both at selection and on the built loop.

At ≤ 0.5 this is **harmful**. Almost every candidate leaving a city shares > 50 % with one of 3–15 served loops, so 2 000+ candidates per request are rejected. The walk falls through to the rescue pass and soft-leash fallbacks: 12–16 % fallback, R1 16–20 %, 40–53 exact-mirror spikes, top-ups 20–29 % (which are repeats), and 2× latency.

At **0.8** it is a near-twin catcher. Alone it reaches 16.3 % (≈ apex 0.05·d). On top of apex 0.08·d it reaches **0.6 %** and halves the ≥ 0.8 class (20.4 → 11.5 %). Wire cost: 160–1 060 ids per loop (2–15 KB JSON per loop, × up to 15 served loops). Production would need a sketch, e.g. a 1-in-8 hash-sampled id set or MinHash, plus a new response field.

## 4. Stacks

### 4.1 Pairwise and triple stacks

| stack | repeats | ≥0.8 | curv c0.5 / c1.0 | R1 | p90 | T2 | fallback / rescue | budget (T1 vs today) |
|---|---|---|---|---|---|---|---|---|
| apex 0.05 + rotation | 12.2 | 29.3 | 313 / 316 | 4.6 | 0.296 | 0.161 | 0.2 / 1.5 | **pass** |
| apex 0.05 + offset + rotation | **10.6** | 29.4 | 312 / 316 | 4.9 | 0.292 | 0.153 | 0.8 / 1.4 | **pass** |
| apex 0.05 + curviness jitter | 14.3 | 34.8 | 312 / 316 | 4.8 | 0.192 | 0.162 | 0 / 2.6 | **pass** |
| apex 0.05 + offset | 13.9 | 36.7 | 309 / 314 | 4.8 | 0.190 | 0.160 | 0 / 2.5 | T1 0.947× |
| apex 0.05 + distinct (L3c) | 13.0 | 33.7 | 303 / 308 | 4.5 | 0.195 | 0.141 | 0 / 2.5 | T1 |
| apex 0.05 + deep | 14.4 | 33.7 | 306 / 310 | 4.0 | 0.192 | 0.152 | 0.3 / 1.9 | T1 |
| apex 0.05 + identity 0.8 | 3.3 | 19.8 | 303 / 312 | 3.9 | 0.210 | 0.170 | 0.3 / 5.1 | T1, T2 |
| apex 0.05 + distinct + identity 0.8 | 3.3 | 20.9 | 300 / 306 | 3.2 | 0.199 | 0.155 | 0 / 4.2 | T1 |
| apex 0.05 + distinct + rotation | 10.6 | 27.4 | 307 / 310 | 4.8 | 0.292 | 0.136 | 0.2 / 1.1 | T1 |
| apex 0.05 + rotation + deep | 11.1 | 28.5 | 313 / 312 | 4.8 | 0.307 | 0.132 | 0.2 / 0.8 | p90 |
| apex 0.06 + rotation | 6.9 | 23.9 | 308 / 314 | 3.9 | 0.297 | 0.155 | 0.2 / 0.9 | T1 0.944× |
| apex 0.08 + rotation | 6.1 | 18.3 | 305 / 308 | 4.0 | **0.317** | 0.157 | 0.2 / 1.7 | p90 |
| apex 0.08 + distinct | 5.7 | 20.0 | 285 / 295 | 3.5 | 0.215 | 0.147 | 0.3 / 4.3 | T1 (= oracle) |
| **apex 0.08 + deep** | **6.1** | 20.7 | 288 / 298 | 3.4 | 0.217 | **0.146** | 0.2 / 3.5 | T1 (= oracle) |
| apex 0.08 + identity 0.8 | **0.6** | 11.5 | 286 / 297 | 4.0 | 0.267 | 0.172 | 1.5 / 6.6 | T1 (= oracle), T2 1.12× |
| apex 0.08 + identity 0.8 + deep | 1.5 | 12.4 | 285 / 296 | 4.3 | 0.226 | 0.170 | 1.4 / 5.4 | T1 (= oracle), T2 1.11× |
| apex 0.10 + rotation | 3.5 | 11.1 | 292 / 301 | 4.8 | 0.339 | 0.173 | 0.8 / 4.0 | p90, T2 |
| apex 0.10 + distinct | 3.9 | 13.9 | 278 / 289 | 3.4 | 0.268 | 0.175 | 1.7 / 8.5 | T2 |
| **apex 0.10 + deep** | 4.6 | 13.3 | 279 / 293 | 3.7 | 0.269 | **0.165** | 1.5 / 9.3 | T1 (0.95–0.98× oracle) |
| apex 0.10 + curviness jitter | 3.1 | 10.6 | 280 / 289 | 3.9 | 0.282 | 0.192 | 1.5 / 7.6 | T2 |

### 4.2 Overlap of effects, redundancy, additive costs

- **Rotation is redundant once exclusion is ≥ 0.08·d.** At 0.05·d it still helps: 17.0 → 12.2 %, against an independent-effects expectation of 17.0 × 0.517 = 8.8 %, so the effects overlap by ~40 %. At 0.06·d it helps less (11.3 → 6.9 %, p90 0.297, at the budget's edge). At 0.08·d it adds nothing (6.3 → 6.1 %), and its distance cost is additive (p90 0.217 → 0.317). **Retire rotation when apex exclusion ships at ≥ 0.08·d.** Below that, rotation still pays, and at 0.05·d it is also the only way to pass T1 vs today.
- **Rotation does buy curviness at small radii.** Shifting the target band surfaces fresh *curvy* loops instead of the next-curviest at the same distance (Shuffle 6 at 0.05·d: 302 with rotation vs 293 without). That is why rows 4–5 of the TL;DR pass T1.
- **The seed levers (L2, L3c) are mostly subsumed by exclusion.** On top of apex 0.05 they add 3–4 pp (17 → 13–14 %). L2 costs nothing. L3c costs curviness.
- **Identity 0.8 is complementary.** It targets the near-twin class that apex exclusion misses: the same roads ridden from a different apex, which is 20 % of loops at ≥ 0.8 under apex08. Its costs add (T2, fallback, payload).
- **Deep pick is the one free companion.** It never adds repeats beyond noise (6.3 → 6.1 at 0.08, 3.5 → 4.6 at 0.10, within ±1.5 pp), cuts T2 by 8–12 %, and brings apex10 under the latency bar (1.14× → 1.09×).

### 4.3 Sparse and mountain cells (Zlatibor, Subotica)

**Fill held at 100 % in every apex stack**: no under-filled cell up to 0.12·d, thanks to the rescue pass and the plain-run top-up. The cost is the *kind* of loop that fills the slot, and it concentrates in the sparse cells. These are per-origin reads: repeats / rescue+fallback / p90 distance error.

| origin | today | apex 0.05·d | apex 0.08·d | apex 0.10·d | apex 0.10·d + deep | apex 0.08·d + guard |
|---|---|---|---|---|---|---|
| Belgrade Vračar | 100 / 0 / 0.22 | 10 / 1 / 0.21 | 8 / 4 / 0.23 | 3 / 6 / 0.28 | 2 / 6 / 0.32 | 17 / 0 / 0.21 |
| Novi Sad | 89 / 0 / 0.17 | 18 / 0 / 0.19 | 7 / 0 / 0.27 | 4 / 0 / 0.20 | 4 / 0 / 0.20 | 19 / 0 / 0.19 |
| Niš | 91 / 0 / 0.18 | 13 / 0 / 0.19 | 4 / 0 / 0.19 | 1 / 0 / 0.19 | 2 / 0 / 0.18 | 6 / 0 / 0.19 |
| Kragujevac | 89 / 0 / 0.16 | 27 / 1 / 0.18 | 2 / 2 / 0.20 | 1 / 4 / 0.19 | 3 / 1 / 0.20 | 9 / 0 / 0.19 |
| **Zlatibor** | 88 / 4 / 0.19 | 11 / 8 / 0.19 | 7 / 16 / **0.30** | 3 / **26** / **0.59** | 6 / **28** / **0.59** | 28 / 1 / 0.18 |
| **Subotica** | 96 / 0 / 0.17 | 23 / 4 / 0.19 | 10 / 8 / **0.33** | 8 / **21** / **0.40** | 10 / **23** / **0.39** | 30 / 0 / 0.18 |

At 0.10·d the aggregate p90 (0.27–0.29) is inside budget, but **Zlatibor's p90 is 0.59 and a quarter of its loops are rescue or fallback builds**. Zlatibor is a mountain plateau with a sparse network. Subotica's half-compass is clipped by the border.

The prototype **quality guard** (`var_guard` 0.30) drops excluded-run loops that are rescue, fallback or gated, or off by > 30 %, and tops up from the plain run. It fixes every per-origin defect (p90 ≤ 0.21, fallback+rescue ≤ 2 % everywhere), but the top-ups are repeats: 18 % (0.08·d), 23.5 % (0.10·d), 25 % (0.12·d) aggregate, and 28–47 % in the sparse cells. It is too blunt to ship as is.

The right next prototype is a **density-adaptive radius**: shrink R per cell when the excluded run starts producing rescue builds, or scale R by the harvest's candidate density. That keeps 0.08–0.10·d in dense cells and ~0.05·d in Zlatibor and Subotica. This matters doubly because **prod's tileset is wider** (364 tiles, Balkans, vs the rig's 211, Serbia only). Subotica's border clipping, in particular, does not exist on prod.

## 5. Latency (same-session bracket)

One engine, workers = 2, 216 requests per run. The machine was quiet (the other agent's engines were stopped). Two base brackets ran before and after the variants and agree within 0.5 % (p50 0.212 / 0.211 s).

| run | p50 | ×base | p95 | ×base | mean | ×base |
|---|---|---|---|---|---|---|
| base (2 brackets) | 0.212 / 0.211 s | 1.00 | 1.06 / 0.99 s | 1.00 | 0.290 / 0.281 s | 1.00 |
| apex 0.05 + offset + rotation | 0.198 | 0.94 | 1.05 | 1.02 | 0.301 | 1.06 |
| apex 0.05 + rotation | 0.200 | 0.95 | 0.95 | 0.92 | 0.300 | 1.05 |
| apex 0.08 | 0.231 | 1.09 | 0.84 | 0.82 | 0.288 | 1.01 |
| **apex 0.08 + deep** | 0.229 | **1.08** | 0.83 | 0.81 | 0.298 | 1.05 |
| apex 0.08 + identity 0.8 | 0.229 | 1.08 | 1.05 | 1.02 | 0.357 | **1.25** |
| apex 0.10 | 0.242 | **1.14** ✗ | 0.93 | 0.90 | 0.329 | 1.15 |
| apex 0.10 + deep | 0.232 | 1.09 | 1.01 | 0.99 | 0.321 | 1.13 |

The p50 includes Shuffle 1, which is unchanged. Read on Shuffles 2–6 only, the excluded requests sit at p50 **1.11×** (apex 0.08), **1.10×** (apex 0.08 + deep), 1.17× (apex 0.10) and 1.13× (apex 0.10 + deep); means are 1.03 / 1.06 / 1.19 / 1.17×. Identity 0.8's mean is 1.32×. So on the excluded serves themselves, the 0.08·d stack sits *at* the 1.10× bar, not under it. The cause is that each served apex removes candidates the walk would have built first. A top-up costs a second full run, but top-ups are 0 % at ≤ 0.10·d. Engine memory was not read separately: exclusion adds a vector of ≤ 64 points, and the identity filter adds hash sets of ≤ 32 served loops.

## 6. Determinism and FINGERPRINT

- **Determinism:** with no exclusions, every knob is a pure function of (request, seed): 9 variants × 216 serves were byte-identical on re-run. With exclusions, the output is a pure function of (request, seed, exclusion list), but a *seed alone no longer reproduces a Shuffle*. The orchestrator already treats a nudged Shuffle as un-cacheable (`serving/mod.rs`: rotation sets `ctx = None`, so no Negative Cache and no Bank Fill under the un-nudged prefix). An excluded Shuffle must take the same path, and Shuffle serves are not memoized. A replay of a served seed (a share link) would need the served loops from the Serve Record, not a re-generation.
- **FINGERPRINT:** an opt-in request field that leaves unchanged requests byte-identical does **not** alter route output for any cached key. This was verified for the whole prototype (216/216 at knobs-off), so the rule in `extension-api.md` does not fire for L1 or L4, or for L3d gated on "exclusions present". It **does** fire for any lever that changes a default: L2 sector offset, L3/L5 as new defaults, or a guard applied to plain requests. The recommended stack (apex 0.08·d, deep pick only inside excluded runs, rotation retired) needs **no bump**. The first Shuffle of a request is today's cacheable answer, unchanged. Retiring rotation is orchestrator-only.

## 7. Lever ranking (for the follow-up issues)

| rank | lever | repeats alone / in recommended stack | quality cost | fill | latency | FINGERPRINT | effort |
|---|---|---|---|---|---|---|---|
| 1 | **L1 apex exclusion**, r = 0.08·d | 6.3 % / 6.1 % | curviness = the engine's own best-16; sparse cells p90 0.30–0.33 | 100 % | 1.09× (1.11× on Shuffles 2–6) | none (opt-in) | **in flight** (PR #29/#68) |
| 2 | **L3d deep pick** inside excluded runs | — / −0.2 pp, T2 −8 % | none (positive) | 100 % | −1 pp | none if gated on exclusions | ~20 lines engine |
| 3 | retire L6a distance rotation once (1) ships | — / keeps p90 at 0.22 instead of 0.32 | — | — | — | none | delete orchestrator#67's module |
| 4 | density-adaptive radius (next prototype) | expected between apex08 and apex+guard in sparse cells | targets Zlatibor/Subotica p90 | 100 % | ≈ | none (opt-in) | engine: per-cell R from harvest density, or a rescue-triggered shrink |
| 5 | L4 identity filter at 0.8 | 16.3 % / 0.6 % | T2 1.12×, fallback 1.5 %, mean latency 1.25× | 100 % | 1.08× p50 | none (opt-in) | new response + request field, needs an id sketch (2–15 KB/loop raw) |
| 6 | L2 seed-rotated sector origin | 83 % / −3 pp at 0.05·d | none | 100 % | ≈ | **bump** (default change) | 5 lines; also fixes the l. 1536 comment |
| — | L6a rotation **with** apex 0.05·d (if T1 vs today is binding) | 51.7 % / 12.2 % | p90 0.296 | 100 % | 0.95× | none | deployed |
| ✗ | L3a/b/c shortlist widening | 86–96 % | curviness −3 % | | | bump | not worth it |
| ✗ | L5 12 sectors | 62 % | curviness −10 % | | | bump | rejected |
| ✗ | L4 at ≤ 0.5 | 51–69 % | R1/R2/T2 fail | | 2× | | rejected |
| ✗ | L6b curviness jitter | 78 % | lies about curviness | | | | rejected |
| ✗ | L6c start jitter | 49 % | moves the Rider's start, one 442 | | | | rejected |

## 8. Caveats

- **Rig tiles are not prod's.** The rig is Serbia-only, 211 tiles; prod is 364 tiles, wider Balkans. Border-clipped cells (Subotica, and Niš's southern half) have a smaller pool here than on prod. Sparse-cell damage is probably *over*stated for the border cells and representative for Zlatibor's plateau. Absolute curviness and latency are rig numbers; the ratios are what transfer. Baseline v3 on rig tiles (curvagen-valhalla#19) applies.
- **Corpus:** 36 cells, 6 Shuffles each, motorways allowed (93 % of real demand), c0.5/1.0 only, no c0.7 and no avoid-motorways slice. Repeat shares carry ±1–1.5 pp noise; T1 at 0.947× and 0.944× is within noise of the bar.
- **The gate is adapted, not Gate v2 itself.** Gate v2 judges K = 12 banks on corpus-v2 against Baseline v2. Here it is K = 3 serves across Shuffles against today's v4. R1's 5 % absolute is exceeded by today's engine on this corpus (6.2 %), so it is read relative. **T1 vs today is structurally unattainable for any no-repeat scheme** (the `oracle16` reference); that framing is a proposal for Andrey's call, not a gate amendment.
- **Memory semantics:** the harness remembers every loop served in the session. Production memory is the orchestrator's: per request key, rider-agnostic, in memory, with a TTL, like the rotation's. A Rider whose Shuffles span > TTL, or two Riders at the same 5-dp start, behave differently.
- **PR #29 vs this prototype:** the prototype re-implements the PR's semantics on its own branch so stacks could be measured on one binary. The two agree within noise at 0.08·d (6.3 % vs 6.3 %) and roughly at 0.10·d (3.5 % vs 4.6 %). The PR's own engine was not measured from here.
- **Identity payload:** raw id lists were used for measurement only. A sketch changes the filter's precision and was not measured.

## Appendix A: prototype knobs (`research/shuffle-variety`)

All live under `roundtrip` in the `/route` request (`proto/descriptors/options.proto` `RoundTrip` 4–19, parsed in `src/worker.cc`). All default off, and a request with none of them is byte-identical to `5b29936f0`.

| field | lever | effect (`src/thor/route_action.cc`) |
|---|---|---|
| `exclude_turnarounds: [{lat, lon}]`, `exclude_radius_m` | L1 | drop harvest / Distance-Flex candidates within R of a served apex (default R = min-separation, 0.05·d). `roundtrip_variety()` tops an under-filled run up from one plain run (`provenance.topup`); `var_no_topup` disables the top-up |
| `var_sector_offset` | L2 | sector origin rotated by `splitmix64(seed) mod 360°` |
| `var_shortlist`, `var_pick_tol` | L3a/b/d | per-sector top-M (cap 64); the seed picks among survivors scoring ≥ (1 − tol) × best (ε = best only) |
| `var_distinct` | L3c | skip a survivor sharing more than this with an earlier survivor of the sector |
| `var_buckets` | L5 | sector count independent of K; K evenly spread sectors from a seed-rotated start are walked first |
| `emit_identity`, `exclude_loops: [[id…]]`, `exclude_loop_frac` | L4 | `provenance.identity` = the built loop's canonical edge ids beyond the Start Exemption. Candidates are rejected (selection, backfill, dequeue, post-build) when twins-aware `shared_fraction` with a served loop exceeds the fraction (default 0.5) |
| `var_guard` | L1g | inside `roundtrip_variety()`, drop excluded-run loops that are rescue / rung 2 / gated or above this distance error, then top up |

Ledger line `roundtrip variety: buckets= offset= apex_dropped= served= served_rejects= served_built_rejects= surv_dups= loops= seed= sectors= b:bucket/evaluated/survivors[reason=count,…]`.

## Appendix B: reproduce

```bash
# engine (arm64, in a valhalla24-deps:local container with this worktree mounted)
cmake /src -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=Off -DENABLE_PYTHON_BINDINGS=Off \
      -DENABLE_DATA_TOOLS=Off -DENABLE_TOOLS=On && make -j4 valhalla_service
./valhalla_service <orchestrator>/data/valhalla.json 2          # serve on a free port
# harness
cd tools/shuffle-variety
python3 variety.py fire http://localhost:<port> apex08+deep runs/apex08+deep   # 36 cells x 6 Shuffles
python3 oracle.py http://localhost:<port> runs/oracle16                       # best-16 reference
python3 variety.py analyze runs/base runs/apex08+deep && python3 table.py runs/base runs/apex08+deep
python3 byorigin.py apex08+deep                                               # sparse-cell read
```

Variant names compose with `+` (`variety.py` `VARIANTS`). Seeds are derived from the cell name, so every variant sees the same seeds.

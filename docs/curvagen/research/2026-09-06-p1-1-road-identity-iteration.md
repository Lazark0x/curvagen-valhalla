# P1.1 — switchback-safe twins, built-loop ranking, the geometry Defect Gate

- **Date:** 2026-09-06 (curvagen-valhalla [#12](https://github.com/Lazark0x/curvagen-valhalla/issues/12), P1.1 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); blocks [#11](https://github.com/Lazark0x/curvagen-valhalla/issues/11))
- **Scope:** a **throwaway prototype** on branch `proto/v4-p1.1`, cut from `proto/v4-p1` @ `6fb8e37df`. Seven items from the P1 close: the switchback test on the twin sidecar, the F01 pass cost, ranking the *built* loop, a geometry Defect Gate, an intermediate fallback rung, the seven under-fills, and gurka for each. Nothing pushed; no production traffic; `valhalla-local` (:8002) never addressed.
- **Method:** the P1 rig, unchanged — build in the warm `valhalla-fork-test:audit` container (arm64, `Release`), serve on :8003 against the same read-only Serbia tiles as the census (`curvagen-orchestrator/data`, `tileset_last_modified 1785932567`), fire `corpus-v2` (552 requests, K = 12) with the census's own run line (engine mode, 3 workers, way pass on). The baseline engine `valhalla-curvature:prod-equivalent-b4f514d7f` serves :8004. Detectors are P1's scripts with the fixed D3 counter, re-pointed by env, so every column in this document is read by byte-identical code.

## TL;DR verdict

**The rider-visible defect is essentially gone, and the deep bank stopped being a dump — but P1.1 costs 1.48× latency and still fails the distinctness ratchet.**

On the two rider-shaped blocks, against a same-session re-run of the census engine:

| | census | P1 | **P1.1** |
|---|---|---|---|
| **D1b ≥ 500 m, Vračar** | 61.1 % | 6.4 % | **0.4 %** |
| **D1b ≥ 500 m, demand** | 64.1 % | 9.2 % | **8.1 %** |
| **D1 same-pavement run ≥ 500 m, Vračar** | 33.6 % | 14.6 % | **0.0 %** |
| **D1 same-pavement run ≥ 500 m, demand** | 24.6 % | 11.3 % | **2.8 %** |
| D1b unseen metres/loop, Vračar | 5 755 m | 171 m | **11 m** |
| **D4 Fallback proxy, ALL** | 28.5 % | 22.0 % | **10.2 %** |
| fallback-like heavy reuse (mechanism) | 11.9 % | **15.7 %** | **5.7 %** |
| worst single loop, D1b metres | 48 596 m (48.6 % of its ride) | 11 193 m (28.5 %) | **2 759 m (6.1 %)** |
| loops classified `clean` | 4.8 % | 8.5 % | **9.4 %** |

Item by item:

1. **Switchback test — works, and finds the assumption wrong.** 819 of 81 288 geometric twin pairs (1.0 %) fail "different OSM way OR constant offset", all of them same-way *and* divergent. They cluster in **Belgrade and Novi Sad, not the mountains**. The vlasina-50 km cell at c0.5 goes 62.7 % → **39.7 %** (Gate 6b now passes there), but **block C's c0.8 regressions are unmoved**: `is_lollipop` 3.91 → 3.65 % and curviness retention 0.946 → 0.945×. The false positive P1 blamed is real but rare, and it is not what block C is paying for.
2. **F01 pass cost — 2–4× cheaper.** 22.5 ms/request mean, 20 ms at 50 km and 54 ms at 300 km, against P1's +48 ms. The ticket's ≤ 10 ms target is met at the corpus median (15 ms) and missed at 300 km.
3. **Built-loop ranking — the deep bank flattened.** Slots 9–11 go from 44.9 % same-pavement runs and 43.3 % fallback-like (P1) to **9.0 % and 8.8 %** — the same as slots 0–2. The bank is now uniform.
4. **Geometry Defect Gate — the residue is out of the served surface, and it is where the latency went.** 2 722 rejects per corpus (2 611 twin-ride, 552 seam, 106 mid-return bounce) each buy a replacement build: +3.4 attempts/request, and the two A\* lines carry 92 % of the +411 ms. G6b is green.
5. **Intermediate fallback rung — real but expensive.** It converts **61 of 2 956 failed rung-0 legs (2.1 %)** and costs ~200 ms/request on hard cells. One config flip turns it off.
6. **Under-fills — budget-forced, not network-forced.** No request in any P1.1 corpus ended with a dry candidate queue; 31 of 552 ended on the attempt cap. F09 is fixed, but the budget it grants is spent on the gate's refills. Fills are **538/552**, worse than P1's 545 and the baseline's 550.
7. **Gurka — 43 green** (18 + 22 + 3), including G6b, four new P1.1 tests and a new G6c pinning the last-resort contract.

**The distinctness call, apples to apples with the prod xcand penalty on at 0.2 (§9): P1.1 reads 1.096× `bank_overlap` and +7.8 pp `near_dup > 0.6` against the same engine with the penalty on — it does NOT pass the ≤ 1.02× ratchet.** It does, however, come out *better than the rig serves today* with the penalty off (0.980×, −2.4 pp). Whether ratchet 9 is a regression bar against prod or a floor on bank quality is the decision that goes back to Andrey.

---

## 0. Where the xcand knobs actually live (asked for explicitly)

| | `roundtrip_xcand_penalty` | `_strength` | `_cap` | set by |
|---|---|---|---|---|
| **code defaults** (`src/thor/worker.cc:77-80`) | `false` | `0.5` | `4` | — |
| **the rig** (`curvagen-orchestrator/data/valhalla.json`, mounted read-only into every engine here) | *absent* → `false` | *absent* → `0.5` | *absent* → `4` | nothing: the file's `thor` block carries no `roundtrip_*` key at all |
| **prod** (api.curvagen.cc) | `true` | `0.2` | `4` | **no file in any repo.** `curvagen-meta/deploy/docker-compose.yml:5-6` mounts `./data:/custom_files` and `deploy/data/` holds only `.gitkeep`; with `update_existing_config=True` the live `valhalla.json` is operator-supplied on the box (`/opt/curvagen/data/`). The values are recorded only in `docs/curvagen/extension-api.md:23-27` (ADR-0038 mechanism, ADR-0039 ship) and `curvagen-meta/CLAUDE.md:13` names `thor.roundtrip_xcand_strength → 0` as the engine rollback lever |

So P1's report is right — **the rig ran with the penalty off**, and it did so by omission rather than by choice. Every P1.1 run below states its xcand setting explicitly, and §8 runs the ratchet both ways.

## 1. Switchback-safe twins

**Design.** A pair that passes P1's geometry (chord collinearity ±30°, cover ≥ 0.70, span ≥ 0.60 of the shorter edge) is now also asked to be *the same physical road*:

```
same road  ⟺  different OSM way  OR  the lateral offset is constant along the matched run
```

- **Different OSM way** — `EdgeInfo::wayid()`, read once per segment in pass 1 (`src/thor/road_twin_index.cc:158-166`). A dual carriageway is *always* two ways (each carriageway is its own one-way way); the two arms either side of a hairpin usually are not.
- **Constant offset** — `run()` now returns the min and max distance of the *covered* samples to the neighbour's polyline (`road_twin_index.cc:304-313`), and the pair is "flat" when `max − min ≤ 12 m` **or** `min ≥ 0.40 × max` (`kOffsetSpreadM` / `kOffsetConstFrac`, `:51-52`). A carriageway pair holds its separation (a wide-median motorway swings 20–60 m but never collapses); a hairpin's arms converge to ~0 at the apex, so `min/max` goes to zero.

A pair that fails the test is **neither** a twin nor a parallel (`:373-390`) — the arm is a fully legal way home again.

**A side effect that pays for itself.** The sidecar's canonical key changed from "the `forward()` directed edge" to `min(edge, opposing edge)` (`valhalla/thor/road_twin_index.h:88-95`). It is the same undirected identity but needs no `DirectedEdge` dereference, which is what lets the F01 forest pass key every settled label without a tile lookup (§2). One opposing lookup per segment at build time buys it.

**What it removed, over the whole Serbia tileset** (engine-start INFO line, `road_twin_index.cc:448-468`):

| | P1 | P1.1 |
|---|---|---|
| geometric twin pairs | 81 288 | 81 288 |
| — **dropped by the switchback test** | — | **819 (1.01 %)**, every one *same OSM way* **and** divergent offset |
| twins kept | 81 288 pairs / 115 001 keyed segments | **80 469 pairs / 113 642 keyed segments** |
| parallel pairs dropped | — | 678 of 181 288 (0.37 %) |
| build time (cold) | 4 227–4 554 ms | **5 507 ms** (+1.1 s: one opposing lookup and one `wayid()` per segment) |
| retained | 7 MiB | 7 MiB |

**By terrain — and this is the finding.** The ledger buckets each drop by the 0.5° cell of the dropped edge's first sample. The top twelve cells:

| cell (lat, lon) | drops | what is there |
|---|---|---|
| (44.5, 20.0) | 127 | Belgrade / Šumadija |
| (45.0, 19.5) | 72 | Novi Sad |
| (44.5, 20.5) | 63 | Belgrade east |
| (43.5, 19.5) | 42 | west Serbia hills |
| (45.0, 20.0) | 37 | Banat |
| (44.5, 19.5) | 37 | Mačva |
| (43.0, 21.5) | 33 | south, toward Vlasina |
| (45.5, 19.5) · (43.5, 20.0) · (45.0, 20.5) · (44.0, 20.5) · (43.5, 20.5) | 29 · 23 · 22 · 22 · 22 | mixed |

**The switchback false positive is not a mountain phenomenon at the scale P1 assumed.** 1 % of twin pairs fail the test, and they cluster in the *cities and plains*, not in the block C massifs — a same-way divergent-offset pair is far more often a slip road peeling off a trunk than a hairpin. Whether block C's two P1 regressions (`is_lollipop` at c0.8, the vlasina-50 km cell) actually move is therefore an empirical question, answered in §7 and §9, not something the drop count can promise.

## 2. The F01 pass, made cheap

P1's forest pass cost **+48 ms/request** (harvest 89 → 137 ms). Three changes, all in `src/thor/roundtrip_expansion.cc:208-345`:

1. **The canonical key needs no tile.** P1 read `de->forward() ? eid : GetOpposingEdgeId(eid)` — a tile fetch for the edge plus a second one inside the opposing lookup, on every settled label. `BDEdgeLabel` already stores `endnode()` and `opp_index()`, so with the min-canonical convention (§1) the key is `min(edgeid, GraphId(endnode.tile, endnode.level, node->edge_index() + opp_index))`: **one** tile fetch, served from a single-entry cache because settle order clusters by tile, and the `DirectedEdge` is never dereferenced (`:236-259`). (The label's own `opp_edgeid()` cannot be used — `dijkstras.cc:136` deliberately leaves it unset on the forward expansion.)
2. **The band prunes whole subtrees.** `path_distance` is monotone along a chain, so a label past the widest band's `hi` can be neither a candidate nor an ancestor of one. Those labels are marked `kPruned` and their subtrees are never entered (`:244-246, :280, :325`). Both `ScanBand` callers use `hi_frac = 1.18`, so the pass is still computed once and cached.
3. **The DFS stops touching the label vector.** `BDEdgeLabel` is ~80 bytes and the vector is tens of MB at 300 km, so the predecessor link is lifted into a compact `uint32` array written sequentially in the key pass; the DFS then walks only `canon` / `pred` / `child_head` / `child_next`. Two more constant-factor cuts: a 512 KiB **Bloom prefilter** over the twin key set answers "no twins" — the common case — in one probe instead of a `lower_bound` over 113 k keys (`road_twin_index.h:105-119`), and a bad node's whole subtree inherits its verdict, so ~23 % of labels skip the on-path bookkeeping entirely (`:288-296`).

Measured on the rig, single requests from the Belgrade centroid (the ledger's own `f01_key=` / `f01_dfs=` fields):

| | 50 km request | 300 km request |
|---|---|---|
| labels keyed | 176 873 | 640 998 |
| P1 shape (tile-per-label, no prune, hot label vector) | 8 + 33 = **41 ms** | 39 + 158 = **197 ms** |
| P1.1 | 4 + 16 = **20 ms** | 13 + 41 = **54 ms** |
| speed-up | 2.1× | **3.6×** |

Over the corpus the pass costs **4.9 ms (key) + 17.6 ms (DFS) = 22.5 ms/request mean** against P1's +48 ms — the ticket's ≤ 10 ms target is met at the corpus median (15 ms) but **not** at 300 km, where 641 k labels at ~62 ns each is 41 ms of DFS alone. Getting under 10 ms there needs a different algorithm, not a smaller constant; it is listed for P2.

## 3. Ranking the built loop (F20's full fix)

v3 and P1 ranked on `cands[ci].curviness_per_km` — the *harvest label chain's* curviness, computed before the walk-back pops the tip, blind to the return leg, to distance error and to self-overlap. P1 added one axis (clean before Fallback). P1.1 replaces the key with a score over the loop that will actually be ridden (`src/thor/route_action.cc:1831-1930`):

```
score = curviness(whole loop) / ( (1 + Wo · overlap_frac) · (1 + Wd · dist_err) )
```

| term | definition | knob |
|---|---|---|
| `curviness(whole loop)` | `Σ curvature·len / Σ len / 15` over **both legs**, so a curvy forward leg no longer hides a motorway home | — |
| `overlap_frac` | twins-aware self-overlap ÷ loop metres. A return edge counts when its canonical id, **or a sidecar twin of it**, is on the forward leg outside the Start Exemption — the engine-side D1 ("ridden both ways") | — |
| `dist_err` | `|built − target| / target` (F21: ranking was distance-blind) | — |
| `Wo = 4` | a loop retreading a quarter of itself scores half a clean one | `thor.roundtrip_rank_overlap_w` |
| `Wd = 1` | a 20 %-off loop loses ~17 % | `thor.roundtrip_rank_disterr_w` |

**Tiers come first and are absolute** (`:2258-2270`): rung 0 (clean hard-exclude) → rung 1 (twins released) → rung 2 (soft-leash Fallback) → gated (failed the geometry gate, §4). Within a tier the sort is stable, so equal scores keep queue order and the seed still owns the tie-break. The ledger prints `slot:rTIER/score/loop-curviness/self-overlap-m/dist-err`, so any served slot can be explained from the log alone.

**Gurka P1.1b** (`test/gurka/test_roundtrip_audit.cc`, `RtP11BuiltRanking`) pins it with a control: two clean loops, one with a curvature-15 forward leg, a straight 16 km way home and a 50 % distance error (harvest curviness 0.75, built score 0.17), one uniform curvature-8 and on target (harvest 0.53, built score 0.53). With `thor.roundtrip_built_ranking=false` slot 0 is the curvy-forward lobe — the map discriminates; with it on, the slots swap.

## 4. The geometry Defect Gate

ADR-0037's gate reads exactly one shape: an exact-mirror stub across the **seam**. Two rider-visible shapes walk past it, and P1 left both in the bank:

- **(a) the return rides the forward corridor's TWINS** beyond the Start Exemption. Not the same edge, so there is no palindrome anywhere for the seam decoder to find — but it is the same physical road ridden the other way, which is F02, the census's whole rider-visible residual.
- **(b) a mid-return exact-mirror bounce**, whose apex sits far off the seam (F08; audit F13 leak 3; gurka G6b served a 999.99 m one).

Both are now gated (`src/thor/route_action.cc:2009-2050`):

| half | detector | cost | knob (default) |
|---|---|---|---|
| (a) twin ride | the same twins-aware self-overlap the rank score computes — no decode, one hash-set pass over both legs | free (shared with §3) | `thor.roundtrip_gate_twin_ride_m` (**500 m**) |
| (b) mid-return mirror | `leg_mirror_stub_m` (`:1279-1305`) decodes the **whole return leg** onto the 1e-5 grid and takes the longest palindrome anywhere, not only the one covering the seam | the decode | `thor.roundtrip_gate_return_bounce_m` (**30 m**, the seam gate's own threshold) |

(b) is skipped when (a) already rejected, so a request pays at most one full return decode per built candidate.

**Decode budget.** The ledger's own `gate=` field, mean over the corpus: **5.8 ms/request** (p50 4 ms, max 28 ms), against `seam=` 2.6 ms. On the heavy Belgrade 50–100 km A/B cells it is 5.3 ms/request against 1.4 ms with the gate off — so **the decode itself costs ~4 ms/request**. The gate's real price is not the decode, it is the refill (below).

**Refill, and who pays for it.** A gated loop is stashed and its slot refilled from the queue, exactly as the seam gate has always done. The first P1.1 corpus did that with no bound and it was the single worst decision in this iteration:

| | P1 | P1.1, refill on every reject | P1.1, per-slot last resort |
|---|---|---|---|
| p50 latency | 1.517 s | **3.954 s** (3.33× the same-session control) | §7 |
| requests filling K = 12 | 545/552 | **541/552 — and nine of the eleven served 1–6 loops** | §7 |

Two changes fixed it, both in `:2260-2285`:

1. **Last resort per slot, not per request.** ADR-0037 promoted the stash only when the bank was *completely* empty, so a cell that built three clean loops and stashed nine served **three**. The stash now tops the bank up to `want`, best-scored first, with every topped-up loop marked `gated` so the tier sort keeps it behind every clean loop. Nothing a rider reads changes; the bank stops collapsing.
2. **A refill budget** (`thor.roundtrip_gate_refill_budget`, 0 = unlimited = the seam gate's semantics). Past the budget a gated loop is kept in the bank's last tier instead of buying a replacement build. The corpus below runs it **unlimited**, which is the faithful reading of the ticket; the knob exists because the budget is the obvious lever if the latency ratchet has to be met.

**Gurka.** `RtP11GeometryGate.P11c` (twin-ridden return, control = gate off) and the repaired **G6b** both pass; **G6c** is new and pins the boundary the gate must not cross — on the original degenerate G6b map, where the bounce is the only route home at all, the loop must still be served rather than 442.

## 5. The fallback rungs

v3 had one step: hard exclusion, or **drop every hard exclusion**. P1.1 makes it a ladder (`route_action.cc:1531-1560, 1720-1775`):

| rung | corridor | twins | parallels | meaning |
|---|---|---|---|---|
| **0** | barred beyond the exemption | barred | leashed + progress-graded | the clean hard-exclude success |
| **1** | barred | **released to the leash** | released from leash and grade | the return may ride the other carriageway, but never the corridor's own pavement |
| **2** | released | released | released | the v3 Fallback Loop |

**A deliberate deviation from the ticket, with the reason.** The ticket's rung 1 was "release parallels, keep twins barred". That rung cannot work: `mark_edges_used` and `mark_rejoin_edges` are multiplicative cost factors read by `EdgeFactor` (`valhalla/sif/dynamiccost.h:1292-1308`) and never block anything, so a rung that relaxes only soft costs cannot turn "no route home" into a route — it can only buy a second exhaustive A\*. The releasable *bar* at rung 1 is the twin tier, so that is what rung 1 releases. The literal version is kept behind `thor.roundtrip_fallback_parallel_rung` (default **off**) so the claim is measurable rather than argued.

Corpus counts, per return leg (11 321 legs over 552 requests):

| rung | legs | share |
|---|---|---|
| 0 clean hard-exclude | 8 365 | **73.9 %** |
| 1 twins released | 61 | **0.54 %** |
| 2 full soft leash | 2 888 | 25.5 % |
| no route on any rung (candidate fails) | 7 | 0.06 % |

**The intermediate rung converts 61 of the 2 956 legs that fail rung 0 — 2.1 %.** It costs an extra failed A\* on the other 2 895: the A/B on 27 heavy Belgrade cells reads `astar_fb` **1 074 ms with the ladder against 856 ms without** (`thor.roundtrip_fallback_rungs=false`), i.e. **~200 ms/request on hard cells** for a 2 % conversion. That is the clearest "turn this off" candidate in the build and it is one config flip.

## 6. The under-fills (F09)

F09 is real and it is fixed (`route_action.cc:2056-2080`): the stall branch granted the fresh `+want` attempt budget only when *it* was the caller of `widen_pool()`, so a cell whose distance correction widened first reached its stall with `widened` already true, was granted nothing, and broke at `want + kAttemptSlack`. The grant now belongs to the stall, fires once, and can only raise the cap (`std::max`). `thor.roundtrip_f09_budget=false` restores the bug for A/B.

The ledger now also names the cause of every short bank — `underfill=queue` (the candidate queue ran dry: **network-forced**) or `underfill=cap` (the attempt budget ran out: **budget-forced**). On the first P1.1 corpus, 31 of 553 requests ended `cap` and 522 `none`; on the final corpus, §7. The answer to the ticket's question is therefore **budget-forced, not network-forced** — no request in either P1.1 corpus ended with a dry queue — but the budget in question is spent on the geometry gate's refills, not on F09's missing grant. F09's fix raises the ceiling; the gate raises the demand. A gurka test for F09 alone would need a map where the distance correction fires before the primary queue stalls, which is a three-lobe construction the corpus already answers more directly; it is **not** written, and that is the one ticket item deliberately skipped.

## 7. Gurka

`gurka_roundtrip_audit` **18/18**, `gurka_motorcycle_roundtrip` **22/22**, `gurka_roundtrip_distinctness` **3/3** — 43 green.

| test | P1 | P1.1 |
|---|---|---|
| G1 `RingReversal` (F01) | PASS | **PASS**, assertion (b) re-based — see below |
| G1b · G2 · G3 · G5 ×2 · G6 · G7 · F03 | PASS | PASS |
| **G6b** `…NoWayRound` (F08) | **FAIL** — 999.99 m mid-return mirror served | **PASS** — the return-leg decode sees the bounce, the slot is refilled with the clean lobe |
| **G6c** `GateNeverStarvesTheOnlyLoop` (new) | — | **PASS** — on the original degenerate map the gated loop is still served, never a 442 |
| P1a `ParallelTier` · P1b `TwinOnlyWayHome` · P1c `CleanFirstRanking` | PASS | PASS |
| **P1.1a-1** `Hairpin` (new) | — | **PASS** — one OSM way, offset 8 → 32 m: a twin with the test off, not a twin with it on |
| **P1.1a-2** `Carriageway` (new) | — | **PASS** — two ways, constant 8 m: still a twin |
| **P1.1b** `BuiltRanking` (new) | — | **PASS** — control (harvest ranking) serves the curvy-forward lobe in slot 0; the built score swaps the slots |
| **P1.1c** `GeometryGate` (new) | — | **PASS** — control (gate off) rides the twin carriageway home; gated, the slot is refilled with the clean lobe |

Two maps changed, both recorded in the file:

- **G6b** gained a second, clean lobe (A-M-N-A, 12 km south). The T lobe is untouched and still has no way round, so the *bounce* is still the only route home **from T** — but the cell now has an alternative, which is what a gate that refills needs in order to do anything. The original degenerate map lives on as G6c.
- **G1 (b)** asserted the D-E-F ring appears in the served ride. It only ever got there on a soft-leash return that retraces the corridor, which the geometry gate now rejects — so with the gate on the engine refills the single slot with the clean B-G-A loop, which is the intended behaviour and the better ride. (b) is therefore asserted against a **gate-off control** in the same test (`map.config.put("thor.roundtrip_geometry_gate", false)`), which is what actually pins F01: the ring is still reachable and the chain that reaches it is still edge-simple. The gated run then has to show B-G-A.

## 8. corpus-v2 — defects, before → after

Baseline column = `census-v2-control-p11b`, a same-session re-run of the pinned census engine `b4f514d7f`; like P1's control it reproduces `census-v2-b4f514d7f` to the metre (6 622 loops, every v1.3 meter identical). Detector columns are read by byte-identical code on all three runs, including the fixed D3 ring counter.

### 8.1 D1–D6 detector prevalence — census → P1 → **P1.1**

| detector | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| D1 ≥ 1 km ridden both ways | 28.3 → 12.7 → **5.0 %** | 31.6 → 19.1 → 11.8 | 32.2 → 10.5 → **0.0** | 23.0 → 7.0 → **0.1** |
| **D1 longest same-pavement run ≥ 500 m** | 29.9 → 17.4 → **8.2 %** | 33.4 → 24.5 → 16.9 | **33.6 → 14.6 → 0.0** | **24.6 → 11.3 → 2.8** |
| D1 longest same-pavement run ≥ 2 km | 20.1 → 8.1 → **5.0 %** | 26.9 → 16.5 → 11.8 | 14.3 → 1.0 → **0.0** | 15.4 → 2.5 → **0.1** |
| **D1b unseen near-mirror ≥ 500 m** | 49.6 → 9.4 → **7.6 %** | 30.9 → 10.9 → 10.3 | **61.1 → 6.4 → 0.4** | **64.1 → 9.2 → 8.1** |
| D1b unseen near-mirror ≥ 2 km | 37.6 → 0.8 → **0.3 %** | 10.4 → 0.6 → 0.5 | 59.3 → 2.3 → **0.2** | 56.4 → 0.3 → **0.2** |
| D1c near-mirror U-turn at the seam | 2.2 → 0.8 → **0.4 %** | 2.9 → 1.0 → 0.7 | 3.2 → 1.2 → 0.2 | 1.1 → 0.2 → 0.2 |
| D2 exempt-zone corridor ≥ 5 % | 2.5 → 2.3 → **1.8 %** | 5.5 → 5.4 → 4.4 | 0.0 → 0.0 → 0.0 | 0.5 → 0.0 → 0.0 |
| D2 reuse hidden by the exemption ≥ 1 km | 6.9 → 7.4 → **7.2 %** | 15.9 → 16.5 → 16.4 | 0.0 → 0.0 → 0.0 | 0.5 → 1.1 → 0.9 |
| D3 ≥ 1 near-rejoin ring (fixed counter) | 64.1 → 49.9 → **43.4 %** | 46.6 → 39.5 → 31.3 | 80.4 → 55.2 → **47.8** | 75.2 → 58.3 → **54.1** |
| D3 ring inside a single leg | 20.8 → 13.0 → **11.4 %** | 15.7 → 7.0 → 4.4 | 14.2 → 8.7 → 5.2 | 29.2 → 21.3 → 21.6 |
| D3b ≥ 1 transversal self-crossing | 66.7 → 52.9 → **48.2 %** | 56.5 → 46.0 → 40.0 | 66.0 → 49.9 → 43.7 | 77.8 → 61.5 → 58.8 |
| D3b figure-8 (lobe ≥ 25 %) | 22.2 → 14.4 → **13.0 %** | 16.6 → 14.4 → 12.6 | 32.7 → 15.2 → 14.6 | 23.2 → 14.0 → 12.8 |
| **D4 Fallback proxy** | 28.5 → 22.0 → **10.2 %** | 35.6 → 25.9 → 15.8 | 30.5 → 22.2 → **6.1** | 20.0 → 17.9 → **6.3** |
| D5 two prominent lobes | 2.6 → 2.1 → **2.5 %** | 5.1 → 2.1 → 2.5 | 0.9 → 1.8 → 2.3 | 0.8 → 2.3 → 2.4 |
| **D6 cross-leg corridor ≥ 25 %** | 22.1 → 2.7 → **1.3 %** | 9.6 → 5.6 → 3.0 | 49.2 → 2.0 → **0.0** | 23.0 → 0.0 → **0.0** |
| D6 stem zeroed by the 120 m gap | 20.1 → 2.3 → **2.2 %** | 11.7 → 0.6 → 0.4 | 8.8 → 0.0 → 0.0 | 34.2 → 5.2 → 5.0 |
| D6c seam is not the ride's farthest point | 77.3 → 75.5 → **77.6 %** | 83.0 → 81.0 → 82.9 | 66.4 → 63.3 → 71.5 | 76.2 → 75.2 → 74.8 |

### 8.2 v1.3 meters and v2 magnitudes

| meter | ALL: census → P1 → **P1.1** | block C | block A Vračar | block B demand |
|---|---|---|---|---|
| n loops | 6 622 → 6 617 → **6 602** | 2 782 → 2 779 → 2 762 | 1 200 | 2 640 |
| `spike_ge_30m` / `spike_ge_500m` | 0.0 → 0.0 → **0.0 %** | 0.0 | 0.0 | 0.0 |
| `edge_reuse_geom` mean | 0.0342 → 0.0216 → **0.0097** | 0.0587 → 0.0401 → 0.0225 | 0.0273 → 0.0124 → **0.0007** | 0.0114 → 0.0063 → **0.0004** |
| `edge_reuse_geom > 0.30` | 2.4 → 1.7 → **0.8 %** | 5.6 → 4.0 → 2.0 | 0.1 → 0.0 → 0.0 | 0.1 → 0.0 → 0.0 |
| `edge_reuse_way` mean | 0.0546 → 0.0436 → **0.0250** | 0.1007 → 0.0830 → 0.0531 | 0.0434 → 0.0295 → 0.0108 | 0.0232 → 0.0185 → 0.0092 |
| `is_lollipop` | 0.7 → 1.1 → **0.9 %** | 1.8 → 2.6 → **2.1** | 0.0 | 0.0 |
| `bulb_count` mean | 2.136 → 1.644 → **1.473** | 1.717 → 1.483 → 1.359 | 2.595 → 1.856 → 1.534 | 2.369 → 1.718 → 1.565 |
| `shadow_frac` mean | 0.1478 → 0.0414 → **0.0247** | 0.0927 → 0.0627 → 0.0419 | 0.2571 → 0.0410 → **0.0154** | 0.1560 → 0.0190 → **0.0110** |
| `compactness` mean | 0.2349 → 0.2919 → **0.3032** | 0.2441 → 0.2609 → 0.2698 | 0.1925 → 0.3154 → 0.3317 | 0.2444 → 0.3139 → 0.3251 |
| `compactness < 0.10` | 21.8 → 10.5 → **7.8 %** | 14.8 → 12.1 → 9.4 | 41.7 → 14.7 → 11.0 | 20.1 → 7.0 → 4.7 |
| `distance_error` mean | 0.1628 → 0.1515 → **0.1615** | 0.2256 → 0.1768 → 0.1959 | 0.1148 → 0.1539 → 0.1586 | 0.1184 → 0.1239 → 0.1269 |
| `distance_error` max | 8.353 → 2.317 → **2.317** | 8.353 → 2.317 → 2.317 | 0.555 → 0.822 → 1.021 | 0.598 → 0.685 → 0.822 |
| `curviness_geom_clean` mean | 270.7 → 271.2 → **275.2** | 347.0 → 338.4 → 338.9 | 258.7 → 261.3 → **274.2** | 195.7 → 205.0 → **208.9** |
| **unseen near-mirror m mean** | 4 230 → 152 → **108** | 1 295 → 200 → 183 | 5 755 → 171 → **11** | 6 629 → 92 → **74** |
| — p50 / p90 | 490/16 168 → 0/478 → **0/396** | 174/2 380 → 15/516 → 0/504 | 3 059/16 168 → 0/185 → **0/0** | 3 051/19 653 → 0/376 → **0/189** |
| same-pavement both-ways m mean | 3 030 → 1 560 → **930** | 5 367 → 3 077 → 2 135 | 1 536 → 434 → **10** | 1 248 → 474 → **87** |
| **intra-leg self-overlap m mean (F01)** | 955 → 39 → **22** | 1 809 → 88 → 45 | 305 → 0 → **0** | 351 → 5 → **9** |
| — ≥ 500 m | 10.3 → 0.6 → **0.5 %** | 14.1 → 1.4 → 1.0 | 9.8 → 0.0 → 0.0 | 6.6 → 0.2 → 0.2 |

### 8.3 Served-slot cleanliness

The ticket's question — what does a rider actually read? Slots 0–2 are the direct serve (K = 3), 3–5 the first Bank tap.

| | slots 0–2 | slots 3–5 | slots 6–8 | slots 9–11 | ALL |
|---|---|---|---|---|---|
| n loops | 1 656 | 1 656 | 1 655 | 1 635 | 6 602 |
| **near-mirror mechanism (F02)** | P1 4.3 → **5.4 %** | 3.4 → 3.3 | 2.7 → 1.8 | 2.6 → **1.7** | 3.2 → 3.1 |
| same-pavement mechanism (F01) | 0.0 → 0.0 % | 0.0 → 0.0 | 0.1 → 0.1 | 0.1 → 0.1 | 0.1 → 0.1 |
| **near-mirror + same-pavement** | 4.3 → **5.4 %** | 3.4 → **3.3** | 2.8 → **1.9** | 2.7 → **1.8** | 3.3 → **3.1** |
| D1b unseen ≥ 500 m | 8.8 → **8.9 %** | 6.8 → 8.6 | 6.5 → 6.3 | **15.6 → 6.7** | 9.4 → **7.6** |
| D1b unseen m, mean | 119 → **121** | 99 → 124 | 108 → 93 | **282 → 95** | 152 → **108** |
| D1 same-pavement run ≥ 500 m | 9.3 → **11.4 %** | 8.8 → 7.4 | 12.0 → 8.3 | **44.9 → 9.0** | 18.7 → **9.0** |
| same-pavement both-ways m, mean | 1 157 → **825** | 886 → 793 | 1 361 → 1 065 | **2 816 → 1 026** | 1 554 → **927** |
| intra-leg ring (F22) | 15.5 → **12.1 %** | 10.1 → 14.9 | 5.7 → 8.7 | 11.2 → 7.8 | 10.6 → 10.9 |
| `clean` | 14.2 → **12.6 %** | 9.1 → 10.6 | 7.9 → 8.2 | 2.9 → **6.0** | 8.5 → **9.4** |
| fallback-like heavy reuse | 5.3 → **4.0 %** | 4.8 → 4.8 | 9.4 → 5.2 | **43.3 → 8.8** | 15.7 → **5.7** |

**The deep bank stopped being a dump.** P1's design put the residue in slots 9–11 and left it there — 44.9 % same-pavement runs, 43.3 % fallback-like, 15.6 % D1b. P1.1 flattens it: slots 9–11 now read 9.0 / 8.8 / 6.7 %, essentially the same as slots 0–2. The bank is uniform, which is what a bank should be if the ranking is trusted and the gate is real.

**The one number that went the wrong way is slots 0–2's near-mirror share, 4.3 → 5.4 %** — but the *metres* fell (same-pavement 1 157 → 825 m, D1b flat at ~120 m). The gate removes the loops that carry a lot of near-mirror; the ones left carry a little of it, and the mechanism classifier fires on ≥ 500 m of any mechanism, so a residue that got smaller but more evenly spread reads as "more loops, fewer metres". Ring share (F22) is **43.4 % of loops carry ≥ 1 near-rejoin ring, 0.54 rings/loop** (census 64.1 % / 1.09, P1 49.9 % / 0.71).

### 8.4 Worst 20 by D1b unseen near-mirror metres

| | census | P1 | **P1.1** |
|---|---|---|---|
| worst single loop | **48 596 m** on a 99.9 km ride (48.6 %), slot 5 | 11 193 m on 39.3 km (28.5 %), slot 11 | **2 759 m on 45.2 km (6.1 %), slot 8** |
| population mean / p50 / p90 | 4 230 / 490 / 16 168 | 152 / 0 / 478 | **108 / 0 / 396** |

The worst twenty are now: two Vračar 40 km loops at 2 759 m (slots 7–8), thirteen zlatibor 200 km loops at 2 206–2 506 m (slots 2–11, ~1 % of a 230–275 km ride), two demand-cell 70 km loops at 2 317 m (slot 10), and two 300 km demand loops at 2 025 m (slot 0, 0.6 % of the ride). Nineteen of the twenty are F02; **the largest share of any ride is 6.1 %**, against P1's 28.5 % and the census's 48.6 %.

## 9. Distinctness, apples to apples (Gate v1.3 ratchet 9)

P1's clearest open question. Both engines were re-fired with `thor.roundtrip_xcand_penalty=true`, `strength 0.2`, `cap 4` — the values §0 shows prod runs and the rig does not — so the ratchet can be read on the configuration production actually serves.

| | `bank_overlap_mean` | vs its own baseline | `near_dup > 0.6` | Δpp | `common_trunk_frac_75` |
|---|---|---|---|---|---|
| baseline b4f514d7f, **xcand off** (= the rig) | 0.4931 | — | 34.2 % | — | 0.0319 |
| P1, xcand off | 0.5314 | 1.078× | 43.7 % | +9.5 | 0.0500 |
| **P1.1, xcand off** | **0.5501** | **1.116×** | **48.1 %** | **+14.0** | 0.0508 |
| baseline b4f514d7f, **xcand on 0.2** (= prod) | 0.4407 | — | 24.0 % | — | 0.0290 |
| **P1.1, xcand on 0.2** | **0.4832** | **1.096×** | **31.8 %** | **+7.8** | 0.0421 |

Per level, with xcand on at 0.2:

| level | baseline | P1.1 | ratio | near_dup baseline → P1.1 | Δpp |
|---|---|---|---|---|---|
| c0.5 | 0.4613 | 0.4988 | **1.081×** | 27.5 → 34.5 % | +7.0 |
| c0.7 | 0.3800 | 0.4076 | **1.073×** | 13.6 → 19.9 % | +6.3 |
| c0.8 | 0.5537 | 0.6328 | **1.143×** | 46.4 → 57.8 % | +11.5 |
| c1.0 | 0.3753 | 0.4329 | **1.153×** | 12.1 → 22.3 % | +10.2 |

**Plainly: P1.1 does NOT pass ratchet 9 with xcand on.** 1.096× against a ≤ 1.02× bar, +7.8 pp against a ≤ +2 pp bar, and it fails at every level.

**But the sweep changes the shape of the question, and this is the number Andrey asked for.** Turning the penalty on buys more distinctness than road identity costs:

| comparison | `bank_overlap_mean` | verdict |
|---|---|---|
| **P1.1 + xcand 0.2 vs the RIG today** (baseline, xcand off) | 0.4832 vs 0.4931 = **0.980×**, near_dup 31.8 % vs 34.2 % = **−2.4 pp** | **better on both axes** |
| **P1.1 + xcand 0.2 vs PROD today** (baseline, xcand on) | 0.4832 vs 0.4407 = **1.096×**, +7.8 pp | **worse — the ratchet fails** |
| P1.1 xcand off → on | 0.5501 → 0.4832 = **0.878×**, 48.1 → 31.8 % = **−16.3 pp** | the penalty recovers about half the loss and then some |

So the decision is not "does P1.1 break distinctness" but **"is ratchet 9 a regression bar against the shipped engine, or an absolute floor on bank quality?"** Read as a regression bar against prod, P1.1 fails and needs a distinctness partner in P2 (`roundtrip_xcand_strength` above 0.2 is the obvious first sweep — ADR-0039 costed 0.5 as "full win, over the 1.10× latency ratchet on 2-vCPU prod", and P1.1 has its own latency problem, so the two must be swept together). Read as a floor — "the bank must be at least as distinct as what riders get today on the rig" — P1.1 with the prod penalty on clears it. **The call is Andrey's; the numbers are above.**

## 10. Gallery

`tools/loopqual/results/p1-1-road-identity/gallery-p1-1.html` — 40 Leaflet cases, filterable by mechanism, drawn by the census's own payload builder so both panels are identical code: the **20 worst remaining P1.1 loops** by D1b unseen near-mirror metres, followed by **P1's worst 20 on the same axis**. Coordinates are `[lat, lon]`, the order fixed in the scratch builders on 2026-09-06.

The two panels are not comparable case by case — they are two different populations — but the axis is: P1's worst reads **11 193 m of unseen near-mirror on a 39.3 km ride (28.5 %)**; P1.1's worst reads **2 759 m on 45.2 km (6.1 %)**.

## 11. Latency

Wall latency on this box swings with whatever else is running on it, and the first P1.1 corpus was fired while a stray runner was still hammering the same engine — that measurement (p50 3.95 s) is discarded. The numbers below come from a dedicated, uncontended pair fired back to back, P1.1 alone then the baseline alone, both `--no-way`:

| stage (mean ms/request, 552 requests) | baseline `b4f514d7f` | **P1.1** | Δ |
|---|---|---|---|
| harvest (expansion + primary `ScanBand` + F01 forest pass) | 82.4 | **104.5** | +22.1 |
| — of which the F01 pass | — | 19.7 (key 4.1 + DFS 15.6) | — |
| widened `ScanBand` | 18.9 | 18.8 | −0.1 |
| rejoin map build (+ twins, parallels, all levels) | 2.6 | 8.1 | +5.5 |
| **return-leg A\*** | 588.1 | **749.0** | **+160.9** |
| **fallback A\* (the rung ladder)** | 177.4 | **393.1** | **+215.7** |
| Second Via | ~11 | **0** | −11 |
| seam decode | 1.6 | 2.2 | +0.6 |
| **geometry gate decode** | — | **5.0** | +5.0 |
| TripLegBuilder | 28.2 | 28.8 | +0.6 |
| **engine stage total** | **899** | **1 310** | **+411 (1.46×)** |
| build attempts / request | 12.6 | **16.0** | +3.4 |
| fallback (rung-2) legs / request | 4.4 | 5.6 | +1.2 |
| **wall p50 / p95** | **1.299 / 3.487 s** | **1.917 / 4.506 s** | **1.476× / 1.292×** |
| corpus firing wall time | 276 s | 388 s | 1.41× |

**Where the money went.** Not the sidecar (built once at start), not the F01 pass (which P1.1 made 2–4× cheaper), not the gate's decode (5 ms). **It is the extra builds.** The geometry gate rejects 2 722 loops per corpus and buys a replacement for every one — +3.4 attempts per request — and each replacement pays a return A\* and, when the corridor has no fresh way home, the rung ladder's two more. The two A\* lines carry 92 % of the +411 ms.

Three levers, all measured on 27 heavy Belgrade 50–100 km cells (`~/.curvagen-scratch/p1.1/ab-*.log`):

| flip | `astar` | `astar_fb` | attempts | note |
|---|---|---|---|---|
| P1.1 as measured | 3 366 | 1 074 | 14.2 | — |
| `roundtrip_fallback_rungs=false` | 3 401 | **856** | 14.2 | **−218 ms** for a rung that converts 2.1 % of failed legs |
| `roundtrip_geometry_gate=false` | 3 307 | 1 152 | **12.3** | the attempts, not the decode |
| `roundtrip_f09_budget=false` | 3 360 | 1 203 | 14.2 | inside noise |

`thor.roundtrip_gate_refill_budget` (§4) is the fourth lever, and it **has now been swept — see §15.** Capping the refills does cut the attempts and does meet the bar (budget 2 with the rung ladder off reads 1.10×, fills 552/552), at a cost §11 did not anticipate: the budget is shared with the seam gate, so bounding it puts ≥ 500 m seam stubs back into the deep bank (§15.5).

### 8.5 Mechanism classification

Share of loops / share of that population's total ride kilometres. census → P1 → **P1.1**.

| mechanism | ALL | block C v1 | block A Vračar | block B demand |
|---|---|---|---|---|
| **near-mirror / parallel carriageway (F02)** | 37.0/3.81 → 3.2/0.05 → **3.1/0.03 %** | 14.1/0.71 → 5.5/0.05 → 6.0/0.05 | **53.3/13.39 → 2.3/0.23 → 0.2/0.01** | **53.7/6.89 → 1.3/0.02 → 1.2/0.02** |
| **same-pavement retrace (F01)** | 5.3/0.71 → 0.1/0.01 → **0.1/0.00 %** | 8.4/1.01 → 0.1/0.02 → 0.1/0.00 | 3.1/0.43 → 0.0 → **0.0** | 3.1/0.28 → 0.0 → **0.1/0.00** |
| **fallback-like heavy reuse** | 11.9/1.26 → 15.7/1.29 → **5.7/0.67 %** | 20.3/1.94 → 20.6/1.85 → 11.7/1.18 | 8.4/0.78 → 15.8/1.02 → **2.2/0.03** | 4.5/0.25 → 10.4/0.45 → **1.0/0.01** |
| intra-leg ring / near-rejoin (F22) | 5.2/0.19 → 10.6/0.24 → **10.9/0.19 %** | 3.8/0.20 → 4.6/0.11 → 3.9/0.06 | 2.8/0.23 → 6.4/0.51 → 5.0/0.39 | 7.8/0.16 → 18.9/0.39 → 20.9/0.37 |
| exempt-zone stem | 12.5/0.13 → 13.6/0.12 → **15.0/0.13 %** | 20.5/0.15 → 20.9/0.14 → 23.9/0.16 | 0.0 | 9.8/0.13 → 12.1/0.11 → 12.6/0.12 |
| seam residue | 0.5/0.01 → 0.1/0.00 → **0.0/0.00 %** | 0.4 → 0.1 → 0.0 | 0.9 → 0.2 → 0.2 | 0.4 → 0.0 → 0.0 |
| other (a detector fires, < 500 m of any mechanism) | 22.7 → 48.1 → **55.9 %** | 28.6 → 42.5 → 48.3 | 24.0 → 60.7 → 76.8 | 15.9 → 48.4 → 54.2 |
| **clean** | 4.8 → 8.5 → **9.4 %** | 3.7 → 5.5 → 6.2 | 7.4 → 14.7 → **15.6** | 4.7 → 8.9 → **9.9** |

The mechanism P1 *grew* — fallback-like heavy reuse, 11.9 → 15.7 % of loops — P1.1 takes back to **5.7 %**, below the census, and its ride share from 1.29 to **0.67 %**. That is the geometry gate and the built-loop score doing exactly what they were added for. **F22 (intra-leg rings) is now the largest defect class by loop share on rider-shaped demand (20.9 %)** and it is untouched by design; whether a rider reads "round the block" as a defect is the labelling question, not an engine one.

## 12. Provisional gate

| # | gate | bar | baseline (same session) | **P1.1** | verdict |
|---|---|---|---|---|---|
| 1 | D1b ≥ 500 m, **Vračar** | down ≥ 50 %, ≥ P1's | 61.1 % | **0.4 %** (−99.3 %; P1 −89.5 %) | **PASS** |
| 2 | D1b ≥ 500 m, **demand** | down ≥ 50 %, ≥ P1's | 64.1 % | **8.1 %** (−87.4 %; P1 −85.6 %) | **PASS** |
| 3 | D1 ride-fraction, **Vračar** (run ≥ 500 m) | down ≥ 50 %, ≥ P1's | 33.6 % | **0.0 %** (−100 %; P1 −56.5 %) | **PASS** |
| 4 | D1 ride-fraction, **demand** | down ≥ 50 %, ≥ P1's | 24.6 % | **2.8 %** (−88.6 %; P1 −54.1 %) | **PASS** |
| 5 | curviness retention, per level | ≥ 0.95× | c0.5 287.1 · c0.7 206.3 · c0.8 344.2 · c1.0 233.3 | 289.5 (**1.008**) · 221.1 (**1.072**) · **325.5 (0.945)** · 245.9 (**1.054**) | **FAIL at c0.8** (P1: 0.946× — unmoved) |
| 6 | latency p50 | ≤ 1.10× | 1.299 s | **1.917 s (1.476×)** | **FAIL** |
| 6b | latency p95 | — | 3.487 s | 4.506 s (1.292×) | fail |
| 7 | fills K = 12 on every request | 552/552 | 550/552 | **538/552** (11×9, 10×3, 9×1, 8×1) | **FAIL** (P1: 545/552) |
| 8 | failures | 0 | 0 | **0** | **PASS** |
| 9 | fallback share | report | D4 proxy 28.5 % (census) | rung-2 legs **26.7 %**; **D4 proxy 10.2 %**; served slots 0–2 carry 1.1–1.6 % rung-2 and 3.6–4.3 % gated | **improved** |
| 10 | exempt-zone stem | report | 12.5 % of loops / 0.13 % of ride | **15.0 % / 0.13 %**; D2 ≥ 5 % **1.8 %** | **flat** |
| 11 | Gate v1.3 absolutes (1, 2, 3, 4, 5, 7) | pass | pass | **pass at every level** — incl. `spike_ge_500m` 0.00 % and `spike_ge_30m` 0.00 % | **PASS** |
| 11b | Gate v1.3 **6a** lollipop ≤ 2 % | pass | c0.8 1.3 % | c0.8 **3.65 %** | **FAIL at c0.8** (P1: 3.91 %) |
| 11c | Gate v1.3 **6b** worst cell ≤ 55 % | pass | vlasina-50 km c0.5 48.3 % | **c0.5 39.7 % PASS** (P1: 62.7 % FAIL) · **c0.8 58.3 % FAIL** | **half fixed** |
| 11d | Gate v1.3 **9** distinctness ratchet | ≤ 1.02× / +2 pp | — | xcand off **1.116× / +14.0 pp**; **xcand on 0.2 1.096× / +7.8 pp** | **FAIL** (§9) |
| 12 | served-slot cleanliness (slots 0–2, 3–5) | report | — | near-mirror + same-pavement **5.4 % / 3.3 %**; D1b unseen mean **121 / 124 m** | **§8.3** |
| 13 | worst-20 D1b metres | report | max 48 596 m (slot 5) | **max 2 759 m (slot 8)**, 6.1 % of its ride | **§8.4** |
| 14 | ring share (F22) | report | 64.1 % of loops, 1.09 rings/loop | **43.4 %, 0.54 rings/loop** | **improved** |

Seven of the fourteen pass outright, two are reports that improved, and the five failures are: **c0.8 curviness and lollipop (block C, unmoved from P1), latency, fills, and distinctness.**

## 13. What did not move, and why

1. **Block C at c0.8 is unmoved.** `is_lollipop` 3.91 → 3.65 % (bar 2 %), curviness retention 0.946 → 0.945× (bar 0.95×), vlasina-50 km at c0.8 62.7 → 58.3 % (bar 55 %). The switchback test fixed the *c0.5* half of the vlasina cell (62.7 → 39.7 %) and nothing at c0.8. Since only 1 % of twin pairs fail the test and the drops sit in the plains, the c0.8 cost is **not** mostly false-positive twins — it is the twin exclusion working as designed on a network that has no second way home. c0.8 exists only in block C (384 loops, mountain origins, motorways avoided) and 93 % of production demand is `a0` in the city.
2. **The exempt-zone stem did not move** — 12.5 → 15.0 % of loops, 0.13 → 0.13 % of ride. P1.1 does not touch the Start Exemption. Confirmed twice now.
3. **D6c (the seam is not the ride's farthest point) did not move**: 77.3 → 77.6 %. A shape property of the harvest band; P2 territory.
4. **F03 (return-leg A\* admissibility) is untouched**, as in P1.
5. **F22 grew into the largest defect class on demand** — 20.9 % of block B loops carry an intra-leg ring, against the census's 7.8 %. It costs 0.37 % of ride kilometres. By design (defect atlas §9 rank 4) and still a labelling question.
6. **`distance_error` did not improve** even though the rank score reads it (0.1628 census → 0.1615 P1.1 mean). Ranking reorders a bank; it cannot create an on-target loop the harvest band never offered. F21 stands.

## 14. Open questions

### For P2

1. **The gate's refill economics are the whole latency bill.** +3.4 builds/request. `thor.roundtrip_gate_refill_budget` **is now swept (§15)** and the answer is: bounding it meets the latency bar and fixes the fills, but starves ADR-0037's seam gate along with the geometry gate, so `spike_ge_500m` goes 0.00 → 2.08 %. **The knob is mis-scoped** — a budget that applied only to the geometry gate's rejects would buy the whole win for nothing, and that one-line change is P2's first move (§15.5). The other alternative still stands: score the candidate's *return corridor* before building it, so the gate rejects at harvest time rather than after an A\*.
2. **The intermediate rung earns 2.1 % for ~200 ms.** Either find the rung that converts (release the corridor's *exempt* stretch first? release twins only where the corridor is one-way?) or drop it. **§15 drops it:** turning the ladder off changes served slots 0–5 by *nothing* to the decimal, loses no request (`none` stays 14), and is what carries the knee under the 1.10× bar.
3. **Block C at c0.8 needs its own answer.** Not the switchback test. Candidates: let the twin tier degrade to the soft leash above some terrain ruggedness; or accept that a mountain out-and-back on a single serpentine *is* the ride and stop calling it a lollipop.
4. **Distinctness needs a partner mechanism**, and the sweep must be joint with latency: `roundtrip_xcand_strength` 0.5 is a known full win at a known latency cost (ADR-0039), and P1.1 has spent that headroom.
5. **The F01 pass at 300 km is 54 ms.** O(labels) with ~62 ns/label of pointer-chasing; the remaining wins are algorithmic (Euler-tour ancestor test, or an approximate bounded-window revisit check), not constant-factor.
6. **Fills 538/552.** Every short bank is budget-forced. **§15 settles this one:** capping gate refills takes fills to **552/552** at every budget tried, `underfill=cap` 31 → 0, and it does *not* cost served-slot cleanliness in slots 0–5 (±0.4 pp) — it costs the deep bank and the ≥ 500 m absolute. `kAttemptSlack` never needed raising, and could not have been raised by config anyway (`constexpr`, `route_action.cc:1019`).

### For Gate v2

7. **`spike_ge_500m == 0` is the gate's one true absolute and it caught a real regression here** — the first per-slot top-up put seam mirrors into 0.69 % of c0.5 loops and every other meter looked *better*. Keep it absolute.
8. **`shadow_frac` and `compactness` moved again in the right direction** (0.1478 → 0.0247 and 0.2349 → 0.3032 since the census). The census's advice to gate on them stands; they would have scored both P1 and P1.1 correctly where `edge_reuse_geom` barely moves.
9. **D1b's operating point matters more than ever.** At 500 m the residual is 7.6 %; at 2 km it is 0.3 %. `labeling-sample.jsonl` sets the point.
10. **Ratchet 9 needs its semantics decided before it can be applied** (§9). As a regression bar against prod, P1.1 fails by 1.096×; as a floor against what the rig serves today, it passes at 0.980×.
11. **A "served-slot" gate would be sharper than a population gate.** Every headline defect number in this document is dominated by slots a rider never reads; §8.3 is the table that actually predicts the product.

## 15. Gate refill-budget sweep

The AFK follow-up the HITL close asked for: §4's `thor.roundtrip_gate_refill_budget` and §5's
rung ladder, swept on corpus-v2 against the pinned `b4f514d7f` baseline, **config only — the
binary is `proto/v4-p1.1` @ `a1d24909b`, unchanged.** Fifteen full-corpus runs (552 requests,
K = 12) fired one engine at a time on :8003 (P1.1) and :8004 (baseline).

### 15.0 What the knob can and cannot express (asked for explicitly)

**`0` means *unlimited*, not "no refills".** `worker.cc:94-95` reads
`config.get<uint32_t>("thor.roundtrip_gate_refill_budget", 0)` and `route_action.cc:1397-1398`
maps it:

```cpp
const uint32_t gate_refill_budget =
    roundtrip_gate_refill_budget ? roundtrip_gate_refill_budget : 0xffffffffu;
```

so the **smallest expressible budget is 1**, and there is **no config value for a rank-only
gate**. The ticket's fourth sweep point — "0-refill, gated loops kept in the last tier, no
replacement build" — is therefore **not reachable from configuration on this binary**; it needs
a one-line sentinel change (`-1`, `"none"`, or an explicit `roundtrip_gate_refill` bool), which
is mechanism code and out of a config sweep's scope. **Budget 1 is the closest reachable point**:
at 4.93 gate rejects per request it refills ~20 % of them and keeps the rest, so it is rank-only
plus one rescue per request.

**`kAttemptSlack` is not a knob either** — `route_action.cc:1019` has
`constexpr uint32_t kAttemptSlack = 8`. The ticket's optional "+1" is a rebuild, not a config
flip. It is moot regardless: **every bounded budget already fills 552/552** (§15.2), so there is
nothing left for extra slack to buy.

### 15.1 The runs

| # | configuration | results dir | engine ledger |
|---|---|---|---|
| 1 | budget **unlimited** (= §11's run) | `results/p1-1-latency/` | `engine-lat-p11.log` |
| 2 | budget **4** | `results/p1-1-sweep-b4/` | `eng-b4.log` |
| 3 | budget **2** | `results/p1-1-sweep-b2/` | `eng-b2.log` |
| 4 | budget **1** | `results/p1-1-sweep-b1/` | `eng-b1.log` |
| 5 | budget **2** + `roundtrip_fallback_rungs=false` | `results/p1-1-sweep-b2-norungs/` | `eng-b2-norungs.log` |
| 6 | budget **1** + `roundtrip_fallback_rungs=false` | `results/p1-1-sweep-b1-norungs/` | `eng-b1nt.log` |
| — | latency repeats of 3 and 5 | `results/p1-1-sweep-b2-{repeat,tight}/`, `-b1-tight/`, `-b2-norungs-tight/` | — |
| — | baseline brackets A–F | `results/census-v2-sweep-base{A,B,C,D,E,F}/` | `eng-base{C,D,E,F}.log` |

**The engine is deterministic.** Budget 2 fired three times produced a *byte-identical* ledger
(`r0=7801 r1=65 r2=2120 none=14`, `refilled 865 kept 946`) and identical loop geometry each time;
the repeats differ only in wall latency. Every quality number below is therefore a single
measurement with no sampling error — **only latency needed repeating.**

### 15.2 Latency — and why it took four brackets

The box drifts. **Seven full-corpus baseline readings** were taken across the session:
1.299 (§11's pair) · 1.362 (A) · 1.031 (B) · 1.074 (C) · 1.181 (D) · 1.089 (E) · 1.246 (F) s —
**mean 1.183 s, sd 0.125 s = 10.5 %**, which confirms the ±12 % noise floor empirically and means
**a single pair cannot resolve the 1.10 × bar.** Bracket B..F run on a warm page cache; A and
§11's pair do not, which is most of the drift.

Two instruments are therefore reported: the harness's **wall p50** (the bar's own metric, and
what a rider feels — it includes queueing behind the other two workers) and the engine's own
**stage total** from the `roundtrip timing:` ledger (`roundtrip_stage_timing` was enabled on the
baseline engine for this sweep, which §11 could not do). Ratios are against the pooled baseline
of the same instrument.

| configuration | wall p50 (n) | **× base** | wall p95 | × base | engine stage ms | **× base** | attempts/req |
|---|---|---|---|---|---|---|---|
| baseline `b4f514d7f` | 1.183 s (7) | 1.000× | 3.109 s | 1.000× | 800 | 1.000× | 12.62 |
| budget **unlimited** | 1.917 s (1) | **1.620×** | 4.506 s | 1.449× | 1 310 | **1.637×** | 15.98 |
| budget **4** | 1.741 s (1) | **1.472×** | 3.849 s | 1.238× | 1 160 | **1.449×** | 14.26 |
| budget **2** | 1.362 s (3) | **1.151×** | 3.005 s | 0.967× | 896 | **1.120×** | 13.50 |
| budget **1** | 1.353 s (2) | **1.143×** | 3.139 s | 1.010× | 925 | **1.155×** | 12.92 |
| budget **2** + rungs off | 1.307 s (2) | **1.104×** | 2.907 s | 0.935× | 861 | **1.076×** | 13.51 |
| budget **1** + rungs off | 1.337 s (1) | **1.130×** | 2.872 s | 0.924× | 862 | **1.077×** | 12.93 |

Both instruments agree to within 3 pp on every row, which is the best available evidence that the
ordering is real and not box noise. Three readings matter:

1. **The budget is the whole latency bill, exactly as §11 predicted.** Capping refills at 4 takes
   1.62 → 1.47×; at 2 it takes it to **1.15×**. Attempts/request fall 15.98 → 14.26 → 13.50, and
   `astar` + `astar_fb` fall 1 142 → 994 → 802 ms — the two A\* lines again carry the change.
2. **Budget 1 buys nothing over budget 2.** 1.143× vs 1.151× on wall and 1.155× vs 1.120× on the
   engine: the two are indistinguishable, because the requests that trip the gate many times are a
   minority and 0.58 builds/request is inside the noise. **Budget 1 is Pareto-dominated** — it
   costs real cleanliness (§15.4, §15.5) for no latency at all.
3. **Only turning the rung ladder off clears the bar.** Budget 2 + `roundtrip_fallback_rungs=false`
   is **1.104× wall / 1.076× engine** — the ladder's `astar_fb` drops 214 → 148 ms, matching §11's
   −218 ms A/B. It converts 65 legs of 10 057; without it those 65 fall to rung 2 and `r2` goes
   2 120 → 2 187 (+0.6 % of legs). **`none` stays 14 — no request loses a route.**

### 15.3 Fills and the gate ledger

| configuration | fills 12/12 | short banks | `underfill=cap` | `underfill=queue` | gate rejects seam/twin/bounce | refilled | kept in bank | topped up | rungs r0/r1/r2/none | failures |
|---|---|---|---|---|---|---|---|---|---|---|
| unlimited | 538/552 | 11×9, 10×3, 9×1, 8×1 | **31** | 0 | 552 / 2 611 / 106 | 2 722 | 0 | 300 | 8303/67/3054/14 | 0 |
| budget 4 | **552/552** | — | 0 | 0 | 444 / 1 943 / 98 | 1 342 | 704 | 0 | 8101/67/2365/14 | 0 |
| budget 2 | **552/552** | — | 0 | 1 | 398 / 1 715 / 92 | 865 | 946 | 1 | 7801/65/2120/14 | 0 |
| budget 1 | **552/552** | — | 0 | 2 | 380 / 1 608 / 89 | 479 | 1 223 | 2 | 7517/65/2003/14 | 0 |
| budget 2 + rungs off | **552/552** | — | 0 | 1 | 397 / 1 724 / 91 | 867 | 952 | 1 | 7805/**0**/2187/14 | 0 |
| budget 1 + rungs off | **552/552** | — | 0 | 2 | 380 / 1 614 / 89 | 479 | 1 229 | 2 | 7519/**0**/2068/14 | 0 |

**Gate 7 of §12 flips from FAIL to PASS at every bounded budget.** §6's diagnosis is confirmed
end to end: the 14 short banks were *budget-forced*, the budget was being spent on gate refills,
and bounding the refills returns it. `underfill=cap` goes **31 → 0**; the 1–2 requests that now
end `underfill=queue` are genuinely network-forced and the last-resort top-up fills them anyway
(`topped up` 1–2 loops), so the served bank is still 12/12.

Reject counts fall with the budget (2 722 → 1 811 at budget 2) because fewer replacement builds
means fewer built candidates to reject — the gate is not getting weaker, the pipeline is getting
shorter. The ledger counts rejects **by kind** but refills/keeps **in aggregate** (a loop can trip
two kinds at once), so a per-kind refilled/kept split is not attributable from this build's ledger.

### 15.4 Served-slot cleanliness — slots 0–2 and 3–5

The question the ticket actually cares about. Unlimited is the reference column.

| | unlimited | budget 4 | budget 2 | budget 1 | b2 + rungs off | b1 + rungs off |
|---|---|---|---|---|---|---|
| **slots 0–2** near-mirror + same-pavement | 5.4 % | 5.5 % | 5.7 % | 5.8 % | **5.7 %** | 5.8 % |
| slots 0–2 D1b unseen mean m | 121 | 122 | 125 | 125 | **125** | 125 |
| slots 0–2 D1b unseen ≥ 500 m | 8.9 % | 9.1 % | 9.3 % | 9.1 % | **9.3 %** | 9.1 % |
| slots 0–2 D1 same-pavement run ≥ 500 m | 11.4 % | 11.3 % | 11.0 % | 10.9 % | **11.0 %** | 10.9 % |
| **slots 3–5** near-mirror + same-pavement | 3.3 % | 3.3 % | 3.1 % | 3.0 % | **3.1 %** | 3.0 % |
| slots 3–5 D1b unseen mean m | 124 | 120 | 113 | 109 | **113** | 109 |
| slots 3–5 D1b unseen ≥ 500 m | 8.6 % | 8.4 % | 7.9 % | 7.5 % | **7.9 %** | 7.5 % |
| slots 3–5 D1 same-pavement run ≥ 500 m | 7.4 % | 7.3 % | 7.4 % | 7.7 % | **7.4 %** | 7.7 % |
| slots 9–11 D1 same-pavement run ≥ 500 m | 9.0 % | 17.9 % | 30.6 % | 40.6 % | 31.1 % | 41.2 % |
| slots 9–11 fallback-like heavy reuse | 8.8 % | 15.8 % | 23.1 % | 31.3 % | 23.3 % | 31.5 % |

**Slots 0–5 do not move.** Against the unlimited run every metric a rider reads is within
**±0.4 pp and ±4 m at every budget** — and the rung ladder changes them by **nothing at all**
(budget 2 and budget 2 + rungs off are identical to the decimal in slots 0–5; the ladder only
ever touched the deep bank). That is the mechanism working as designed: a kept-gated loop is
marked and sorted into the last tier, so it can only surface where the cell had nothing better.

**The deep bank pays.** Slots 9–11 go 9.0 → 17.9 → 30.6 → 40.6 % on D1 same-pavement runs. §8.3's
"the deep bank stopped being a dump" is a property of *unlimited* refills, and the budget sells it
back. At budget 2 the deep bank sits between the census and P1; at budget 1 it is back to P1's
44.9 %-class dump. **This is the second reason budget 1 is dominated.**

Worst-20 by D1b unseen metres, and where they sit:

| configuration | worst loop | its slot | worst-20 slot spread |
|---|---|---|---|
| unlimited | 2 759 m / 6.1 % of a 45.2 km ride | 8 | 0–11 (two in slots 0–2) |
| budget 4 | 4 002 m / 10.2 % of 39.4 km | 11 | 4–11 |
| **budget 2 (± rungs)** | **4 002 m / 10.2 %** | **11** | **3–11, sixteen of twenty in slots 10–11** |
| budget 1 (± rungs) | **11 193 m / 28.5 %** of 39.3 km | 11 | **all twenty in slots 10–11** |

Budget 1 puts P1's single worst loop — the same vracar 50 km c0.5 s101 ride, 28.5 % of it a
near-mirror — back into the bank. It is parked in slot 11, but it is back.

### 15.5 The cost the sweep found that nobody was looking for

**Every bounded budget breaks Gate v1.3's one true absolute.**

| configuration | `spike_ge_500m` ALL | slots 0–2 | slots 3–5 | slots 6–8 | slots 9–11 | `spike_ge_30m` | max stub |
|---|---|---|---|---|---|---|---|
| baseline / unlimited | **0.00 %** | 0 | 0 | 0 | 0 | **0.00 %** | 0 m |
| budget 4 | 1.04 % | **1** | 6 | 24 | 38 | 1.19 % | 23.8 km |
| budget 2 (± rungs) | 2.08 % | **0** | **4** | 34 | 100 | 2.52 % | 23.8 km |
| budget 1 (± rungs) | 2.42 % | **0** | **4** | 44–45 | 111–112 | 3.3 % | 23.8 km |

**Why.** The refill budget is **shared between the seam gate and the geometry gate**. ADR-0037's
seam gate has always refilled unconditionally, and its rejects *are* the ≥ 500 m U-turn stubs that
`spike_ge_500m == 0` forbids. Capping the budget starves the seam gate along with the new one:
`spike_ge_30m` — the seam gate's own 30 m threshold — goes 0.00 → 3.3 %, and 398 seam rejects per
corpus at budget 2 stop being refilled. §14's open question 7 said this absolute "caught a real
regression here"; it has caught a second one. Two of budget 2's worst-20 are literally classified
`seam residue` (novisad 20 km, 3 685 m, slot 10).

**The knob is mis-scoped, and that is the sweep's most useful finding.** A budget that applied to
the *geometry* gate's rejects only — leaving seam rejects always refilled, as ADR-0037 has them —
would buy the entire latency win with none of this. That is a one-line change in the
`stub >= kSeamStubRejectM || gate_twin || gate_bounce` branch at `route_action.cc:2059-2068` and
it is the first thing P2 should do.

Mitigating the alarm: at budget 2 **slots 0–2 are still spike-free**, and slots 3–5 carry 4 loops
of 1 656 (**0.24 %**). The population meter fails; the served surface very nearly does not.

### 15.6 Whole-bank quality, curviness, rings

| configuration | D1b ≥ 500 m Vračar | D1b ≥ 500 m demand | D1 run ≥ 500 m Vračar | D1 run ≥ 500 m demand | D4 fallback proxy | rings/loop |
|---|---|---|---|---|---|---|
| baseline | 61.1 % | 64.1 % | 33.6 % | 24.6 % | — | 1.09 |
| unlimited | **0.4 %** | **8.1 %** | **0.0 %** | **2.8 %** | 10.2 % | 0.54 |
| budget 4 | 0.8 % | 8.0 % | 3.2 % | 3.5 % | 13.0 % | 0.56 |
| budget 2 | 1.8 % | 8.2 % | 9.3 % | 6.2 % | 16.9 % | 0.61 |
| budget 2 + rungs off | 1.6 % | 8.1 % | 9.3 % | 6.5 % | 17.2 % | 0.61 |
| budget 1 | 3.9 % | 8.8 % | 12.8 % | 9.2 % | 20.6 % | 0.65 |
| budget 1 + rungs off | 3.8 % | 8.7 % | 12.8 % | 9.5 % | 21.0 % | 0.65 |

**Gates 1–4 of §12 stay green at every budget** (the bar is "down ≥ 50 % and ≥ P1's"): the worst
cell is budget 1's Vračar D1 run at 12.8 % against a 16.8 % bar. The margin is real but it is
shrinking, and budget 2 keeps roughly twice as much of it as budget 1.

Curviness retention is **flat across the whole sweep** — c0.5 1.003–1.010×, c0.7 1.059–1.067×,
**c0.8 0.939–0.945×**, c1.0 1.029–1.048×. §12 gate 5 fails at c0.8 in every configuration
including unlimited; **the budget neither causes nor fixes it** (§13.1 — it is block C's
twin exclusion, not the gate's economics). Ring share creeps 43.4 → 46.4 → 48.0 % as kept-gated
loops stay in the bank, still far below the census's 64.1 %.

Failures: **0 in all fifteen runs.**

### 15.7 The knee, and the Pareto front

**No configuration satisfies every bar.** Latency ≤ 1.10 × and `spike_ge_500m == 0` are mutually
exclusive on this binary, because the one knob that buys the latency is the same knob that starves
the seam gate (§15.5). So both are given.

**The knee — `roundtrip_gate_refill_budget = 2` with `roundtrip_fallback_rungs = false`:**

- **latency 1.104× wall p50** (1.076× on the engine-stage instrument), p95 **0.935×** — the bar is
  met at the point estimate on both instruments, with the honest caveat that the baseline's own
  sd is 10.5 %, so "1.10×" and "1.15×" are not separable in one pair. It is the only configuration
  whose point estimate is at or under the bar on **both** instruments.
- **fills 552/552**, `underfill=cap` 0, failures 0.
- **served slots 0–5 within ±0.4 pp and ±4 m of the unlimited run on every near-mirror (D1b) and
  same-pavement (D1) metric** — that is the stated tolerance, and it is met.
- **the tolerance it breaks:** 4 loops of 1 656 in slots 3–5 (0.24 %) carry a ≥ 500 m seam stub
  that unlimited refilled away; slots 0–2 stay spike-free.

Budget 1 is **dominated** — identical latency (1.143× / 1.130× vs 1.151× / 1.104×, all inside
noise), a dirtier deep bank (40.6 % vs 30.6 %), a worse whole-bank Vračar D1 (12.8 % vs 9.3 %) and
P1's 11 193 m loop back in the bank. It should not be shipped at any price.

Pareto front (latency × served-slot cleanliness × fills):

| point | latency ×base (wall / engine) | slots 0–5 spiked loops | slots 9–11 D1 | fills | verdict |
|---|---|---|---|---|---|
| **unlimited** | 1.620 / 1.637 | **0** | **9.0 %** | 538/552 | cleanest, fails latency **and** fills |
| **budget 4** | 1.472 / 1.449 | 7 (one in slots 0–2) | 17.9 % | 552/552 | fails latency; buys back most of the deep bank |
| **budget 2** | 1.151 / 1.120 | 4 (none in 0–2) | 30.6 % | 552/552 | ~1.15×; keeps the rung ladder's 2.1 % conversion |
| **budget 2 + rungs off** | **1.104 / 1.076** | 4 (none in 0–2) | 31.1 % | 552/552 | **the knee** |
| ~~budget 1 (± rungs)~~ | 1.143 / 1.155 | 4 (none in 0–2) | 40.6 % | 552/552 | **dominated — no latency win, real cleanliness loss** |

Andrey's pick is really between three rows: **unlimited** if the ≥ 500 m absolute is
non-negotiable and the latency bar is the thing to renegotiate; **budget 4** if the deep bank is
worth 1.47×; **budget 2 + rungs off** if the 1.10× bar is the hard one. The fourth and best option
is not on this list because it is not a config: **scope the budget to the geometry gate only**
(§15.5) and the front collapses to a single dominant point.

### 15.8 Recommended `valhalla.json`

The rig's `thor` block carries none of P1.1's knobs (`data/valhalla.json` — checked). The knee is
two lines on top of the P1.1 defaults:

```jsonc
"thor": {
  // ... existing prod thor block unchanged ...

  // proto/v4-p1.1 geometry Defect Gate — the knee from the refill-budget sweep
  // (research 2026-09-06-p1-1-road-identity-iteration.md §15).
  "roundtrip_gate_refill_budget": 2,      // 0 = unlimited; 2 => 1.10x p50, fills 552/552
  "roundtrip_fallback_rungs": false,      // the ladder converts 2.1 % of failed legs for ~65 ms

  // left at their P1.1 defaults, listed so the served config is explicit:
  "roundtrip_switchback_test": true,
  "roundtrip_built_ranking": true,
  "roundtrip_geometry_gate": true,
  "roundtrip_gate_twin_ride_m": 500,
  "roundtrip_gate_return_bounce_m": 30,
  "roundtrip_f09_budget": true
}
```

**Do not ship this without reading §15.5.** It trades `spike_ge_500m` 0.00 → 2.08 % (0 loops in
slots 0–2, 4 in slots 3–5) for the latency bar. If that trade is unacceptable — and Gate v2's
open question 7 argues it should be — the answer is not a different budget value, it is scoping
the budget to the geometry gate's rejects so the seam gate keeps refilling unconditionally.

## Appendix A — commands

Branch tip: **`proto/v4-p1.1`**, see Appendix C. Nothing pushed; `curvature-costing` untouched.

```bash
# build (the warm audit image, one long-lived container, docker cp + incremental make)
docker start rt-p1-build
docker cp <file> rt-p1-build:/src/valhalla/<file>
docker exec rt-p1-build bash -lc "cd /src/valhalla && make -C build -j6 valhalla_service \
    gurka_roundtrip_audit gurka_motorcycle_roundtrip gurka_roundtrip_distinctness"
# NOTE: make installs nothing — run build/valhalla_service, not /usr/local/bin's b4f514d7f.

# the baseline engine container was REBUILT for P1.1: rt-p1-base ran valhalla_service as
# PID 1, so `pkill` inside it killed the container and every config swap silently
# reverted.  rt-p11-base runs `sleep infinity` and takes the engine as an exec.
docker run -d --name rt-p11-base -p 8004:8004 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  valhalla-curvature:prod-equivalent-b4f514d7f bash -lc "sleep infinity"

# configs: :8003 = P1.1, :8004 = baseline; -xcand.json adds penalty/0.2/cap 4
docker exec rt-p1-build python3 -c "import json; d=json.load(open('/custom_files/valhalla.json')); \
  d['httpd']['service']['listen']='tcp://*:8003'; d['thor']['roundtrip_stage_timing']=True; \
  json.dump(d, open('/tmp/v8003.json','w'))"
docker exec -d rt-p1-build bash -lc "exec /src/valhalla/build/valhalla_service /tmp/v8003.json 2 > /tmp/engine.log 2>&1"

# corpus (nothing under tools/loopqual written except each run's own output dir)
cd /Users/xenix/Projects/curvagen-valhalla/tools/loopqual
./loopqual run --engine http://localhost:8003 --corpus corpus-v2.yaml \
  --out results/p1-1-road-identity/ --trace-engine http://localhost:8003 --engine-note "..."
./loopqual compare results/census-v2-b4f514d7f/report.json results/p1-1-road-identity/report.json
python3 gate_v1_proto.py results/census-v2-latency results/p1-1-latency

# detectors (P1's scripts with the fixed D3 counter, re-pointed by env)
~/.curvagen-scratch/p1.1/analyse.sh <run_dir> p11v2
LQ_RUN=<run> LQ_PREFIX=p11v2 python3 ~/.curvagen-scratch/p1.1/census_agg.py [det|v13|mech|slot]
LQ_RUN=<run> LQ_PREFIX=p11v2 python3 ~/.curvagen-scratch/p1.1/slot_clean.py

# engine ledger (3 workers interleave, so only aggregates are attributable)
python3 ~/.curvagen-scratch/p1.1/ledger_agg.py <engine.log> ["YYYY-MM-DD HH:MM:SS"]

# gallery: 20 worst P1.1 beside 20 worst P1, [lat, lon] order
LQ_RUN=<p1.1 run> LQ_PREFIX=p11v2 LQ_TAG="P1.1 " \
LQ_RUN_B=<p1 run> LQ_PREFIX_B=p1v2 LQ_TAG_B="P1 " \
  python3 ~/.curvagen-scratch/p1.1/p1_gallery.py <run>/gallery-p1-1.html
```

**The §15 sweep** (config only — same binary, same corpus, one engine at a time):

```bash
# per-budget configs, written next to /tmp/v8003.json inside the build container
docker exec rt-p1-build python3 -c "import json; b=json.load(open('/tmp/v8003.json')); \
  b['thor']['roundtrip_gate_refill_budget']=2; json.dump(b,open('/tmp/v8003-b2.json','w'))"
# ... and -b4 / -b1, plus roundtrip_fallback_rungs=false for the -norungs variants.

# the baseline needs the ledger too, which §11 could not read:
docker exec rt-p11-base python3 -c "import json; d=json.load(open('/custom_files/valhalla.json')); \
  d['httpd']['service']['listen']='tcp://*:8004'; d.setdefault('thor',{})['roundtrip_stage_timing']=True; \
  json.dump(d,open('/tmp/v8004.json','w'))"

~/.curvagen-scratch/p1.1/sweep_lib.sh          # run_p11 / run_base: stop both engines, start one,
                                               # wait for /status, fire the corpus, docker cp the ledger
~/.curvagen-scratch/p1.1/sweep-phase{1,1b,2,3,4}.sh   # the fifteen runs, in order
~/.curvagen-scratch/p1.1/sweep-analyse.sh <run_dir> <prefix>   # detectors + det/mech/v13/slot tables
python3 ~/.curvagen-scratch/p1.1/sweep_agg.py <run_dir> <engine.log>   # one JSON row per configuration
```

**A trap worth recording:** a `loopqual run` started with `nohup ... &` from a tool shell dies with that shell, and its children keep firing at the engine after the parent is gone. One P1.1 corpus was measured while a stray runner hammered the same engine (p50 3.95 s, fire 641 s); §11's numbers come from a dedicated uncontended pair. Check `pgrep -f 'loopqual run'` before trusting any latency number on this box.

## Appendix B — artefacts

| Path | What |
|---|---|
| `tools/loopqual/results/p1-1-road-identity/` | the main run: 552 responses, `loops.jsonl` (6 602), `report.{json,md}`, way pass on |
| **`tools/loopqual/results/p1-1-road-identity/gallery-p1-1.html`** | **40-case gallery: 20 worst P1.1 + 20 worst P1, same axis** |
| `tools/loopqual/results/p1-1-latency/` · `census-v2-latency/` | the uncontended latency pair (§11) |
| `tools/loopqual/results/p1-1-xcandon/` · `census-v2-xcandon/` | ratchet 9 apples to apples, both engines at `xcand 0.2` (§9) |
| `tools/loopqual/results/census-v2-control-p11{,b}/` | same-session baseline controls; both reproduce the census to the metre |
| `tools/loopqual/results/p1-1-refill-every/` | the discarded first corpus: refill on every gate reject (§4) |
| `~/.curvagen-scratch/p1.1/{analyse,ledger_agg,slot_clean,p1_gallery}.py`, `ab-*.log` | the P1.1 scratch tooling and the knob A/B |
| `~/.curvagen-scratch/p11v2*.jsonl` | detector output for the P1.1 run |
| `~/.curvagen-scratch/p1.1/engine-*.log` | engine ledgers: sidecar + switchback build, identity/gate/rung counters, ranking order, stage timing |
| **`tools/loopqual/results/p1-1-sweep-{b4,b2,b1,b2-norungs,b1-norungs}/`** | **§15's six swept configurations (the unlimited point is `p1-1-latency/`)** |
| `tools/loopqual/results/p1-1-sweep-b2-{repeat,tight}/` · `-b1-tight/` · `-b2-norungs-tight/` | §15.2's latency repeats — same ledger, different wall clock |
| `tools/loopqual/results/census-v2-sweep-base{A..F}/` | the six baseline brackets; A/B bracket the session, C–D and E–F are the two tight interleaved blocks |
| **`tools/loopqual/results/p1-1-sweep-summary.md`** | **the sweep summary: the knee, the Pareto front, the config snippet** |
| `~/.curvagen-scratch/p1.1/{sweep_lib.sh,sweep-phase*.sh,sweep-analyse.sh,sweep_agg.py,sweep_tables.py}` | the sweep driver and its aggregation |
| `~/.curvagen-scratch/p1.1/eng-{b4,b2,b1,b2t,b1t,b2r,b2-norungs,b1nt,base*}.log` | one engine ledger per swept run |
| `~/.curvagen-scratch/sw{4,2,1,2n,1n}*.jsonl` · `~/.curvagen-scratch/p1.1/agg-sw*-*.txt` | detector output and tables for the swept configurations |

Container `rt-p1-build` (the P1.1 build + engine, :8003) is **stopped, not removed**, so P2 re-uses the warm `build/` — a rebuild from cold is ~40 minutes. `rt-p11-base` (the baseline on :8004) is created and removed per session; recreate it with the `docker run` line in Appendix A. `valhalla-local` (:8002) and the results `http.server` on :8791 were never addressed.

## Appendix C — the branch

`proto/v4-p1.1`, cut from `proto/v4-p1` @ `6fb8e37df`. Three `proto(v4-p1.1):` commits plus this document. Nothing pushed; `curvature-costing` is untouched and carries this document as an untracked file at the same path.

**The branch copy of this document is stale.** §15 was written by the sweep session into the
**untracked working-tree copy** on `curvature-costing`; the copy committed on `proto/v4-p1.1`
@ `a1d24909b` still ends at Appendix C. The sweep changed no code, so the branch's *binary* is
current — only its document is behind. Fold §15 into the branch when the parent session next
commits there; nothing was pushed and no branch was switched to write it.

Touched: `valhalla/thor/road_twin_index.h`, `src/thor/road_twin_index.cc`, `valhalla/thor/roundtrip_expansion.h`, `src/thor/roundtrip_expansion.cc`, `src/thor/route_action.cc`, `valhalla/thor/worker.h`, `src/thor/worker.cc`, `test/gurka/test_roundtrip_audit.cc`.

Config surface added on top of P1's five knobs (all `thor.*`):

| knob | default | meaning |
|---|---|---|
| `roundtrip_switchback_test` | `true` | the OSM-way / constant-offset test on twin and parallel qualification |
| `roundtrip_built_ranking` | `true` | rank the built loop; `false` = P1's clean-first-then-harvest-curviness |
| `roundtrip_geometry_gate` | `true` | the twin-ride and mid-return-mirror gate halves |
| `roundtrip_gate_twin_ride_m` | `500` | twin metres beyond the Start Exemption that reject a loop |
| `roundtrip_gate_return_bounce_m` | `30` | mid-return exact-mirror metres that reject a loop |
| `roundtrip_gate_refill_budget` | `0` (unlimited) | gate rejects that may buy a replacement build |
| `roundtrip_fallback_rungs` | `true` | the intermediate rung (twins released, corridor still barred) |
| `roundtrip_fallback_parallel_rung` | `false` | the ticket-literal parallels-only rung, kept for the measurement |
| `roundtrip_f09_budget` | `true` | the stall branch's fresh `+want` attempt budget |
| `roundtrip_rank_overlap_w` / `_disterr_w` | `4.0` / `1.0` | the rank score's two penalties |

**Primary sources:** `docs/curvagen/research/2026-09-06-p1-road-identity.md` §1/§2/§6/§7/§8/§11 · `2026-09-06-defect-atlas-v2.md` §8/§9 · `2026-09-05-v3-correctness-optimality-audit.md` F01/F08/F09/F13/F20/F21/F22 · `2026-09-05-audit-rig-confirmation.md` §6 · `docs/curvagen/extension-api.md` · `tools/loopqual/{metrics.py,runner.py,gate_v1_proto.py,corpus-v2.yaml}`.

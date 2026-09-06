# P1.1 — switchback-safe twins, built-loop ranking, the geometry Defect Gate

- **Date:** 2026-09-06 (curvagen-valhalla [#12](https://github.com/Lazark0x/curvagen-valhalla/issues/12), P1.1 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); blocks [#11](https://github.com/Lazark0x/curvagen-valhalla/issues/11))
- **Scope:** a **throwaway prototype** on branch `proto/v4-p1.1`, cut from `proto/v4-p1` @ `6fb8e37df`. Seven items from the P1 close: the switchback test on the twin sidecar, the F01 pass cost, ranking the *built* loop, a geometry Defect Gate, an intermediate fallback rung, the seven under-fills, and gurka for each. Nothing pushed; no production traffic; `valhalla-local` (:8002) never addressed.
- **Method:** the P1 rig, unchanged — build in the warm `valhalla-fork-test:audit` container (arm64, `Release`), serve on :8003 against the same read-only Serbia tiles as the census (`curvagen-orchestrator/data`, `tileset_last_modified 1785932567`), fire `corpus-v2` (552 requests, K = 12) with the census's own run line (engine mode, 3 workers, way pass on). The baseline engine `valhalla-curvature:prod-equivalent-b4f514d7f` serves :8004. Detectors are P1's scripts with the fixed D3 counter, re-pointed by env, so every column in this document is read by byte-identical code.

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

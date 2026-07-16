# P1 — full-pipeline latency profile, ranked hotspots on Valhalla 3.8.2

_Wayfinder map #66 (latency optimization), ticket #70. 2026-07-15._

The measured per-stage latency breakdown of the round-trip serving path on the
3.8.2-rebased fork, so the per-algo audit tickets target real hotspots instead of
guesses. **Headline: the return-leg A\* search (`astar` + `astar_fb`) is the whole
game — 56 % of p50, up to 65 % at 300 km, and the sole home of xcand's cost.**
Everything else is a ledger footnote.

## Method

| | |
|---|---|
| Engine | `valhalla-fork-test:rebase38` (fork `a2fd86343`, 3.8.2 base, K1) |
| Tiles | Balkans tar-extract, `Backend/data/valhalla_tiles.tar` (364 tiles, warm page cache) |
| Rig | `tools/loopqual`, `corpus-v1.yaml` (232 req, K=12), `--workers 1 --no-way` |
| Instrument | existing `thor.roundtrip_stage_timing` ledger (`route_action.cc:2175`), **no rebuild** |
| A/B | run A `valhalla-A.json` xcand OFF; run B `valhalla-B.json` `roundtrip_xcand_penalty=true` (strength 0.5 / cap 4) |
| Attribution | engine log line per request (job-ordered, workers=1) zipped to loopqual per-request `latency_s`; `framing = latency_s − Σ(9 stages)` |
| Scratch | `~/.curvagen-scratch/p1-profile/` (configs, `off/on-timing.txt`, `aggregate.py`); loopqual `results/p1-xcand{off,on}/` |

**Why workers=1, not prod-like:** one request in flight ⇒ zero CPU contention ⇒
clean per-request stage wall-clock and log-order = job-order (curviness/distance
attribution is free). The cost: the headline ratio understates the contended
prod figure. My clean **xcand = 1.19× p50 / 1.14× p95** vs the map's contended
**1.32×** — contention amplifies the delta. The *decomposition* (where the cost
goes) is condition-independent and is P1's actual deliverable; the ratchet gate
stays a contended measurement.

**Stages** are the `roundtrip_impl` ledger: `harvest` (one forward expansion +
turnaround harvest, incl. ScanBand), `scan` (widened Distance-Flex re-scan),
`walkback`, `rejoin` (return-leg leash+xcand penalty *setup*), `astar` (return-leg
A\*, ×K candidates), `astar_fb` (soft-leash fallback return A\*), `seam` (build-time
Defect Gate), `secondvia` (Second Via rebuild), `build` (TripLegBuilder / trip
decode). `framing` = the residual outside `roundtrip_impl`: loki correlation +
odin/directions + trip serialize + HTTP loopback (not separately timed — see
ticket for a split).

## 1. Ranked per-stage profile — xcand OFF (the baseline)

Overall (n=232), absolute ms and share of the p50 request (total p50 **514 ms** /
p95 **1700 ms** / mean 678 ms):

| rank | stage | p50 ms | p95 ms | mean | % p50 | what it is |
|---|---|---|---|---|---|---|
| 1 | **astar** | **230** | **1061** | 361 | **45 %** | K return-leg A\* searches (leash + rejoin-graded) |
| 2 | framing | 69 | 248 | 89 | 13 % | loki + odin + serialize + HTTP (outside `roundtrip_impl`) |
| 3 | **astar_fb** | 55 | **521** | 131 | 11 % | soft-leash fallback return A\* — tail monster |
| 4 | harvest | 24 | 192 | 49 | 5 % | single forward expansion + ScanBand harvest (amortized over K) |
| 5 | build | 20 | 61 | 25 | 4 % | TripLegBuilder (trip decode, 24 legs) |
| 6 | scan | 8 | 48 | 12 | 2 % | widened Distance-Flex re-scan |
| 7 | secondvia | 4 | 142 | 17 | 1 % | Second Via shape rebuild (rare, spikes) |
| 8 | rejoin | 2 | 6 | 2 | 0 % | return-leg penalty *setup* (the xcand apply site) |
| 9 | seam | 1 | 10 | 2 | 0 % | build-time Defect Gate seam check |
| 10 | walkback | 0 | 0 | 0 | 0 % | candidate walk-back (negligible) |

**`astar` + `astar_fb` = 56 % of p50, and dominate the p95 tail (1061 + 521 ms).**
The return leg — not the forward curvy expansion — is where the round-trip action
spends its time, because it runs K=12 times per request (one leash-penalized A\*
per candidate) and each can spill into the unbounded fallback.

### Scaling by distance (the real driver)

`astar`'s share climbs with distance; the forward `harvest` grows too but stays
amortized. `astar_fb`'s p95 peaks at **100 km** — mid-distance networks where
fresh-road returns are scarcest trigger the deepest fallback thrash.

| dist | n | total p50 | astar p50 (%p50) | astar_fb p95 | framing p50 | harvest p50 |
|---|---|---|---|---|---|---|
| 20 km | 40 | 132 | 77 (58 %) | 521 | 17 | 1 |
| 50 km | 56 | 334 | 192 (57 %) | 243 | 38 | 10 |
| 100 km | 40 | 598 | 244 (41 %) | **996** | 91 | 26 |
| 200 km | 56 | 853 | 432 (51 %) | 477 | 126 | 69 |
| 300 km | 40 | 1279 | **779 (61 %)** | 563 | 175 | 87 |

### By curviness

| curviness | n | total p50/p95 | astar p50 (%) | astar_fb p50/p95 |
|---|---|---|---|---|
| 0.5 | 200 | 526 / 1742 | 217 (41 %) | 54 / 523 |
| 0.8 | 32 | 500 / 1397 | 255 (**51 %**) | 81 / 361 |

Higher curviness pushes a **larger** `astar` share (curvier candidate edges cost
more per relaxation and the search wanders more) at slightly lower total, because
the 0.8 cell has no 300 km jobs.

## 2. xcand decomposition — where the +98 ms p50 / +243 ms p95 goes

Per-stage ON−OFF delta (median of each stage; note percentiles are **not**
additive, so per-stage p50 deltas need not sum to the total delta):

| stage | OFF p50 | ON p50 | Δp50 | OFF p95 | ON p95 | **Δp95** |
|---|---|---|---|---|---|---|
| astar | 230 | 213 | −17 | 1061 | 1347 | **+286** |
| astar_fb | 55 | 62 | +7 | 521 | 792 | **+271** |
| rejoin | 2 | 4 | +2 | 6 | 16 | +10 |
| framing | 69 | 74 | +5 | 248 | 278 | +30 |
| scan | 8 | 8 | 0 | 48 | 55 | +7 |
| secondvia | 4 | 4 | 0 | 142 | 15 | **−127** |
| harvest | 24 | 24 | 0 | 192 | 179 | −13 |
| build | 20 | 19 | −1 | 61 | 58 | −3 |
| **total** | **514** | **612** | **+98** | **1700** | **1943** | **+243** |

**Answering the ticket's three-way question:**

- **Wider A\* node expansion — DOMINANT.** xcand's cost is a *tail* phenomenon:
  the median request barely moves, but at p95 `astar` +286 ms and `astar_fb`
  +271 ms. The cross-candidate penalty (`route_action.cc:1673`) marks every edge
  earlier loops used, so each later candidate's return A\* must expand further to
  find fresh road, and more of them spill into the unbounded soft-leash fallback.
  Confirmed by the counters: fallbacks/req actually *drop* 6.29→5.59, yet
  `astar_fb` p95 rises +271 ms and max 1145→1439 ms — **each** fallback search
  gets deeper, not more numerous.
- **Rejoin-tier merge — small.** `rejoin` (the stage that *applies* the penalty)
  moves +2 ms p50 / +10 ms p95. Applying the penalty is cheap; the expansion it
  provokes downstream is not.
- **Penalty bookkeeping — negligible.** The per-edge cross-candidate count map
  (`route_action.cc:2102`) never surfaces as a distinct cost; sub-ms, folded.

**The win is real and priced:** mean `max_pair_overlap` **0.570 → 0.463** (matches
ADR-0038). And a partial **offset**: xcand ON cuts Second Via firing 0.40→0.08/req
(more-distinct candidates trip the shape-rebuild less), returning −127 ms at p95.

⟹ The xcand-direct optimization must attack the **penalized return-leg search
width** (`astar`/`astar_fb` tail), not the bookkeeping. Same stage as the #1
hotspot, different angle — see tickets below.

## 3. Hotspot shortlist → audit tickets

1. **Return-leg A\* expansion (`astar`)** — THE hotspot. 45 % p50, 61 % @ 300 km,
   the p95 tail, and the landing site of xcand's cost. Audit angles: is running K
   independent leash-penalized A\* searches necessary, or can they share pruning /
   the forward tree? The unidirectional `expand_within_distance` L1 = **100 km**
   (R2 lever #1) directly bounds this search — capping it is the first lever to
   measure. Per-edge `motorcyclecost` curvature eval lives *inside* this stage;
   profile it with a **sampling profiler (perf)**, not inline timers (which would
   distort a 2 ms-granularity ledger).
2. **Fallback return A\* (`astar_fb`)** — tail hotspot. 11 % p50 but 521 ms p95
   (996 ms @ 100 km), unbounded, and xcand's biggest tail multiplier. Audit:
   bound / short-circuit the soft-leash retry, or avoid triggering it (better
   first-pass leash so fewer candidates fall back).
3. **xcand-direct** (guaranteed ticket) — reduce the *marginal* return-search
   width the cross-candidate penalty adds, holding the 0.570→0.463 overlap win.
   Levers: cap penalized-search node expansion; reformulate the penalty to prune
   earlier; or fold the distinctness into the harvest/bucketing stage instead of
   the per-edge return cost. Shares stage 1's code; scoped by the "hold the win"
   constraint.
4. **`framing` split** — 13 % p50 (loki + odin + serialize + HTTP), unmeasured
   internally. A cheaper instrumentation ticket: add coarse loki/odin/serialize
   timers (or read valhalla's per-action logs) to see whether odin directions for
   24 legs or JSON serialize of 12 full-geometry routes dominates. Grows to
   175 ms @ 300 km, so non-trivial at long distance.

## 4. Ledger stubs — cheap stages, no audit ticket

- **harvest** (24 ms p50 / 192 p95) — single forward expansion, amortized over K;
  grows to 87 ms @ 300 km. Watch, don't audit; folded into the stage-1 lever
  discussion (same `unidirectional_astar` block governs it).
- **build** (20 ms p50) — TripLegBuilder decode of 24 legs; inherent, steady, no
  cheap win.
- **scan** (8 ms p50) — Distance-Flex re-scan; only fires on stalled cells.
- **secondvia** (4 ms p50, 142 p95) — rare shape rebuild; xcand *reduces* it.
  Leave alone.
- **rejoin / seam / walkback** (≤2 ms p50) — penalty setup, Defect Gate seam
  check, walk-back. Effectively free; ignore.
- **distance correction** — counted (4.2/req) but time folded into the rebuild's
  `astar`; not worth a separate timer given the stage-1 audit will re-measure the
  rebuild path anyway.

## 5. R2 knob-identity answers (ticket #70 comment, #68 levers)

Resolved from source + the live `valhalla.json`:

- **Lever #1 — which hierarchy block does the round-trip Dijkstras expansion read?**
  `unidirectional_astar`. The round-trip forward expansion is a `Dijkstras`
  subclass (unidirectional, `is_bidir=false`, `route_action.cc:577`); its costing
  reads `hierarchy_limits` from the **`unidirectional_astar`** block →
  `expand_within_distance` L1 = **100 000 m (100 km)**, L2 = 5 km (vs
  bidirectional/costmatrix L1 = 20 km). This is the p50 surface for the `astar`
  hotspot; capping it is the first stage-1 lever.
- **Lever #2 — which reserved label pool does `bdedgelabels_` draw from?**
  `max_reserved_labels_count_dijkstras` = **4 000 000** (`dijkstras.cc:17`), with
  `clear_reserved_memory: false` (`dijkstras.cc:19`) — both live in the current
  `valhalla.json`. The RSS/box-budget surface for xcand; rightsizing 4M frees
  resident memory without a rebuild.
- **Lever #7 gate — do prod route requests set `prefer_elevation>0`?** **No.** The
  orchestrator does not pass `prefer_elevation` to the round-trip action; it
  fetches an elevation *profile* via a separate skadi `/height` call
  (`handlers.rs:463`) on Balkans tiles that "lack elevation coverage" →
  zero-filled. So the drop-elevation tile lever is **ungated** by routing.
  _Caveat:_ loopqual's costing does set `prefer_elevation: 0.3` (`runner.py:113`),
  a prod discrepancy — but a **no-op on elevation-less tiles**, so it does not
  distort these numbers.

## Caveats

- Percentile non-additivity: per-stage p50s do not sum to total p50 (each is a
  percentile over a different request set). Means reconcile: Σstage means 599 +
  framing 89 ≈ total 678.
- `framing` is a residual (total − Σstages), clamped ≥0; it lumps loki, odin,
  serialize, HTTP — split it before treating it as a single target (ticket 4).
- workers=1 / no-way ⇒ clean stage composition but understated contended ratio;
  the 1.10× ratchet gate remains a contended (`--workers 3`) measurement.
- Numbers are Balkans-tile / warm-cache specific; ratios travel, absolutes don't.

# (b2) curvature rescale on the 3.9.1 base

_Results, 2026-11-04. Branch `feat/b2-curvature-rescale` (from `rebase/3.9.1` @ `9c379e75c`). Question: does the "(b2)" change proposed in `2026-11-04-upstream-6449-use-curvature.md` (branch `research/upstream-6449-use-curvature`, §5–§6) earn a cutover together with the 3.9.1 rebase, under one orchestrator FINGERPRINT bump (5 → 6)? Measurement build: branch `research/b2-variants` (a request switch for the variants, an A\* label meter, the distance knobs; not for merging)._

## TL;DR

1. **What ships.** Every motorcycle transition cost is multiplied by `m / m_up`, the smallest factor `EdgeCost` can emit over the smallest factor upstream's own terms can emit: 0.61 at c0.5, 0.47 at c0.7, 0.24 at c1.0 (1 with the fork's discounts off). The fixed seconds (turn costs, `maneuver_penalty`, the continuity penalty) weigh against the roads what they weigh in upstream's costing again. Plus a ranking tier, `thor.roundtrip_disterr_tier` = 0.3: a loop more than 30 % off the asked distance ranks after every loop within it.
2. **Why this form.** Three forms were run on the full corpus-v2. Dividing the edges (the research's mode 4) and multiplying the transitions by the same `m` build the same routes (1.6 % of loops differ), but the edge form costs +23 % A\* labels and the transition form +13 %. Scaling by `m / m_up` instead of `m` is better on c1.0 curviness, distinctness and distance error (behind only on advisory or far-inside-bar meters) and cheaper again (+10 %). See §2.
3. **The c0.7 distance error.** It is a whole-bank effect, not the served surface's. The pair pass predicts a return from harvest-tree arcs, some of which cannot be ridden in reverse. The full repair's A\* then comes back far longer (a 16 km turnaround returning in 38 km), and the built score divides curviness by only (1 + error), so such a loop could outrank an on-target one. The tier fixes what riders read at no cost. A harder fix (a distance gate) also cuts the K = 3 error but costs fills and builds, and is parked. See §3.
4. **Against the rebased engine (= prod):** Gate v2 14/15 blocking rows pass (the 15th, T5, is unreadable without the census baseline and fails for 3.9.1 alone too). c1.0 curviness 1.133×, c0.5 1.009×, c0.7 0.960×; Retrace 2.0 → 0.2 %; T3 1.072×; A\* labels +10 %; round-trip latency +7–11 %; peak RSS equal. The Shuffle harness is neutral: repeats 7.6 → 7.8 %, fills 100 %, distance-error p90 0.218 → 0.221. Zlatibor is over the 30 % budget before and after (0.318 → 0.326). §4.
5. **Recommendation: cut over 3.9.1 + (b2) together, one FINGERPRINT bump (5 → 6).** §6.

## 1. The change

The fork's `prefer_curvature` adds `2c · kForkCurvatureDiscount[curvature]` (0 … −0.30) to the edge factor, and `prefer_elevation` subtracts up to 0.027. Upstream's own terms never take the factor below `kDensityFactor[0]` = 0.85 (0.82 with `use_trails` 0.8). The fork's reach 0.52 / 0.40 / 0.20 at c0.5 / 0.7 / 1.0. Transition costs are seconds and are not discounted, so against the roads they weigh 1/m times what upstream designed them for. That is the research's finding: the curvature gain of "penalise straight" came from the transitions regaining their weight, not from admissibility.

Two ways to give the weight back, equal relative to the roads:

| form | edge cost | transition cost | A\* heuristic, bidirectional threshold, bucket width |
|---|---|---|---|
| edges ÷ k | × 1/k | unchanged | unchanged in seconds, so relatively k× weaker: more search |
| transitions × k | unchanged | × k | unchanged relative to the edges: no extra search from the form itself |

And two choices of `k`:

- `m`: the smallest factor the costing can emit. Every edge then costs ≥ 1 × its time in transition units.
- `m / m_up`: `m` over the smallest factor without the fork's discounts. The fork's discounts then never take an edge below upstream's own floor in transition units, and with the discounts off nothing changes (plain motorcycle routing stays byte-identical to upstream).

Implementation (`src/sif/motorcyclecost.cc`): `MotorcycleCost::SmallestEdgeFactor(bool with_fork_discounts)` takes every additive term of `EdgeCost`'s factor at its minimum (density 0, the most favoured class, surface and toll, the full curvature and grade discounts) times the smallest use multiplier. `EdgeFactor()`'s leash, rejoin and cross-candidate multipliers are all ≥ 1 and `SpeedPenalty` is ≥ 0, so they can only raise it. The constructor sets `transition_scale_ = clamp(m / m_up, 0.1, 1)`, and `TransitionCost` / `TransitionCostReverse` multiply their cost (not their seconds) by it. ETAs do not move.

## 2. Choosing the form

Rig: arm64, `valhalla_service` Release built in `valhalla391-deps:local`; rig Serbia tiles (the orchestrator's `data/`, `tileset_last_modified` 1785932567, the tiles the rebase was measured on); production `thor` config (no `roundtrip_*` key, so the v4 defaults) plus `roundtrip_stage_timing`; each engine pinned to 2 CPUs with `server_threads` 2. Corpus: loopqual corpus-v2, all three blocks, 552 requests at K = 12, two workers. Baseline: `rebase/3.9.1` (the measurement build at `curvature_rescale` 0 is byte-identical to it on 552/552 answers, and to the rebase's own run `rb-391`).

The three forms ran side by side on a VM that another session was also loading, so their wall times are not comparable. Search effort comes from the A\* label meter instead (forward + reverse labels per return-leg search, summed per request). It is deterministic: two passes of the same form give identical counts and byte-identical answers.

| Gate v2 meter (blocks A + B unless noted) | base | edges ÷ m | transitions × m | **transitions × m/m_up** |
|---|---|---|---|---|
| R1 Retrace family, served | 2.0 % | 0.3 % | 0.3 % | **0.2 %** |
| R3 c0.5 dist err mean / p90 (whole bank, all blocks) | 0.174 / 0.372 | 0.178 / 0.370 | 0.178 / 0.370 | **0.173 / 0.348** |
| R3 c0.7 | 0.127 / 0.231 | 0.175 / 0.311 | 0.175 / 0.311 | **0.170 / 0.256** |
| R3 c1.0 | 0.135 / 0.226 | 0.144 / 0.269 | 0.144 / 0.269 | **0.138 / 0.255** |
| T1 curviness c0.5 (°/km) | 298.8 | 302.6 (1.013×) | 302.7 (1.013×) | **301.5 (1.009×)** |
| T1 c0.7 | 245.8 | 236.7 (0.963×) | 236.7 (0.963×) | **236.0 (0.960×)** |
| T1 c1.0 | 253.8 | 276.7 (1.090×) | 276.8 (1.090×) | **287.6 (1.133×)** |
| T2 served overlap mean / near-dup | 0.3367 / 13.8 % | 0.3426 / 15.4 % | 0.3428 / 15.5 % | **0.3274 / 13.9 %** |
| T5 near-mirror metres (served) | 36 m | 12 m | 12 m | **25 m** |
| T6 deep-bank Retrace | 2.6 % | 1.0 % | 1.0 % | **1.5 %** |
| A3 rings, share ≥ 1 | 37.5 % | 32.6 % | 32.6 % | **34.3 %** |
| A4 self-crossings, share ≥ 1 | 45.3 % | 38.1 % | 38.1 % | **37.5 %** |
| A\* labels, corpus total | 942.6 M | 1 160 M (+23 %) | 1 062 M (+13 %) | **1 035 M (+10 %)** |
| A\* labels / request p50 / p90 | 1.45 M / 3.41 M | 1.80 M / 4.47 M | 1.70 M / 3.71 M | **1.74 M / 3.55 M** |

- **Edges vs transitions, same `m`:** the same routes (6 521 of 6 624 loops identical; the rest are the bidirectional A\*'s approximation, since `threshold_delta` is in seconds). Every quality meter is equal to the third digit, and the transition form spends 8 % fewer labels in total and 17 % fewer at p90. Dividing the edges buys nothing.
- **`m` vs `m / m_up`:** the fork-relative scale gives the transitions back only what the fork's discount took. It is curvier at c1.0 (1.133× against 1.090×), distinct where the full scale is not (T2 0.327 against 0.343, below base), and closer to the asked distance at every level. It is also the cheapest. The full scale is ahead only on near-mirror metres, deep-bank Retrace and rings, all far inside their bars or advisory.
- **c0.7 loses curviness in every form** (0.960–0.963×, inside T1's 0.95× bar). The c0.7 cells are Vračar 25–60 km and the Belgrade demand cells at 100 km only, so this is a Belgrade read; why c0.7 there loses while c0.5 and c1.0 gain was not traced. It is the one quality meter that moves the wrong way.
- **Effort:** the +10 % labels of the chosen form are not search inefficiency (the heuristic is unchanged relative to the edges). They come from the different loops it builds: more turns, more rescue builds (2 → 4 % of served loops).

The chosen form is **transitions × m / m_up**. Measured in transition units, every edge it emits costs at least upstream's own floor: the "factor ≥ 1" property read against upstream's costing rather than against 1, which a plain motorcycle request never satisfied either (0.85 at density 0). The gurka test `MotorcycleCurvatureRescale` pins it.

## 3. The c0.7 distance error

Base → (b2) on the whole bank (R3, all 12 slots): c0.7 mean 0.127 → 0.170. Two findings.

**Where it comes from.** It is not spread evenly. On the research's own runs (`a3ea836af`, blocks A + B) the served loops beyond +30 % went from 0 to 33 at c0.7, almost all Vračar 30–50 km. Tracing one (Vračar 30 km c0.7 seed 7, ledger lines `pair-loop` and `pair-ranks`): the pair pass picks a turnaround 16.1 km out with a predicted 18.8 km return (34.9 km, inside the pair band), but 41 of the return's tree arcs have no legal reverse edge, so the loop is a full repair. The repair's A\* return under the corridor exclusions comes back in 38.3 km, and the built loop is 54.4 km for a 30 km ask (+81 %). Two more turnarounds on the same road give 60.2 km loops. Under the base costing the pair pass does not pick those turnarounds. (b2) lets the harvest tree turn onto different roads (that is its point), and in the urban east of Belgrade some of those trees use one-way and dual-carriageway arcs that cannot be ridden home.

The ranking then serves them. The built score is `curviness / ((1 + 4·overlap) (1 + dist_err))`: an 81 % overshoot only halves a loop's score. A curvy 54 km loop (score 57) outranked a flatter 34 km one (score 43) and took slot 3.

**The cheap fix: a distance-error tier.** Within a rung tier, a loop more than `thor.roundtrip_disterr_tier` (0.3) off the asked distance ranks after every loop within it. It only reorders the bank, so it costs no search and no fills, and it cannot move the whole-bank meter R3. It moves what riders read:

| served surface (slots 0–5, blocks A + B) | c0.5 mean / p90 / share > 0.3 | c0.7 | c1.0 |
|---|---|---|---|
| base | 0.106 / 0.188 / 2.3 % | 0.096 / 0.187 / 0.0 % | 0.108 / 0.188 / 0.8 % |
| (b2) | 0.109 / 0.187 / 2.6 % | 0.112 / 0.193 / 2.3 % | 0.103 / 0.187 / 0.3 % |
| **(b2) + tier 0.3** | **0.103 / 0.186 / 0.0 %** | **0.103 / 0.186 / 0.0 %** | **0.103 / 0.183 / 0.0 %** |

Curviness, distinctness and every other meter are unchanged by the tier (T1 1.009× / 0.960× / 1.133×, T2 0.3294). It also removes the base's own served overshoots (c0.5 2.3 % → 0). A `roundtrip_rank_disterr_w` of 3 instead of 1 was also run: it leaves 1.8 % / 1.0 % / 0.2 % of served loops beyond 0.3, so the tier is the stronger and simpler lever.

**A harder fix, parked: a distance gate.** `thor.roundtrip_disterr_gate` (measurement build only) stashes a loop still beyond 0.3 after the distance correction with the gated loops, refills its slot from the queue, and serves it only to top up an under-filled bank. It is the only lever here that reaches K = 3 serves, where the builder stops at three loops and a tier has nothing to reorder. Shuffle harness distance error p90 0.221 → 0.200, Zlatibor 0.326 → 0.254, Subotica 0.284 → 0.229, fills 100 %. But at K = 12 it costs fills (R4 552 → 540/552), barely moves R3 c0.7 (0.170 → 0.168), and fires up to 13 extra builds on one request. Not cheap, so not in this change. It is the lever to take if the K = 3 distance budget needs to tighten.

## 4. The cutover read: `rebase/3.9.1` → `feat/b2-curvature-rescale`

Same rig as §2, run on a quiet VM, one corpus at a time: base bracket, candidate, base bracket. Base = `rebase/3.9.1` built from `9c379e75c`, byte-identical to the rebase's own `rb-391` run (552/552) and to itself across the two brackets. Candidate = this branch's head, byte-identical (552/552) to the measurement build's transitions × m/m_up + tier 0.3 run, so §2 and §3 describe the binary that ships. The baseline is therefore prod's engine on prod's config: Baseline v2 (the census run `b4f514d7f`) is not on this machine, and the 3.9.1 rebase measured equal to prod (`a3ea836af`) on every Gate meter.

### 4.1 Gate v2 (corpus-v2, all blocks, K = 12)

| tier | row | bar | base | (b2) | verdict |
|---|---|---|---|---|---|
| reject | R1 Retrace family, served | ≤ 5 % | 2.0 % | **0.2 %** | PASS |
| reject | R2 exact-mirror seam stub | 0 | 0 | 0 | PASS |
| reject | R3 c0.5 distance error mean / p90 (whole bank) | ≤ 0.22 / 0.42 | 0.174 / 0.372 | 0.173 / 0.348 | PASS |
| reject | R3 c0.7 | ≤ 0.32 / 0.65 | 0.127 / 0.231 | **0.170 / 0.256** | PASS |
| reject | R3 c1.0 | ≤ 0.32 / 0.65 | 0.135 / 0.226 | 0.138 / 0.255 | PASS |
| reject | R4 fills | 552/552 | 552/552 | 552/552 | PASS |
| reject | R5 failed requests | 0 | 0 | 0 | PASS |
| ratchet | T1 curviness c0.5 (°/km) | ≥ 0.95× | 298.8 | 301.5 (1.009×) | PASS |
| ratchet | T1 c0.7 | ≥ 0.95× | 245.8 | **236.0 (0.960×)** | PASS |
| ratchet | T1 c1.0 | ≥ 0.95× | 253.8 | **287.6 (1.133×)** | PASS |
| ratchet | T2 served overlap mean / near-dup > 0.6 | ≤ 0.3704 / ≤ 18.8 % | 0.3367 / 13.8 % | 0.3294 / 14.2 % | PASS |
| ratchet | T3 wall p50, pooled over 2 same-session brackets | ≤ 1.10× | 0.907 s | 0.972 s (1.072×) | PASS |
| ratchet | T4 wall p95, blocks A + B | ≤ 1.30× | 2.312 s | 2.494 s (1.079×) | PASS |
| ratchet | T5 near-mirror metres, served | ≤ 0.25× of Baseline v2 | 36 m | 26 m | not readable* |
| ratchet | T6 deep-bank Retrace | ≤ 2.6 % | 2.6 % | 1.5 % | PASS |
| canary | C1 spike ≥ 30 m / C2 edge reuse | report | 0.0 % / 0 | 0.0 % / 0 | — |
| advisory | A1 Bank Distinctness | report | 0.4441 / 24.5 % | 0.4360 / 25.2 % | — |
| advisory | A2 per-leg overlap, forward / return | report | 0.234 / 0.210 | 0.245 / 0.192 | — |
| advisory | A3 rings: share ≥ 1 / mean largest | report | 37.5 % / 2 579 m | 34.3 % / 2 800 m | — |
| advisory | A4 self-crossings | report | 45.3 % | 37.5 % | — |
| advisory | A6 shadow fraction | report | 0.0088 | 0.0085 | — |
| advisory | A7 block B Retrace / D1b ≥ 500 m | report | 3.0 % / 6.2 % | 0.3 % / 3.9 % | — |
| advisory | A7 block C (motorways avoided, v1 corpus) | report | 19.0 % / 0.6 % | 19.6 % / 0.0 % | — |
| advisory | A8 c0.8 curviness / Retrace | report | 332.1 / 20.3 % | 346.6 (1.044×) / 20.8 % | — |
| advisory | A9 long asks (≥ 200 km) wall p95 | report | 4.471 s | 3.854 s | — |
| advisory | A10 builder mix, served | report | pair 98 %, rescue 2 % | pair 96 %, rescue 4 % | — |

\* T5's bar is a quarter of the census Baseline v2's near-mirror metres (36 m), which this base already sits at, so it reads FAIL for any candidate including the 3.9.1 rebase itself (ADR-0041's bar needs the census run). (b2) moves it the right way, 36 → 26 m. **14 of 15 blocking rows pass; the 15th is that unreadable row.**

The same-session wall-time drift was 13 % (bracket fire times 339 s and 296 s), which is the ±12 % ADR-0041 warns about; T3 is read against the pooled brackets as the gate prescribes. Search effort, counted directly (§2): +10 % A\* labels in total, +20 % per request at p50 and +4 % at p90.

### 4.2 Shuffle harness (6 cities × 30/60/120 km × c0.5/1.0, 6 consecutive K = 3 Shuffles, `exclude_turnarounds` at 0.08 × distance)

| | repeats (≥ 0.9 cover) | fill | dist err mean / p90 | rescue / fallback |
|---|---|---|---|---|
| base | 41/540 = 7.6 % | 648/648 (216/216 full serves) | 0.140 / 0.218 | 3.7 % / 0.5 % |
| (b2) | 42/540 = 7.8 % | 648/648 (216/216) | 0.136 / 0.221 | 7.9 % / 1.1 % |

| city | repeats base → (b2) | dist err p90 base → (b2) |
|---|---|---|
| Belgrade (Vračar) | 5.6 → 4.4 % | 0.224 → 0.216 |
| Novi Sad | 10.0 → 11.1 % | 0.223 → 0.191 |
| Niš | 2.2 → 4.4 % | 0.188 → 0.191 |
| Kragujevac | 4.4 → 3.3 % | 0.204 → 0.200 |
| **Zlatibor** | 10.0 → 7.8 % | **0.318 → 0.326** |
| **Subotica** | 13.3 → 15.6 % | **0.276 → 0.284** |

At K = 3 (b2) is neutral: one more repeat in 540, every serve full, distance error flat. **Zlatibor is over the owner's 30 % p90 budget before and after** (0.318 → 0.326); Subotica stays under it (0.276 → 0.284). The tier cannot help here (three loops built, three served). The parked distance gate (§3) is what brings Zlatibor to 0.254 and Subotica to 0.229, at its cost.

### 4.3 Latency, memory and the other endpoints

Paired replay of the rebase's endpoint corpus (165 requests: round trip K = 3 / K = 12 / excluded / motorways avoided, one-way with alternates and with heading, OSRM navigate, `trace_route`, `/height`), three repetitions, base and candidate interleaved request by request with the order alternating, one request in flight:

| slice | n | p50 base → (b2) | p90 base → (b2) | paired ratio median / p90 |
|---|---|---|---|---|
| round trip | 234 | 215 → 241 ms | 751 → 835 ms | 1.010 / 1.241 |
| one-way + alternates | 60 | 32 → 32 ms | 42 → 43 ms | 0.991 / 1.027 |
| one-way with heading | 15 | 19 → 19 ms | 30 → 29 ms | 0.979 / 1.033 |
| OSRM navigate | 18 | 31 → 30 ms | 38 → 37 ms | 0.996 / 1.039 |
| `trace_route` | 84 | 19 → 19 ms | 26 → 25 ms | 0.990 / 1.028 |
| `/height` | 84 | 4 → 3 ms | 16 → 4 ms | 0.895 / 1.196 |

Round trips cost about +11 % at p50 and p90 by the marginal percentiles, consistent with the +10 % A\* labels and with T3's 1.072×. The paired median is ~1: half the round trips are not slower at all, the tail of the ratio is. Nothing else moves.

**Peak RSS** (`VmHWM` of `valhalla_service`, the same two processes after the three Gate runs, the Shuffle runs and the replay): base 1 742 MB, (b2) 1 740 MB. Equal.

**Output on the other endpoints:** `/height`, `trace_route` and OSRM navigate (no `prefer_curvature`) are byte-identical. One-way routes with the curvy costing change (25/25 answers, 30 of 64 routes, mean length change 0.2–1.8 % by slice), as every motorcycle route with the fork's discounts on must.

## 5. gurka

The relevant suites (the rebase's set: 20 gurka targets plus `curvature_metric` and `cost_inline_tests`) built and run on `rebase/3.9.1` and on this branch:

| | before | after |
|---|---|---|
| `gurka_motorcycle_roundtrip`, `roundtrip_exclude`, `roundtrip_audit`, `roundtrip_distinctness` | 22 / 6 / 28 / 3 pass | 22 / 6 / 28 / 3 pass |
| `gurka_motorcycle_curvature` | 3 pass | **4 pass** (new `MotorcycleCurvatureRescale`) |
| `motorcycle_continuity`, `_elevation`, `_reuse` | 1 / 1 / 2 pass | 1 / 1 / 2 pass |
| `destination_only`, `conditional_restrictions`, `time_dependent_restrictions`, `simple_restrictions`, `only_restrictions`, `linear_feature_factors`, `access`, `avoids`, `route`, `alternates`, `bidir_search`, `search_filter` | all pass | all pass |
| `curvature_metric`, `cost_inline_tests` | 1 / 7 pass | 1 / 7 pass |

The new test run against the base fails as intended: a curvature-15 edge costs 0.55 / 0.43 / 0.25 per second at c0.5 / 0.7 / 1.0 against upstream's 0.85. After: 0.90 / 0.91 / 1.05 in transition units (scale 0.61 / 0.47 / 0.24).

## 6. Recommendation

**Cut over 3.9.1 and (b2) together, under the one FINGERPRINT bump (5 → 6).**

- Gate v2 passes on every blocking row it can read: 14 of 15, with T5 unreadable without the census baseline and failing identically for 3.9.1 alone. T3 is 1.072× against a 1.10× bar.
- The gains are real and broad: c1.0 curviness +13 %, Retrace 2.0 → 0.2 % served and 2.6 → 1.5 % deep, rings −3 points, self-crossings −8 points, near-mirror metres 36 → 26, and served distinctness slightly better (T2 0.337 → 0.329).
- Shipping (b2) on its own later costs a second bump (6 → 7) and a second cache flush for the same Gate run. 3.9.1 alone measured equal to prod, so it has nothing to show riders. Bundling is the cheaper path.

What the owner accepts with it:

1. **c0.7 curviness −4 %** (0.960×, bar 0.95×). A Belgrade-only read (the corpus has c0.7 only there) and not traced.
2. **Round-trip latency +7 % (T3 p50) to +11 % (paired replay)**, from +10 % A\* labels. Inside the ratchet, but most of its headroom on the 2-vCPU box.
3. **The deep bank's distance error at c0.7** (R3 0.127 → 0.170 mean, inside the 0.32 bar). The served surface is held at base by the tier.
4. **Zlatibor's K = 3 distance-error p90 is over the 30 % budget, before (0.318) and after (0.326).** (b2) does not cause that and the tier cannot fix it. The parked distance gate (§3) would (0.254), at a fill and build cost. Worth its own ticket if the budget is binding.

Not decided here: whether to keep the edge form's literal "factor ≥ 1". It was measured, and it is worse on quality and dearer on search.

## Process notes

The variant runs in §2–§3 shared the VM with another session's engines. One measurement engine was killed by the VM's OOM killer mid-run, and a few long requests timed out at the client. Every affected request was re-fired on a fresh engine (answers are deterministic), and each run used here is complete: 552/552 answers at 12 routes, except the parked gate's 12 genuine under-fills, which reproduce byte-identically. No wall time from that window is used. The §4 runs were sequential on a quiet VM with at most two engines.

## Reproduce

```bash
# build valhalla_service in valhalla391-deps:local (Release); serve on the rig tiles, 2 CPUs, server_threads 2,
# config = the rig valhalla.json + thor.roundtrip_stage_timing (labels come from the research/b2-variants build)
# variants (research/b2-variants): costing option curvature_rescale 0 | 1 | 2 | 3; thor.roundtrip_disterr_tier / _gate
LOOPQUAL_COSTING_EXTRA='{"curvature_rescale": 3}' tools/loopqual/loopqual run --engine http://localhost:<port> \
    --corpus corpus-v2.yaml --out results/<run> --workers 2 --no-way
python3 tools/loopqual/gate_v2.py results/<base-1> results/<cand> --bracket results/<base-1> --bracket results/<base-2>
# Shuffle harness: research/shuffle-variety's exclude bench, exclusion radius 0.08 x distance, 6 cities x 30/60/120 km x c0.5/1.0
# A* labels: the stage ledger's astar_labels / astar_fb_labels per request (research/b2-variants)
```

## Sources

- `docs/curvagen/research/2026-11-04-upstream-6449-use-curvature.md` (branch `research/upstream-6449-use-curvature`): (b2) as proposed, mode 4, and the untested transition sibling.
- `src/sif/motorcyclecost.cc` (`SmallestEdgeFactor`, constructor, `TransitionCost`, `TransitionCostReverse`); `valhalla/sif/dynamiccost.h` (`kDensityFactor`, `EdgeFactor`, `SpeedPenalty`); `src/thor/route_action.cc` (`score_built`, the ranking sort, the pair build's full repair, `route_leg`).
- `tools/loopqual/gate_v2.py` (ADR-0041 as amended); `docs/curvagen/extension-api.md` (FINGERPRINT rule).

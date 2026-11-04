# Upstream #6449 `use_curvature` against the fork's curvature costing

_Research, 2026-11-04. Question: what can we take from upstream Valhalla PR [#6449](https://github.com/valhalla/valhalla/pull/6449) (motorcycle `use_curvature`) to improve our curvature routing, or to replace parts of the fork's implementation?_
_Sources: the PR's description, diff (head `ed4053607`, 2026-11-03), review and comments, read with `gh pr view|diff 6449 -R valhalla/valhalla` and `gh api repos/valhalla/valhalla/pulls/6449/comments`; the fork at `a3ea836af` (`src/sif/motorcyclecost.cc`, `src/mjolnir/util.cc`, `valhalla/sif/dynamiccost.h`, `src/thor/route_action.cc`); the fork's own research (`2026-09-05-v3-correctness-optimality-audit.md` F03/F22, `2026-11-04-upstream-since-fork.md` on `research/upstream-since-fork`, `2026-11-04-shuffle-variety-levers.md` on `research/shuffle-variety`); the orchestrator's `crates/domain/src/costing.rs`. Measurements: a prototype engine built from this branch, run on the Serbia rig tiles (§4)._

## TL;DR

1. **#6449 is a different objective, not a better search.** Its curvature table multiplies cost by up to 7× for straight rural roads and up to 14× for any urban road, and never goes below 1. "Penalise straight" and "discount curvy, then lower the A\* factor" find the same paths. They differ only in how heavy the seconds-denominated transition costs are next to the edges (§2.2).
2. **It runs on our tiles as they are.** It reads `DirectedEdge::curvature()`, which our mjolnir redefines (turning density instead of averaged radius). A census of 25 722 rig edges shows the two metrics agree on edges ≥ 200 m (r 0.95–0.98) and diverge on short ones (r 0.66), and that #6449's table prices our buckets within 2 % of upstream's. No tile rebuild, no metric swap (§3).
3. **Adopting it (a) fails Gate v2, measured.** On blocks A + B (K = 12, 320 requests, mode 0 as the baseline) it raises curviness +5 % / −3 % / +19 % at c0.5 / 0.7 / 1.0, removes retraces (2.0 → 0.0 %) and cuts rings by 5 points. But it **fails T2** (served overlap 0.337 → 0.378) and **T3** (1.11×), and the c0.7 distance error goes 0.127 → 0.190. On the six-city Shuffle corpus: c1.0 +3 %, c0.5 −1 %, repeats 6.1 → 5.9 %, fallbacks 0.3 → 1.7 % (§4).
4. **Borrowing admissibility alone (b1) is worthless.** An admissible A\* factor moves no Gate meter (258/320 requests identical, T1 1.00×) and costs +9–21 % A\* labels per search. Of 1 255 comparable return legs, 12 change cost, 5 cheaper and 7 dearer, net +0.01 %. Close F03 as measured.
5. **Borrowing the *form* (b2) is the find.** Our own weights divided by their smallest factor (mode 4) give curviness **+10 % / 0 % / +19 %**, retraces 2.0 → 0.0 %, rings −4 points and self-crossings −7 points, and **pass T2** (0.349). T3 is on the bar (1.096×) and the c0.7 distance error rises (0.127 → 0.183). The gain comes from transitions regaining weight, not from admissibility.
6. **Recommendation: keep `prefer_curvature` and clear the two clashes now** (proto field 100 is already moved on `rebase/proto-private-range`; rename `kCurvatureFactor` → `kForkCurvatureDiscount`). Both are output-neutral, so no FINGERPRINT bump. **Take (b2) to a full Gate v2 after the 3.9.1 rebase**: engine-only, `costing.rs` unchanged, FINGERPRINT bump when it ships. Do not adopt #6449 while it is open. A draft comment with our data on the reviewer's open question is in §7, for the owner to post if wanted.

## 1. What #6449 is

Maintainer-authored (opened 2026-10-30 by a Valhalla member, building on a 2025 contributor branch), **open**, one maintainer "lgtm" review, no linked issues. It adds one costing option, `use_curvature`, read for every costing in `ParseBaseCostOptions` but acted on only in `MotorcycleCost::EdgeCost`.

| | #6449 (`valhalla/sif/dynamiccost.h`, `src/sif/motorcyclecost.cc`) |
|---|---|
| Option | `use_curvature` ∈ [0, 1], default **0.5 = neutral**. Below 0.5 has no effect ("reserved for a straight road preference"). Proto `Costing.Options` field **100**. |
| Quantisation | 21 steps of 0.05: `step = round(use_curvature · 20)`; `strength = ((step − 10) / 10)²` for step > 10, else 0. |
| Table | `kCurvatureFactor[step][c] = 1 + 6 · strength · ((15 − c) / 15)²` for edge curvature `c` = `DirectedEdge::curvature()` 0..15. Range **[1, 7]**: a straight edge costs up to 7×, a curvature-15 edge 1×. Built `constexpr` (21 × 16 floats). |
| Density gate | `edge->density() < 3` (rural): `factor *= table[step][c]`. `3 ≤ density < 7` (suburban): `factor *= (1 + 0.1·strength) · table[step][c]`. `density ≥ 7` (urban): `factor *= 2 · table[step][0]`, i.e. **up to 14× regardless of curvature**. |
| What counts as "curvy" | Only `use == kRoad`, not a link, not a roundabout, not internal. Everything else (ramps, tracks, service roads, roundabouts, turn channels) is priced as **curvature 0 = full straight penalty**, so curvature can never make those edges cheap. |
| Where it multiplies | After `EdgeFactor()`, on the whole factor (density, highway, surface, speed penalty, toll, linear cost factors). **Cost only**: `Cost(cost · factor, sec)`; time and ETA unchanged. No `TransitionCost` change. |
| A\* heuristic | `AStarCostFactor()` untouched. Every multiplier is ≥ 1, so the option adds no inadmissibility (the "inverted avoidance" in the PR text). |
| Companions the PR recommends | lower `use_highways`; higher `maneuver_penalty` ("avoid lots of turns only for the sake of curvy roads"); widening `hierarchy_limits` on level 1, because with default limits long routes stay on levels 0/1 and the curvy secondary/tertiary roads are never reached. It removed its own motorway avoidance before review. Depends in spirit on [#6425](https://github.com/valhalla/valhalla/pull/6425) for a better rural/urban density. |
| Review | nilsnolde: "we should look into other factors which could be inverted, it's much nicer to the heuristic … not sure there aren't also any downsides … e.g. in the priority queue due to much higher cost ranges?" The author notes the precomputed table now only saves a multiply. |
| Tests | gurka `UseCurvatureTest` (a straight `primary` vs a curvy `primary` between the same nodes, with and without speed differences) plus an inline table-monotonicity test. |

## 2. The two designs side by side

The fork's curvature costing (all in `src/sif/motorcyclecost.cc` at `a3ea836af`):

- `prefer_curvature` c (0..1) → `curvature_factor_ = 2c`, **added** to the factor: `factor = kDensityFactor[d] + highway·kHighwayFactor + surface·kSurfaceFactor + 2c·kCurvatureFactor[b]` with `kCurvatureFactor[b] = −0.02·b` (0 … −0.30). A curvy edge is **discounted**, linearly in the bucket.
- `prefer_elevation` subtracts up to `0.3 · 9 · 0.01 = 0.027` for graded edges.
- `curviness_continuity` adds `8c` seconds in `TransitionCost` when a curvy (≥ 9) edge is left for a non-curvy one.
- `reuse_penalty` and the round-trip's rejoin / cross-candidate factors multiply in `EdgeFactor()` and are all ≥ 1.

The orchestrator (`costing.rs`) sends `prefer_curvature = curviness_continuity = c`, `use_highways = max(0.1, 1 − 0.7c)` (motorways allowed), `use_trails = min(0.8, 0.8c)`, `top_speed = 120 − 40c`, `maneuver_penalty = 10 − 10c`, `reuse_penalty 0.8`, `prefer_elevation 0.3`.

### 2.1 What each one pays for a road (rural, density 1, paved, no class penalty)

The prototype maps the rider's c onto #6449 as `use_curvature = 0.5 + 0.5c` (§4.1).

| factor on edge time | bucket 0 | bucket 5 | bucket 9 | bucket 15 | straight ÷ curvy (b0/b15) | b0/b9 | urban (density 8) |
|---|---|---|---|---|---|---|---|
| fork, c0.5 | 0.875 | 0.775 | 0.695 | 0.575 | 1.52 | 1.26 | ~1.05, still discounted by curvature |
| #6449, use_curvature 0.75 | 2.19 | 1.46 | 1.09 | 0.875 | 2.50 | 2.02 | 5.25 flat |
| fork, c1.0 | 0.875 | 0.675 | 0.515 | 0.275 | 3.18 | 1.70 | ~1.05, still discounted |
| #6449, use_curvature 1.0 | 6.13 | 3.21 | 1.72 | 0.875 | 7.00 | 3.57 | 14.7 flat |

- **#6449 rewards curvature much harder**, and convexly: the step from bucket 0 to 5 is worth more than 5 to 15. The fork's discount is linear. At c0.5 #6449's straight-to-curvy ratio is already above the fork's c1.0 at the middle buckets.
- **#6449 never lets a city street win on curvature.** Above density 7 it is a flat surcharge; the fork discounts a curvy urban street exactly like a rural one. That is the mechanism behind F22 (urban block rings: a curvature-15 ring is 2–3× cheaper under the fork's discount than upstream's base costing).
- **#6449 over-rewards curvy short connectors less** (links, roundabouts, tracks and service roads get the straight price) but **under-rewards** curvy tracks for a rider who asked for `use_trails` (our c ≥ 0.63 sends `use_trails ≥ 0.5`): a curvy gravel road pays the full 7×.
- **Neither touches time**, so neither moves ETAs; both are pure preference.

### 2.2 Admissibility

`AStarCostFactor()` is `kSpeedFactor[top_speed] · min_linear_cost_factor` in both. The true cost per metre is `kSpeedFactor[min(v, top_speed)] · factor`, so the heuristic is admissible iff `factor ≥ 1` on every edge that can run at `top_speed`.

| costing (motorways allowed) | smallest factor the fork can emit | worst overestimate |
|---|---|---|
| upstream base (any costing) | 0.85 (`kDensityFactor[0]`) | 1.18× |
| fork c0.5 | 0.520 | 1.92× |
| fork c0.7 | 0.403 | 2.48× |
| fork c1.0 | 0.196 | 5.10× |
| #6449 at any `use_curvature` | 0.85 (the base table; its own multiplier is ≥ 1) | 1.18×, unchanged |

So #6449 is admissible **relative to upstream**, not absolutely: upstream's own density table (and `use_highways`/`use_trails`/`use_tolls` above 0.5) already go below 1. The fork's discount is what multiplies the gap (F03).

The point that matters for us: "penalise straight" and "discount curvy, then lower the heuristic" are **the same search**. Dividing every edge factor by a constant `m` (the smallest factor) does not change which path is cheapest; it only rescales the costs so the old heuristic underestimates them. What the two forms do differ in is everything measured in **seconds outside `EdgeCost`**: `TransitionCost` (turn costs, `maneuver_penalty`, the continuity penalty), the bidirectional A\*'s post-connection threshold, and the bucket width. Under a discount they weigh `1/m` times more against the road costs around them; under a penalty they keep their face value. That is why the prototype has two "borrow" modes (§4.1): mode 1 changes only the heuristic, mode 4 rescales the edges.

And "admissible" is not "faster". A heuristic that never overestimates a cost whose cheapest edge is 0.2 × time is also 5× too weak on a straight road. Both forms buy admissibility with **more** expansions, not fewer (§4.4). That is exactly the reviewer's open question on #6449.

### 2.3 Hierarchy and shortcuts

- **Round-trips do not use shortcuts or hierarchy limits** (F05: the round-trip branch leaves `route()` before `check_hierarchy_limits`, the costing has no `hierarchy_limits`, so `BidirectionalAStar` runs with `ignore_hierarchy_limits_` and skips every shortcut). #6449's advice to widen `hierarchy_limits` is about A→B routing and does not apply to our hot path.
- For A→B routing, a shortcut's curvature is `compute_curvature` over its whole shape (`shortcutbuilder.cc:529`), effectively an average. The fork's **linear** discount prices a shortcut about the same as its base edges. #6449's **quadratic** penalty does not (Jensen): a shortcut that is half bucket 0 and half bucket 15 averages to roughly bucket 7, and pays `1 + 6·(8/15)² = 2.7×` instead of the `(7 + 1)/2 = 4×` its base edges would. Shortcuts get cheaper than the roads they contract.
- `use_highways`: #6449 stacks multiplicatively on our additive highway factor. Motorways are bucket-0 roads, so at c1.0 they already pay 7× on top of `use_highways = 0.3`.

## 3. Data dependency: which curvature #6449 reads

#6449 reads `DirectedEdge::curvature()`, the 4-bit 0..15 value mjolnir writes at build time. Its table was tuned against **upstream's** `compute_curvature`: the average, over consecutive shape-point triples, of `min(25, 1500 / circumradius)` (0 when the radius is above 1 km), truncated. The fork **redefines** that function (`src/mjolnir/util.cc`): Douglas-Peucker at 5 m, then total absolute heading change per km, with 350°/km mapping to 15 (rounded). Same field and range, different meaning, and the rig and prod tiles carry the fork's version.

**Census** (`curvhist.py`, rig tiles, 25 722 distinct road edges from 36 loops and 18 town-to-town routes, recomputing both metrics from each edge's full shape via `/trace_attributes`):

- The tiles **are** the fork's metric: the recomputed fork bucket equals the stored value on **99.4 %** of edges (99.8 % within ±1).
- On real Serbian roads the two metrics **mostly agree**. For edges ≥ 200 m (69 % of km), the length-weighted correlation is r = 0.95–0.98, and 94–99 % of km is within ±2 buckets. The disagreement sits in short edges (< 200 m, 31 % of km): r = 0.66, and 12 % of km is ≥ 5 buckets apart, which is the averaged-radius metric's known sensitivity to how densely a short way is mapped.
- By fork bucket, the mean upstream bucket is 0.9 (b0), 0.3 (b1), 1.6 (b2), 3.5, 4.7, 5.8, 7.3, 8.4, 8.5, 9.6, 10.3, 11.1, 11.8, 11.6, 12.5 and 14.3 (b15). Close to the identity from bucket 3 up, and slightly compressed at the top.
- Consequence: #6449's table, applied to our tiles, costs rural km almost exactly what it would cost on upstream tiles (mean rural factor 2.19 vs 2.16 at `use_curvature` 0.75, 5.75 vs 5.64 at 1.0).

**Can they coexist? Yes, without a tile rebuild.** #6449 runs on our tiles as they are, and its calibration roughly survives the redefinition. Adopting it does **not** require upstream's metric. Going the other way (upstream tiles under the fork's discount) would put noisy short edges into a linear discount, which is milder than #6449's quadratic but still wrong in the same places. The fork's metric is the better input for either formulation.

## 4. Measurements

### 4.1 Prototype

Branch `research/upstream-6449-use-curvature` (this doc's branch), from `a3ea836af`. One costing option, `curvature_mode` (fork-private proto number, JSON key `curvature_mode`, default 0), selects:

| mode | objective | A\* heuristic | reads as |
|---|---|---|---|
| 0 | the fork today | today | baseline |
| 1 | the fork today | × smallest factor (F03's fix shape) | option (b), heuristic only |
| 2 | #6449's `EdgeCost` block verbatim (renamed `kUc*`), `use_curvature = 0.5 + 0.5c`; `prefer_curvature`'s discount off; continuity, elevation, leash, rejoin, xcand unchanged | today | option (a) |
| 3 | as 2 | × smallest factor (closes upstream's own 0.85 too) | (a), strictly admissible |
| 4 | the fork today, every edge factor ÷ the smallest factor | today | option (b), "penalise straight" form of our own weights |

Plus an A\* label counter (`BidirectionalAStar::label_count()`, forward + reverse labels per search, summed per request in the `thor.roundtrip_stage_timing` ledger line together with seed and target). Label counts are deterministic: two runs of mode 0 give identical counts, so they measure expansions free of the latency noise (§4.4).

**Correctness of the baseline.** Mode 0, and requests without the field, are **byte-identical** to the `curvagen-valhalla:latest` image (the v4 serving engine, which `a3ea836af` matches on requests without exclusions per `extension-api.md`): 216/216 serves of the Shuffle corpus in both cases.

**Rig.** arm64, `valhalla_service` Release, built in `valhalla24-deps:local` with this worktree mounted, served on :8030 with `server_threads=2`; tiles = the orchestrator's `data/` (Serbia, `tileset_last_modified` 1785932567), config = its `valhalla.json` plus `thor.roundtrip_stage_timing`. **Latency caveat:** another session ran a loopqual corpus against its own engine on the same 4-vCPU Docker VM for most of the measurement. Expansions therefore come from the label counter, which noise cannot move. The Gate-corpus T3/T4 read (§4.3) comes from a later block of runs when the VM was quiet: mode 4, 2, 1, then mode 0 again, bracketed by two mode-0 runs.

### 4.2 Shuffle corpus (the requested corpus)

Six origins (Belgrade Vračar, Novi Sad, Niš, Kragujevac, Zlatibor, Subotica; city-centre level) × 30/60/120 km × c0.5/1.0, motorways allowed, **6 consecutive K = 3 Shuffles per cell** with the orchestrator's Shuffle memory as it will ship (`exclude_turnarounds` at 0.08 × distance, curvagen-valhalla#28). 216 serves, 648 loops per run. The harness is `research/shuffle-variety`'s, copied to `tools/curvature-6449/` with the costing modes added. The meters are loopqual's, with Gate v2 adapted to K = 3 as in that study.

| mode | repeats ≥ 0.9 | ≥ 0.8 | curviness c0.5 (°/km) | curviness c1.0 | R1 retrace % | dist err mean / p90 | fallback % | rescue % | T2 served overlap mean | near-dup > 0.6 % | budget read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 (today) | 6.1 % | 20.2 % | 290.3 | 299.8 | 3.2 | 0.141 / 0.226 | 0.3 | 4.6 | 0.147 | 2.3 | — |
| 1 (admissible) | 6.1 % | 20.2 % | 290.3 | 299.7 | 3.2 | 0.141 / 0.226 | 0.3 | 4.6 | 0.147 | 2.3 | pass |
| 2 (#6449) | 5.9 % | 16.7 % | 287.8 (−0.8 %) | 308.8 (+3.0 %) | 3.1 | 0.135 / 0.230 | 1.7 | 6.9 | 0.165 (+12 %) | 4.6 | **fail T2** |
| 3 (#6449, admissible) | 5.9 % | 16.7 % | 287.8 | 308.8 | 3.1 | 0.135 / 0.230 | 1.7 | 6.9 | 0.165 | 4.6 | fail T2 |
| 4 (ours ÷ min) | 6.1 % | 18.7 % | 287.7 (−0.9 %) | 310.7 (+3.6 %) | 2.8 | 0.137 / 0.239 | 1.5 | 6.8 | 0.162 (+10 %) | 3.9 | **fail T2** |

Mode 0 run twice: identical on every meter. Byte identity against mode 0: mode 1 210/216 serves, modes 2–4 0/216.

Where the curviness moves (mean °/km per origin, mode 0 → mode 2):

| origin | c0.5 | c1.0 |
|---|---|---|
| Kragujevac | 321 → 337 | 337 → 354 |
| Niš | 298 → 307 | 297 → 315 |
| Belgrade Vračar | 247 → 234 | 283 → 300 |
| Zlatibor | 557 → 537 | 552 → 565 |
| Novi Sad | 202 → 185 | 207 → 195 |
| Subotica | 117 → 127 | 122 → 124 |

#6449's objective gains in the hilly middle of the country (Kragujevac, Niš) and at c1.0 generally. It loses at c0.5 in the flat north and in the city: Novi Sad −8 %, Belgrade c0.5 −5 %, Zlatibor c0.5 −4 %. Mode 4 moves the same way (Kragujevac 340/352, Novi Sad 184/193, Belgrade 227/307). The mean hides two opposite moves.

What the loops ride (`roadmix.py`: the first Shuffle's 108 loops per mode, ~7 800 km, split by the tile's own attributes via `/trace_attributes`):

| mode | bucket 0 | 1–4 | 5–8 | ≥ 9 (curvy) | urban (density ≥ 7) | motorway + trunk | tertiary and below |
|---|---|---|---|---|---|---|---|
| 0 | 51.5 % | 14.6 % | 10.6 % | 23.2 % | 11.2 % | 2.0 % | 43.0 % |
| 2 (#6449) | 50.1 % | 14.9 % | 10.4 % | 24.5 % | 10.4 % | 2.7 % | 48.5 % |
| 4 (ours ÷ min) | 50.3 % | 14.6 % | 10.2 % | 24.9 % | 10.2 % | 2.4 % | 47.5 % |

A 7× straight-road penalty moves only **1.3 points** of loop km onto curvy roads, and half of every loop is still on bucket-0 roads. The round-trip's own constraints decide most of the loop: the distance band, a disjoint way home, the start's road network, the leash. The edge costing works at the margin. The visible shift is from secondary and above onto tertiary and minor roads (+5.5 points), together with slightly less city.

### 4.3 Gate v2 blocks A + B (corpus-v2, K = 12)

Gate v2 judges the **served surface** (slots 0–5) of corpus-v2's blocks A (the Vračar field cell) and B (the production demand cells): 320 requests at K = 12, 1 920 served loops, motorways allowed except the `vracar_amw` control. `corpus_ab.py` derives that corpus from `corpus-v2.yaml`, and `gate_v2.py` reads it unchanged. **Baseline v2** (the census run on `b4f514d7f`) is not on this machine, so mode 0 stands in as the baseline. That makes this a **relative** read of each mode against today. The absolute reject tier (R1–R5) is read as written. T5 cannot be read: its bar is 0.25× of the *census* near-mirror metres, and against mode 0 it fails even for mode 1, which is 36 m = 36 m. T3/T4 come from a quiet same-session block, pooled over two mode-0 brackets (1.12 s and 0.88 s p50, the ±12 % drift ADR-0041 warns about).

| row | bar | mode 0 | mode 1 (admissible) | mode 2 (#6449) | mode 4 (ours ÷ min) |
|---|---|---|---|---|---|
| R1 Retrace family, served | ≤ 5 % | 2.0 % | 2.0 % | **0.0 %** | **0.0 %** |
| R3 c0.5 dist err mean / p90 | ≤ 0.22 / 0.42 | 0.134 / 0.228 | 0.133 / 0.228 | 0.153 / 0.284 | 0.139 / 0.238 |
| R3 c0.7 dist err mean / p90 | ≤ 0.32 / 0.65 | 0.127 / 0.231 | 0.125 / 0.230 | 0.190 / 0.394 | 0.183 / 0.359 |
| R3 c1.0 dist err mean / p90 | ≤ 0.32 / 0.65 | 0.135 / 0.226 | 0.137 / 0.227 | 0.139 / 0.284 | 0.140 / 0.283 |
| R2, R4, R5 | 0 / 320 / 0 | pass | pass | pass | pass |
| T1 curviness c0.5 (°/km) | ≥ 0.95× | 230.2 | 230.1 (1.000×) | 242.3 (**1.053×**) | 253.0 (**1.099×**) |
| T1 curviness c0.7 | ≥ 0.95× | 245.8 | 245.4 (0.999×) | 238.6 (0.971×) | 244.9 (0.996×) |
| T1 curviness c1.0 | ≥ 0.95× | 253.8 | 254.6 (1.003×) | 303.1 (**1.194×**) | 301.1 (**1.186×**) |
| T2 served distinctness mean / near-dup | ≤ 0.3707 / ≤ 18.8 % | 0.3370 / 13.8 % | 0.3389 / 13.8 % | 0.3776 / 16.8 % **FAIL** | 0.3487 / 12.5 % |
| T3 wall p50 (pooled brackets 1.030 s) | ≤ 1.10× | — | 1.008 s (0.98×) | 1.144 s (1.11×) **FAIL** | 1.129 s (1.096×) |
| T4 wall p95 | ≤ 1.30× | 2.805 s | 0.91× | 0.88× | 0.94× |
| T5 near-mirror metres | (not readable, see above) | 36 m | 36 m | 28 m | 22 m |
| T6 deep-bank Retrace | ≤ 2.6 % | 2.6 % | 2.6 % | 1.7 % | 1.7 % |
| A3 rings: share ≥ 1 ring | advisory | 37.5 % | 38.0 % | 32.8 % | 33.3 % |
| A4 self-crossings | advisory | 45.3 % | 46.0 % | 38.9 % | 38.0 % |
| blocking rows that fail (T5 aside) | | — | none | **T2, T3** | none (T3 at 1.096×) |

Identical loops against mode 0: mode 1 258/320 requests, modes 2 and 4 0/320. A repeat of mode 0 is identical on 320/320.

On the Gate surface the picture is sharper than on the Shuffle corpus. Blocks A + B are Belgrade-heavy, and that is where the fork's discount pays for urban rings and where the rescaled forms bite:

- **#6449 (mode 2):** +5 % / −3 % / +19 % curviness at c0.5 / c0.7 / c1.0, no retraces, 5 points fewer rings and 6 fewer self-crossings. It **fails T2** (served overlap +12 %) and T3 (1.11×), and the c0.7 distance error rises from 0.127 to 0.190 mean (p90 0.231 → 0.394).
- **Our weights, rescaled (mode 4):** +10 % / 0 % / +19 % curviness, no retraces, the same ring and self-crossing drops, and T2 **passes** (0.3487, near-dups 13.8 → 12.5 %). T3 sits on the bar at 1.096×. The c0.7 distance error rises much as in mode 2 (0.183 / 0.359).
- **Admissible heuristic (mode 1):** nothing moves.

### 4.4 Expansions, latency and the F03 optimality loss

**Return-leg A\* expansions** (labels per A\* call, and per request, from the stage ledger; deterministic, so the two mode-0 rows of each corpus are identical):

| corpus | mode | labels / call p50 | p90 | labels / request p50 | p90 | A\* ms / request p50 (quiet session) |
|---|---|---|---|---|---|---|
| Shuffle (K = 3, 216 serves) | 0 | 43 353 | 133 540 | 139 891 | 461 271 | 90–95 |
| | 1 | 52 266 (+21 %) | 159 949 (+20 %) | 158 457 (+13 %) | 514 937 (+12 %) | 116 |
| | 2 | 60 753 (+40 %) | 180 187 (+35 %) | 169 963 (+21 %) | 534 141 (+16 %) | 145 |
| | 3 | 61 125 (+41 %) | 181 529 (+36 %) | 171 235 (+22 %) | 537 364 (+16 %) | 137 |
| | 4 | 57 838 (+33 %) | 191 756 (+44 %) | 166 992 (+19 %) | 575 268 (+25 %) | 117 |
| Gate A + B (K = 12, 320 requests) | 0 | 117 076 | 198 543 | 1 478 728 | 2 503 956 | 424–517 |
| | 1 | 133 985 (+14 %) | 216 156 (+9 %) | 1 660 063 (+12 %) | 2 976 560 (+19 %) | 497 |
| | 2 | 122 608 (+5 %) | 228 065 (+15 %) | 1 696 045 (+15 %) | 3 364 518 (+34 %) | 554 |
| | 4 | 120 020 (+3 %) | 222 298 (+12 %) | 1 709 953 (+16 %) | 3 169 309 (+27 %) | 562 |

Wall-clock latency on the Shuffle corpus moved by less than its noise (p50 0.51–0.61 s for every mode, run while another session loaded the VM). The Gate-corpus T3/T4 read in §4.3 comes from the quiet block.

- **None of the admissible forms is cheaper to search.** The admissible heuristic (mode 1) costs +9–21 % labels per A\* call. #6449's objective and the rescaled fork objective cost +3–44 % per call, depending on corpus and percentile, and +15–34 % per request. That is partly the search and partly the different loops the new objectives build. The return-leg A\* is ~45 % of a request (the A1 profile, matched here: 424–517 ms of 0.88–1.12 s at K = 12). On the quiet block that turns into +10 % (mode 4) and +11 % (mode 2) wall p50. On the 2-vCPU prod box that lands at or above Gate v2's 1.10× ratchet.
- **The F03 optimality loss is noise.** `pairs.py` compares return legs built by the A\* (`route_leg`, `rescue`, or a pair `full_repair`) whose forward leg, Shuffle memory and bank-so-far are identical in mode 0 and mode 1. Each pair therefore faced the same objective and only the heuristic differs.
  - Shuffle corpus: 555 such returns. 551 identical, 2 equal-cost alternatives, and 2 cheaper under the admissible search, by 0.2 % and 0.5 %.
  - Gate corpus: 1 255 unique returns. 12 change cost: 5 are cheaper (−0.2 % to −15.6 %, one outlier at Vračar 60 km c1.0) and **7 are dearer** (+0.1 % to +11.1 %). Total return cost moves by **+0.01 %**.
  - An admissible heuristic does not make Valhalla's bidirectional A\* exact. The search stops once both frontiers pass the best connection plus `threshold_delta` (420), and prunes on the heuristic-based lower bound. A different heuristic is a different approximation, not a better one. The audit's "provably not optimal" stands. In practice it is a wash, at +9–21 % more expansions per search.

## 5. The four options

| | route quality (Gate A + B / Shuffle corpus) | latency | FINGERPRINT / Gate v2 | rebase friendliness | effort |
|---|---|---|---|---|---|
| **(a) adopt #6449, retire `prefer_curvature`** | curviness +5 / −3 / +19 % (c0.5/0.7/1.0); retraces 2 → 0 %; rings −5 pts; **T2 fail** (+12 % served overlap); c0.7 distance error +50 %. Shuffle: c1.0 +3 %, c0.5 −1 %, T2 +12 %, fallback 0.3 → 1.7 %, rescue 4.6 → 6.9 % | +15–34 % A\* labels per request; **T3 1.11× fail** | output change: FINGERPRINT bump + full Gate v2; the c → `use_curvature` map can live in the engine, so the 218-row costing table stays | best once merged (the sif patch shrinks to a mapping); while #6449 is open, it is a second fork patch on a moving target | medium |
| **(b1) borrow admissibility only** (mode 1) | nothing moves (258/320 identical, T1 1.00×) | +9–21 % labels per call, wall flat | output change on ~20 % of K = 12 requests → bump + Gate, for no gain | neutral | small (~15 lines) |
| **(b2) borrow the penalise-straight *form*** (mode 4: our weights ÷ the smallest factor) | curviness **+10 / 0 / +19 %**; retraces 2 → 0 %; rings −4 pts; self-crossings −7 pts; **T2 passes**; c0.7 distance error 0.127 → 0.183 (p90 0.359, inside R3). Shuffle: c1.0 +3.6 %, c0.5 −0.9 %, served overlap +10 % (that harness's T2 ratchet fails at 1.10×) | +16 % / +27 % labels per request (p50 / p90); T3 1.096×, on the bar | output change: FINGERPRINT bump + a full Gate v2 against Baseline v2 at the prod regime | neutral (constructor + one multiply in `EdgeCost`) | small to code, medium to tune |
| **(c) keep ours, avoid the clashes** | none | none | **none**: proto renumbering is JSON-neutral; renaming a table is source-only | removes both collisions (field 100, `kCurvatureFactor`) | trivial; the fields are already moved on `rebase/proto-private-range` (1098–1101) |
| **(d) contribute upstream** | — | — | — | goodwill on the code we may later adopt | a comment (draft in §7) |

Reading the rows:

- **(a) is not a drop-in.** It does make c1.0 curvier and takes urban rings out of the objective. It also funnels a bank's loops onto the same few curvy roads (T2 fails on both corpora), costs more search than T3 allows, and trades curviness between regions (§4.2: +5 % around Kragujevac and Niš, −8 % in Novi Sad). Its quality gains are matched by (b2) without the distinctness cost.
- **(b1) buys nothing.** The F03 "fix" moves no meter and costs expansions. Close F03 as **measured: a wash**.
- **(b2) is the real finding, and it is not about admissibility.** Dividing every edge factor by the smallest factor keeps every edge-vs-edge comparison as it is today. What changes is everything priced in seconds *outside* `EdgeCost`. Turn costs, `maneuver_penalty`, the 8 s continuity break and the density transition factors become `m` times lighter against the roads: m = 0.52 at c0.5, 0.40 at c0.7, 0.20 at c1.0. Today's discount makes those fixed seconds 2–5× heavier than they were designed against, and the router avoids turning onto the curvy minor road. Rescaling gives that back. It also shows why the c0.7 distance error and the T3 margin need a look before it ships. The same effect could be reached by scaling `TransitionCost` by `m` instead of the edges, which leaves the heuristic, the bucket width and the A\* threshold untouched. That variant is untested.
- **(c) is required regardless.** Field 100 and the name `kCurvatureFactor` collide the moment #6449 merges.
- **(d)**: we hold data on the reviewer's open question. Inverted avoidance and "lower the heuristic" are the same search on the edges, and neither is cheaper to search. What inverted avoidance changes is how much the transitions weigh, which here was worth more than the curvature table itself. A second point: #6449's convex table on a sampling-sensitive metric (§3).

## 6. Recommendation

**Keep `prefer_curvature`, clear the clashes (c), and take #6449's lesson rather than its code: run the penalise-straight form of our own weights (b2) through a proper Gate v2. Do not adopt #6449 now (a). Do not "fix" F03 (b1). Offer the data upstream (d).**

What to change in our implementation:

1. **Clashes (with the 3.9.1 rebase, no output change):** land `rebase/proto-private-range` (fields at old number + 1000). Also rename the fork's table `kCurvatureFactor` → `kForkCurvatureDiscount` in `motorcyclecost.cc`, so a future merge of #6449 (which adds `valhalla::sif::kCurvatureFactor` in `dynamiccost.h`) does not turn every use ambiguous. **No FINGERPRINT bump, no Gate run.** JSON clients never see proto numbers, and a rename is source-only.
2. **F03:** record it as measured: 12 of 1 255 returns change cost under an admissible heuristic, 5 cheaper and 7 dearer, total +0.01 %, for +9–21 % expansions. Leave `AStarCostFactor()` as it is, and note it in the audit trail (the v3 audit doc / ADR-0042's follow-ons).
3. **Candidate change, (b2):** in `MotorcycleCost`'s constructor compute `m`, the smallest factor `EdgeCost` can emit (this branch's mode-1/4 computation), and multiply the edge factor by `1/m` (mode 4). Before shipping:
   - (i) a full Gate v2 against **Baseline v2** with prod-config brackets on the 2-vCPU regime (T3 is on the bar here);
   - (ii) a look at the c0.7 distance error (0.127 → 0.183 mean);
   - (iii) the untested sibling that scales `TransitionCost` by `m` instead, which should give the same routes with no search cost.

   It is an engine-side change. `costing.rs` and its 218-row truth table do not change. It **alters route output, so FINGERPRINT bumps**, and a Gate v2 run is mandatory.
4. **When #6449 merges upstream:** re-run mode 2 against that release. If its T2 cost persists, keep `prefer_curvature` (with (b2) if it shipped). If a later revision fixes it and it beats (b2), map `prefer_curvature` onto `use_curvature` inside the engine. That again leaves the orchestrator table frozen and bumps only the FINGERPRINT.

**FINGERPRINT / Gate:** items 1 and 2 change no route output, so the rule does not fire. Item 3, or any adoption of #6449, alters route output: FINGERPRINT bump plus a full Gate v2 run (served surface of blocks A + B, Baseline v2, same-session prod-config brackets).

**The 3.9.1 rebase:** #6449 is not in 3.9.1 or `master` (open), so the rebase meets neither collision today. `rebase/proto-private-range` already moves the four fields to 1098–1101. This prototype's `curvature_mode` is 1199 in source to stay clear of them. It ran as 1100 in the measured binary, which makes no difference to JSON requests. (b2) touches `MotorcycleCost`'s constructor and the end of `EdgeCost`, next to the rebase's `motorcyclecost.cc` conflict hunk (#6214's `inv_distance_factor_`). Land it **after** the rebase, on the 3.9.1 base, and measure it against the rebased engine so the FINGERPRINT bumps once per change. The prototype commit on this branch is not meant to be cherry-picked as is.

## 7. Draft upstream comment (for the owner to post on #6449, if wanted)

> We run a fork with a motorcycle curvature preference implemented the other way round (a linear discount on curvy edges, `AStarCostFactor` untouched), so we prototyped this PR's `EdgeCost` block on our engine for comparison (Serbia extract, our round-trip loop builder, bidirectional A\* return legs). Some data on nilsnolde's question about the downsides of inverting:
>
> - On the edges, inverted avoidance and "discount, then lower `AStarCostFactor` by the smallest factor" are the same search: dividing every edge by a constant doesn't change the cheapest path. Neither was cheaper to search. Making our discount admissible cost +9–21 % A\* labels per search, and this PR's table at `use_curvature` 0.75–1.0 cost +3–44 % (corpus- and percentile-dependent). The heuristic stays admissible but is up to 7× too weak on straight roads.
> - Making the heuristic admissible did not make the search measurably more optimal: of ~1 250 comparable return legs, 12 changed cost, 5 cheaper and 7 dearer, net +0.01 %, because `threshold_delta` and the lower-bound pruning keep bidirectional A\* approximate either way.
> - The difference that *did* matter is the one the inversion causes outside `EdgeCost`. Keeping the minimum edge factor at 1 keeps turn costs, `maneuver_penalty` and other seconds-denominated transition costs at their designed weight. For us that was worth more route curviness (+10–19 % on the heavier settings) than the curvature table itself.
> - The averaged-radius `compute_curvature` and a sampling-invariant turning-density metric agree well on edges ≥ 200 m (r ≈ 0.95–0.98). They diverge on shorter edges (r ≈ 0.66, 12 % of km ≥ 5 buckets apart), and with a quadratic table that noise moves short-edge cost by up to 7×. Shortcuts take curvature from the whole contracted shape, so under a convex penalty a mixed shortcut costs less than its base edges (e.g. 2.7× vs 4× for half straight / half bucket 15).
>
> Happy to share numbers or the prototype if useful.

(Post only if the owner wants to. Nothing was posted or opened upstream.)

## Reproduce

```bash
# engine (arm64): build valhalla_service from this branch in valhalla24-deps:local, worktree mounted at /src
cmake /src -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=Off -DENABLE_PYTHON_BINDINGS=Off \
      -DENABLE_DATA_TOOLS=Off -DENABLE_TOOLS=On -DENABLE_SINGLE_FILES_WERROR=Off && make -j4 valhalla_service
./valhalla_service <orchestrator>/data/valhalla.json 2   # + "thor.roundtrip_stage_timing": true, a free port
# Shuffle corpus, one run per mode
python3 tools/curvature-6449/harness.py fire http://localhost:<port> apex08+m2 runs/s-m2
python3 tools/curvature-6449/harness.py analyze runs/s-m0 runs/s-m2 && python3 tools/curvature-6449/table.py runs/s-m0 runs/s-m2
python3 tools/curvature-6449/ledger.py runs/s-m2 <engine log> <first line> <last line>   # A* labels
python3 tools/curvature-6449/pairs.py returns runs/s-m0 runs/s-m1                       # F03 read
python3 tools/curvature-6449/roadmix.py http://localhost:<port> runs/s-m0 runs/s-m2          # road mix
# Gate v2 blocks A+B: corpus-v2 restricted to the vracar / vracar_amw / d44* cells (320 requests)
tools/loopqual/venv/bin/python tools/curvature-6449/corpus_ab.py corpus-v2-ab.yaml
LOOPQUAL_COSTING_EXTRA='{"curvature_mode": 2}' tools/loopqual/loopqual run --engine http://localhost:<port> \
      --corpus corpus-v2-ab.yaml --out results/v2-m2 --workers 2 --no-way
python3 tools/loopqual/gate_v2.py results/v2-m0-b results/v2-m2-b --bracket results/v2-m0-b --bracket results/v2-m0-c
python3 tools/curvature-6449/pairs.py returns-lq results/v2-m0 results/v2-m1             # F03 read, K = 12
# curvature census (any engine on the rig tiles)
python3 tools/curvature-6449/curvhist.py http://localhost:<port> curvhist.json
```

## Sources

- valhalla/valhalla#6449: description, commits, diff at `ed4053607`, review by nilsnolde (2026-11-01), author comments (2026-10-30, 2026-11-03). Files: `valhalla/sif/dynamiccost.h` (`kCurvatureSteps`, `curvature_strength`, `populate_curvaturefactor`), `src/sif/dynamiccost.cc` (`set_use_curvature`, parse), `src/sif/motorcyclecost.cc` (`EdgeCost` block), `proto/descriptors/options.proto` (`use_curvature = 100`), `test/gurka/test_use_options.cc`.
- valhalla/valhalla#6425 (density), referenced by #6449.
- Fork at `a3ea836af`: `src/sif/motorcyclecost.cc` (`kCurvatureFactor`, constructor, `EdgeCost`, `TransitionCost`, `AStarCostFactor`), `valhalla/sif/dynamiccost.h` (`kDensityFactor`, `EdgeFactor`), `src/mjolnir/util.cc` (`compute_curvature`), `src/mjolnir/shortcutbuilder.cc:529`, `src/midgard/pointll.cc` (`Curvature`), `src/thor/route_action.cc` (`route_leg`, stage ledger).
- `docs/curvagen/research/2026-09-05-v3-correctness-optimality-audit.md` (F03, F05, F22); `2026-11-04-upstream-since-fork.md` (branch `research/upstream-since-fork`, §4–§6); `2026-11-04-shuffle-variety-levers.md` (branch `research/shuffle-variety`, harness and Gate-v2-for-Shuffles adaptation); ADR-0041, ADR-0042; `docs/curvagen/extension-api.md` (FINGERPRINT rule).
- curvagen-orchestrator `crates/domain/src/costing.rs` (the frozen c → costing map).
- Branch `rebase/proto-private-range` (`5d10832b0`, fields at old number + 1000), read only.

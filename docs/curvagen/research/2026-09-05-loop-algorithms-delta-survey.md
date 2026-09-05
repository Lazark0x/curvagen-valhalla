# Loop Algorithms Beyond the 2026-07-13 Shortlist: a Delta Survey with Valhalla Fit

- **Date:** 2026-09-05 (wayfinder map [#36](https://github.com/Lazark0x/curvagen-orchestrator/issues/36), ticket [#37](https://github.com/Lazark0x/curvagen-orchestrator/issues/37); feeds the *choose the v4 direction* grilling [#43](https://github.com/Lazark0x/curvagen-orchestrator/issues/43)).
- **Scope:** what loop-generation techniques the [v3 survey](./2026-07-13-loop-route-algorithms.md) did **not** cover — or dismissed without measurement — that could remove the residual defect classes v3 still ships (mid-route **ring-detour lollipops**, dual-carriageway out-and-backs, stems inside the 1.5 km Start Exemption, Fallback Loops in sparse networks, Second-Via figure-8s), and how each adapts to Valhalla's directed, tiled, three-level hierarchical graph (shortcuts on levels 0–1, nodes split per level) inside the fork's native `/roundtrip` action at K=12 under the 1.10× latency ratchet. **Everything in the v3 survey is referenced, not re-surveyed:** GraphHopper `round_trip`, Kurviger, Gemsa/Zündorf Greedy Faces + TW3/TW4, Abraham et al. via-node alternatives (SEA 2010), Paraskevopoulos & Zaroliagis penalty + graded rejoin (ATMOS 2013), Lewis & Corcoran KRT (SN CS 2024).
- **Method:** primary sources only. Papers read as PDFs and cited by page/section (Suurballe & Tarjan, *Networks* 14 (1984); Abraham et al., SEA 2010; Jossé et al., arXiv:1609.08484). Fork source read at `curvature-costing` @ `b4f514d7f` (= upstream 3.8.2 + 54 commits) and cited `path:line`. Upstream Valhalla, BRouter, GraphHopper and OpenRouteService read from their own repositories via `gh api` (releases, `CHANGELOG.md`, PR bodies, merge/revert commits, per-file commit history). Product claims read from vendor-owned docs only — Calimoto and komoot through their own Zendesk Help Center JSON API and cycle.travel through Wayback snapshots of first-party pages, both because the sites are Cloudflare-gated; original URL and snapshot are cited together. **Anything not traceable to a first-party source is marked UNVERIFIED**, and there is a lot of it in §5. Prod/rig ground truth reused from [ADR-0037](../adr/0037-roundtrip-v3-defect-gated-loop-construction.md), [ADR-0038](../adr/0038-cross-candidate-bank-distinctness.md)/[0039](../adr/0039-xcand-distinctness-ships-at-0.2-latency-headroom-ledger.md)/[0040](../adr/0040-selection-time-distinctness-rejected.md), [A1](./2026-07-15-a1-return-leg-astar-audit.md), [A2](./2026-07-15-a2-fallback-return-astar-audit.md), [H1](./2026-07-16-h1-harvest-bucketing-selection-audit.md), [C1](./2026-07-16-c1-contended-ratchet-prod-regime.md).

## TL;DR verdict

**The v3 survey's load-bearing assumption is false for the return leg, and its defect meters are blind to the class riders are complaining about.** Three code-proven findings reframe the whole question before any new algorithm is considered. (1) **Both v3 defect detectors are cross-leg.** `seam_stub_m` measures an exact mirror spanning the seam (`route_action.cc:1382-1400`) and `stem_fraction` measures a start-windowed forward-prefix/return-suffix shadow, with its own comment conceding "the (rare) mid-loop shadow … is deliberately out of the engine's window" (`route_action.cc:1266-1267`). A ring detour that lives **inside one leg** cannot be seen by either meter, or by the harness (`tools/loopqual/metrics.py:254-269` finds only *exact-mirror* palindromes, and its docstring concedes "dual-carriageway U-turns … are NOT exact mirrors"). 0.00 % spikes and 0.3 % lollipops are therefore consistent with riders seeing both. (2) **The forward leg is an exact optimum under a cost model that pays for rings, and the return leg is not an optimum at all.** With prod `prefer_curvature = 0.8`, a maximally-twisty rural paved edge costs `0.85 − 1.6×0.30 = 0.37 ×` its time while a straight one costs `0.85 ×` — so **a curvy ring is net-cheaper than the straight edge up to 2.30× its length** at equal speed (`motorcyclecost.cc:95-112,383,474-477`; `dynamiccost.h:225-233`). The harvest is a plain `Dijkstras` (no heuristic) and therefore *finds every such ring that exists*; the return leg's bidirectional A\* runs on a heuristic (`AStarCostFactor()` = `kSpeedFactor[top_speed_] × min_linear_cost_factor_`, `motorcyclecost.cc:291-293`) that **ignores the curvature discount entirely** and is consequently **inadmissible by up to 1/0.37 ≈ 2.7×**. Ring detours should therefore concentrate in the **forward** leg — a falsifiable prediction for the census (#41). (3) **The hard exclusion is hierarchy-blind.** `IsUserAvoidEdge` is a bare `GraphId` set test (`dynamiccost.h:1068-1071`); the harvest `Dijkstras` *skips* shortcuts (`dijkstras.cc:131,490`) while the return bidirectional A\* *uses* them (`bidirectional_astar.cc:202-214`) and only expands them back into base edges in `FormPath` (`:1295-1298`). Upstream's own `/route` cost-factor path handles exactly this duality ("if it's a shortcut, also add all of its constituent edges … if it's not a shortcut, it may be part of one", `route_action.cc:308-327`); the round-trip exclusion at `route_action.cc:1631-1636` does not. **A shortcut over the forward corridor is a legal return path today.**

**On the new families.** The one genuinely new *structural* primitive worth building is **exact min-cost disjoint pairs (Suurballe–Tarjan)**, and it is a much better fit than the v3 survey's rejected structural option (Greedy Faces). Suurballe & Tarjan solve, on a **directed graph with non-negative lengths**, the min-total-cost pair of edge-disjoint `s→v` paths **for every sink `v` at once** in `O(m log_(1+m/n) n)` time and `O(m)` space (Networks 14 (1984) 325, abstract) — i.e. one pass over the harvest region yields, for *every* candidate turnaround, the **jointly optimal** disjoint loop instead of today's sequentially-optimal "forward-from-the-tree + hard-excluded return". Two properties settle the ticket's non-metric worry outright: the transform `c'(v,w) = c(v,w) − d(s,w) + d(s,v)` is **provably non-negative** ("For every edge (v,w), c'(v,w) ≥ 0, with equality if (v,w) is a tree edge", §II, p. 326, and the property holds "even if G contains negative-length edges but no negative cycles"), and it is **order-preserving** ("the ordering of such paths by length is unaffected by the transformation", §II) — so a curvature-discounted, non-length-proportional costing is a non-issue. Better still, existence is decidable up front: "there exist two such paths if and only if there is no edge contained in all paths from s to v" (§I, p. 325) — **the Fallback-Loop oracle A2 asked for, free, at selection time**. The vertex-disjoint variant (node splitting, §IV, p. 334, same complexity) additionally makes the two legs share no junction, which is what actually kills spikes, start-stems and same-road out-and-backs on a two-way network. **Cost:** the harvest Dijkstra already computes the required `d(s,·)`; the added work is one Dijkstra-like pass and a per-candidate `O(path)` construction — plausibly *cheaper* than today's K=12 hard-excluded bidirectional A\*s, which are 56 % of p50 (A1+A2). **Two honest caveats:** the single-source bonus applies to the *same-endpoint pair* problem, not to a directed *cycle* through `s` and `t`, so an oriented loop needs either a direction-symmetric approximation of the costing or a per-candidate min-cost circulation; and **length control is what makes it hard** — adding a length bound flips the problem from `O(m log n)` to NP-hard (Short Path Packing, arXiv:2404.10469).

**Nothing else new is shippable.** Arc-orienteering metaheuristics are the formally correct model of "a fixed-length loop maximising curviness", and are `NP`-hard (Jossé et al., §1: the OP "combines the NP-hard Knapsack and Traveling Salesman Problems"); the best road-scale primary numbers available are **0.2–1.0 s per query on a 500 K-vertex / 1 M-arc Los Angeles network for fastest paths of 5–30 minutes at a 200 % budget** (arXiv:1609.08484, §7 + Fig. 10) — one to two orders of magnitude short of a 300 km × K=12 fill, and the paper itself notes "A higher budget allows for greater detours which in turn increases the search space" (§7.1). The bank fill *is* a background `tokio::spawn` worker (`crates/api/src/serving/fill.rs:1,136-150`) so it does not sit on the rider's request — but C1 measured that a 12-way burst on the 2-vCPU box already inflates absolute latency 4.6× (p50 0.87 s → 3.95 s), so "background" buys a factor, not an order of magnitude. **The anti-ring toolbox is a costing question, not a search question**, and the literature already names the defect: a ring detour is precisely a violation of **T-local optimality** — "a path that is not locally optimal includes a local detour, which in general is not desirable" (Abraham et al., SEA 2010, §3). Exact verification is out of reach ("we would like to verify whether a path P is locally optimal … We do not know how to do this", §4; the reported evaluation "requires `O(|P|²)` point-to-point queries", §7), but the **T-test** is one extra P2P query per path with a factor-2 guarantee (Lemmas 1–2), and the cheap engineering surrogate — bound the discount so no detour can pay — is already a shipped upstream idea (`min_linear_cost_factor_`, `dynamiccost.cc:241-244`; `min_allowed_factor`, `route_action.cc:288`) that the fork's curvature term simply bypasses.

**One external datum outweighs the rest of the product survey.** BRouter merged a quality-first round-trip rewrite on 2026-07-08 — [PR #903](https://github.com/abrensch/brouter/pull/903), containing a `GreedyRoundTripPlanner`, an `IsochroneCandidateProvider`, a `ReuseClassifier`, `LoopQualityMetrics`, a `CorridorOverlapIndex` for "parallel same-corridor detection", and an anti-reuse penalty — i.e. independently, most of v3's shortlist — and **reverted the whole thing three days later** ([PR #945](https://github.com/abrensch/brouter/pull/945), 2026-07-11) primarily on latency: "old logic 8–15 s vs new 26–67 s for ~180 km". **That is a ~3× regression at a distance inside our band, and it was disqualifying for a project with no ratchet at all; ours is 1.10×.** Everything else in the product survey is empty of technique: GraphHopper's round trip is functionally unchanged since 2016 (zero commits on `RoundTripRouting`/`MultiPointTour`/`TourStrategy`/`AvoidEdgesWeighting` since 2026-07-01, and both of the source TODOs the v3 survey quoted are still verbatim on master), ORS is a confirmed thin pass-through to it that additionally pins `points = 2`, komoot's own docs say their round trip "often results in an out-and-back route", cycle.travel's own docs say "sometimes it won't be different, particularly on short journeys or in areas with few roads" (an independent corroboration of our Fallback-Loop regime), and Calimoto publishes parameters but no mechanism. **No public competitor detects a ring detour or a dual-carriageway mirror at all.**

**Ranked shortlist (details in §7): (1) bound the curvature discount so a ring can never be net-cheaper — costing, ~0 cost, structural for the ring class; (2) close the shortcut hole in the hard exclusion — return leg, ~0 cost, structural for shortcut-laundered reuse; (3) a self-proximity / return-to-junction meter — new post-pass, ~ms, the missing detector for rings, figure-8s and dual-carriageway mirrors; (4) Suurballe–Tarjan disjoint-pair loop construction — new pass replacing harvest+return, potentially latency-negative, structural for spike/stem/reuse; (5) restore A\* admissibility (or disable the heuristic inside a radius, upstream PR #6257's shape) — costing/search, cost unknown; (6) forbid the Second Via's opposite half-sector — one constant, ~0 cost, structural for figure-8s.** Rejected for fit: arc-orienteering metaheuristics, length-bounded disjoint-path packing, exact UBS/local-optimality verification, plateau-based loop construction, and (still) Greedy Faces.

---

## 1. Ground truth restated: where the residual can hide

The v3 pipeline is documented in [ADR-0037](../adr/0037-roundtrip-v3-defect-gated-loop-construction.md). Three properties of the *as-built* code — none of them stated in the v3 survey — bound what the residual classes can be.

### 1.1 Every defect meter we own is cross-leg

| meter | where | what it measures | blind to |
|---|---|---|---|
| `seam_stub_m` | `route_action.cc:1382-1400` (engine Defect Gate) | "the longest exact-mirror stub whose **interval covers the seam**" | anything not an exact mirror; anything away from the seam |
| `stem_fraction` | `route_action.cc:1251-1379` (Second Via trigger) | forward **prefix** shadowed by return **suffix**, inside a start window `1500 m + 0.10×total + 1000 m` | its own comment: "the (rare) mid-loop shadow the harness's full-leg meter would fold into a stem is **deliberately out of the engine's window**" (`:1266-1267`) |
| `find_spikes` | `tools/loopqual/metrics.py:254-269` (harness) | exact-mirror palindromes `p[i−1]==p[i+1]` extended outward; classes `seam_uturn`/`seam_wrapped`/`mid_fwd`/`mid_ret` | its own caveat: "dual-carriageway U-turns have direction-distinct geometry and are **NOT exact mirrors** … so spike counts are a slight undercount" |
| `corridor_stats` | `metrics.py:401-418` | lollipop **stem** fraction + `bulb_count` | a bulb hung off the *middle* of a leg with no stem |
| `compactness` | `metrics.py:536-548` | isoperimetric quotient | its own caveat: "self-intersecting loops (theta/figure-eight) partially cancel signed area, so IQ **understates** their enclosed area" |

**Consequence.** A **ring detour** — leave junction `J`, ride a curvy ring, rejoin at `J'` 50 m away, entirely inside one leg — is not an exact mirror (it is a simple path), does not span the seam, is not a start-window stem, and does not raise `bulb_count`. It is invisible to **all five**. The same is true of a **dual-carriageway out-and-back** (direction-distinct geometry) and of a **Second-Via figure-8**. This alone explains "0.00 % spikes, 0.3 % lollipop, yet riders still see them" without needing any hypothesis about prod ≠ rig. *(Evidence: measured — code read at HEAD; the atlas's "0 of `mid_fwd`/`mid_ret` in 2 602 engine loops" is a measurement of the **exact-mirror** class only.)*

### 1.2 The cost model pays for rings, and the forward leg is guaranteed to find them

`MotorcycleCost::EdgeCost` (`motorcyclecost.cc:450-503`) computes `cost = length × kSpeedFactor[min(edge_speed, top_speed)] × factor`, where

```
factor = kDensityFactor[density] + highway_factor_·kHighwayFactor[class]
       + surface_factor_·kSurfaceFactor[surface] + curvature_factor_·kCurvatureFactor[curvature] …
```

with `kDensityFactor[d] = 0.85 + 0.025·d` (`dynamiccost.h:225-233`), `kCurvatureFactor[15] = −0.30` (`motorcyclecost.cc:95-112`, "Negative values reward twistiness (make the edge cheaper)"), and `curvature_factor_ = prefer_curvature × 2.0` (`motorcyclecost.cc:383`).

At prod's `prefer_curvature = 0.8`, on a rural (`density 0`), paved, primary/tertiary edge (`kHighwayFactor = 0.0`, `kSurfaceFactor = 0.0`):

| `prefer_curvature` | `factor` (straight, curvature 0) | `factor` (curvature 15) | **max detour that is still net-cheaper, equal speeds** |
|---|---|---|---|
| 0.0 (stock) | 0.85 | 0.85 | 1.00× |
| 0.5 | 0.85 | 0.55 | **1.55×** |
| **0.8 (prod "curvy")** | 0.85 | **0.37** | **2.30×** |
| 1.0 (max) | 0.85 | 0.25 | **3.40×** |

Speed differences temper this: a 50 km/h curvy ring against a 90 km/h straight edge at c0.8 still affords `2.30 × 50/90 = 1.28×` the length. `curviness_continuity` adds `kContinuityBreakPenalty = 8.0 s` when *leaving* a curvy road (`motorcyclecost.cc:63`, applied at `:572-574`), i.e. an extra reward for staying on the ring. **The ring detour is not a bug in the search; it is what the cost function asks for.** *(Evidence: inferred — arithmetic from the constants; the elasticity has not been measured on the rig.)*

And the **harvest is a plain label-setting `Dijkstras`** (`RoundTripExpansion : public Dijkstras`, `roundtrip_expansion.cc`), i.e. an exact optimum with no heuristic. So every ring that is cheaper *is* taken. The forward leg is where rings must live.

### 1.3 The return leg's A\* heuristic is inadmissible — the v3 survey's optimality argument does not hold there

`AStarCostFactor()` returns `kSpeedFactor[top_speed_] × min_linear_cost_factor_` (`motorcyclecost.cc:291-293`). `min_linear_cost_factor_` is initialised to `1.0` (`dynamiccost.cc:202`) and is **only** lowered by upstream's `cost_factor_edges` (`dynamiccost.cc:241-244`, comment: "store the overall minimum factor so it won't mess with the A\* heuristic"). The fork's curvature discount is **not** in that minimum. With `top_speed_` defaulting to `kMaxAssumedSpeed = 140` km/h (`graphconstants.h:101`; `kMotorcycleSpeedRange{10, kMaxAssumedSpeed, kMaxSpeedKph}`, `motorcyclecost.cc:66-67`), the heuristic charges `3.6/140 = 0.0257 s/m` while an edge can cost as little as `3.6/v × factor`.

The heuristic **overestimates** — i.e. A\* loses its optimality guarantee — for any edge whose speed exceeds `top_speed_ × factor`:

| `prefer_curvature` | `factor` floor | inadmissibility onset | overestimate at a 90 km/h curvy edge |
|---|---|---|---|
| 0.0 (stock upstream) | 0.85 | v > 119 km/h | — |
| 0.5 | 0.55 | v > 77 km/h | 1.16× |
| **0.8 (prod)** | **0.37** | **v > 52 km/h** | **1.74×** (worst case 2.70×) |

Two consequences, both new:

- **The v3 survey's §1 claim — "both legs are individually shortest paths … so *in-leg* spikes/sub-loops are essentially impossible" — is not sound for the return leg.** It is sound for the forward leg (exact Dijkstra) and unproven for the return leg (inadmissible A\*).
- The bias runs *toward directness*: an overestimating heuristic under-explores the discounted branch, so the return leg is systematically **less curvy than its own cost model wants**. That is a plausible partial explanation for why `astar` was byte-identical under `threshold_delta` 420→100 in A1 (a heuristic-bound search, not just a "sharp penalty optimum"), and it is a second falsifiable prediction for #39/#41.

Upstream is wrestling with the same trade: open PR [#6257](https://github.com/valhalla/valhalla/pull/6257) adds `thor.costmatrix.dijkstra_distance` — *disable the A\* heuristic up to a distance threshold* — the exact shape of the fix available here.

### 1.4 The hard exclusion is hierarchy-blind (shortcut laundering)

`route_leg` builds the exclusion set from the corridor's own edge ids and their opposing edges only:

```cpp
if (static_cast<double>(pi.path_distance) > kStartExemptionMeters) {
  hard.push_back({e, 0.0});
  if (opp.is_valid()) hard.push_back({opp, 0.0});
}
…
cost->AddUserAvoidEdges(hard);            // route_action.cc:1631-1636, 1689
```

`IsUserAvoidEdge` is a bare `GraphId` membership test (`dynamiccost.h:1068-1071`), consumed by `MotorcycleCost::Allowed`/`AllowedReverse` (`motorcyclecost.cc:413,438`). Meanwhile:

- the harvest **skips shortcuts** — `if (directededge->is_shortcut() || …) continue;` (`dijkstras.cc:131`, `:490`) — so forward-leg edges are always *base* edges, including on levels 0–1 beyond `near_radius_` (`roundtrip_expansion.cc:45-49`);
- the return **uses shortcuts** — `if (meta.edge->is_shortcut()) { … shortcuts |= meta.edge->shortcut(); }` (`bidirectional_astar.cc:199-214`) — and only unpacks them in `FormPath` (`:1295-1298`, `:1324-1327`);
- `validate_alternate_by_sharing` carries the matching warning: "**Note that you should recover all shortcuts before call this function**" (`alternates.cc:148-149`).

Upstream's `/route` cost-factor path already solves this duality both ways (`route_action.cc:308-327`: "if it's a shortcut, also add all of its constituent edges … if it's not a shortcut, it may be part of one", plus `add_shortcut()` at `:181-227`). **The round-trip exclusion does not.** A level-0/1 shortcut that supersedes excluded forward base edges is therefore a legal return path, and `FormPath` then writes the very base edges back into the geometry. This is a concrete, code-proven mechanism for the residual `edge_reuse_geom = 0.059` (A1) that survives a *hard* exclusion. *(Evidence: code-proven mechanism; **not measured** — no counter exists. First question for #39.)*

### 1.5 The Second Via is a figure-8 generator by construction

The Second Via picks `V2` from the **opposite half-sector**: `sep >= 180 − kSecondViaSectorDeg/2` with `kSecondViaSectorDeg = 180.0f`, i.e. any bearing ≥ 90° from the turnaround, ranked by curviness (`route_action.cc:1031,1886-1893`). A loop `start → turnaround → V2 → start` with `turnaround` and `V2` on opposite sides of `start` is a two-lobe theta/figure-8 by construction. The rebuild is gated on `seam_stub_m` at both junctions and on `stem_fraction` (`:1961-1966`) — **never on self-intersection**. `second_via_count` is the only observability. *(Evidence: code-proven; rate not measured per distance band.)*

---

## 2. Exact edge-/vertex-disjoint methods as a loop primitive

### 2.1 Suurballe–Tarjan: what it actually gives

**Primary source:** J. W. Suurballe and R. E. Tarjan, "A Quick Method for Finding Shortest Pairs of Disjoint Paths", *Networks* 14 (1984) 325–336 ([PDF](https://cadmo.ethz.ch/education/lectures/FS18/SAADS/papers/Networks84_Quick_Method_Disjoint_Paths.pdf)), read in full. Predecessor: J. W. Suurballe, "Disjoint paths in a network", *Networks* 4 (1974) 125–145 (their ref [1]; vertex-disjoint version).

Abstract, p. 325 (verbatim):

> "Let G be a directed graph containing n vertices, one of which is a distinguished source s, and m edges, each with a non-negative length. We consider the problem of finding, for each possible sink vertex v, a pair of edge-disjoint paths from s to v of minimum total edge cost. … We give an implementation of Suurballe's algorithm that runs in `O(m log_(1+m/n) n)` time and `O(m)` space. Our algorithm builds an implicit representation of the n pairs of paths; given this representation, the time necessary to explicitly construct the pair of paths for any given sink is `O(1)` per edge on the paths."

Mechanism (§II, p. 326):

> **Step 1.** "Find a shortest-path tree T rooted at s. … Compute d(s,v) … for every vertex v."
> **Step 2.** "Transform the length of every edge (v,w) by defining `c'(v,w) = c(v,w) − d(s,w) + d(s,v)`."

Theorem 1 (p. 327) then obtains the pair "from the path from s to v in T and a shortest path from s to v in `G_v`" (`G_v` = G with the tree path to `v` reversed), "discarding every edge in one path whose reversal appears in the other, and grouping the remaining edges into two paths"; and the single-source speed-up: "we can in effect run Dijkstra's algorithm in parallel on all of them, obtaining `d_v(s,v)` for all vertices v in a single Dijkstra-like calculation."

Vertex-disjoint variant (§IV, p. 334): "splitting each vertex v of G into two vertices `v1` and `v2` joined by an edge `(v1,v2)` of length zero … `G'` has 2n vertices and n + m edges and is constructible in `O(m)` time. Thus we can solve the shortest pairs problem for vertex-disjoint paths in `O(m log_(1+m/n) n)` time and `O(m)` space."

Multidigraph support (§IV, p. 334): "We can easily adapt our algorithm to work on arbitrary multidigraphs; that is, directed graphs with multiple edges, without the antisymmetry restriction. We need only redefine the tentative predecessor `p(v)` … to be an **edge** entering v rather than a vertex preceding v." — Valhalla's graph is exactly this (both directions of a road are separate `DirectedEdge`s), so no adaptation cost.

`k > 2` (§IV, p. 335–336): "the minimum-cost augmenting-path algorithm for minimum-cost network flow [6] will find a shortest set of k edge-disjoint paths from a given source s to a given sink v in k iterations of Dijkstra's algorithm, or `O(km log_(1+m/n) n)` time."

### 2.2 Non-metric costing is a non-issue — and that is provable, not hoped

The ticket's worry ("curvature discount makes cost not proportional to length; negative reduced costs in Suurballe's transform") is answered directly by the paper:

- **Reduced costs are non-negative by construction.** §II, property (1), p. 326: "For every edge (v,w), `c'(v,w) ≥ 0`, with equality if (v,w) is a tree edge" — and explicitly "true even if G contains negative-length edges but no negative cycles". Valhalla edge costs are strictly positive anyway (`sec > 0`, `factor ≥ 0.25` at the extreme), so the transform is safe with room to spare.
- **The transform is order-preserving.** §II, p. 326: "two paths with the same start and finish vertices have their lengths transformed by the same amount, and the ordering of such paths by length is unaffected by the transformation."

No metric, triangle-inequality or length-proportionality assumption appears anywhere in the algorithm. **Suurballe–Tarjan is costing-agnostic** in exactly the way the fork needs. *(Evidence: documented — direct quotation.)*

The only genuine hazard is the reverse direction: the transform needs the *exact* `d(s,·)` of the graph the return may use. The fork's harvest is **pruned** (`max_meters_ = 1.2 × target/2`, level-2 cut beyond `near_radius_ = min(max_meters_, 90 km)`, `roundtrip_expansion.cc:45-61`) and **shortcut-free**. Potentials from a pruned tree are still valid *within* the explored region; a disjoint-pair pass must be confined to the same region (which is what we want anyway) and must make the same shortcut choice on both passes — see §1.4.

### 2.3 The two things that do **not** transfer for free

**(a) Directed edge-disjointness ≠ road-disjointness.** In Valhalla, `(u,v)` and `(v,u)` are distinct `DirectedEdge`s. A Suurballe edge-disjoint pair may therefore ride the same physical road in opposite directions — i.e. it does **not** forbid the spike. The fix is the paper's own §IV vertex-splitting: **vertex-disjoint** legs share no junction, and since a two-way road's endpoints are junctions, vertex-disjointness implies road-disjointness on two-way networks. This is what makes the vertex-disjoint variant the interesting one for us — it structurally kills the seam spike, the start stem, and same-road reuse in one move. **It does not kill the dual-carriageway mirror** (parallel carriageways are distinct node chains) and it does not kill ring detours (a ring is a simple path; disjointness has nothing to say about it).

*Valhalla-specific caveat:* nodes are **split per hierarchy level** — the same physical junction has a different `GraphId` on level 2 and level 1. Vertex-disjointness on raw `GraphId`s would let the two legs meet at the same physical junction across levels. A correct implementation must either confine the pass to one level or canonicalise level-transition node pairs. *(Evidence: inferred from Valhalla's hierarchy design + `roundtrip_expansion.cc:45-49`; not tested.)*

**(b) A pair of `s→v` paths is not an oriented cycle.** Suurballe's problem is two paths *both* running `s→v`. A round trip needs `s→v` and `v→s`. On a **direction-symmetric** costing the two coincide (reverse one path and read it back), and the fork's costing *is* direction-symmetric per edge for the terms that matter (density, class, surface, curvature are properties of the road; the reuse/rejoin/xcand multipliers are explicitly marked on **both** directions, `route_action.cc:1626-1630,1663-1669`). It is **not** symmetric for one-ways, turn/transition costs, or direction-specific speeds. Two honest options:

| rung | construction | exactness | cost |
|---|---|---|---|
| **A (recommended)** | Suurballe–Tarjan (vertex-split) on the road graph with direction-symmetrised edge cost, then a **one-way feasibility repair** on the leg chosen as the return | exact for the symmetrised costing, heuristic for the true one | one Dijkstra-like pass for **all** turnarounds + `O(path)` per candidate |
| **B (fallback)** | per-candidate **min-cost circulation** through `s` and `v` with unit vertex capacities (the framing Suurballe & Tarjan themselves use — §I: "a special case of minimum-cost network flow"; Theorem 1 "is immediate from the minimum-cost augmenting path algorithm for minimum-cost network flow") | exact on the directed graph | per candidate, no single-source bonus; heavier than today's bidir A\* |

*(Evidence for rung A's symmetry premise: code-read of the costing terms — **inferred**, and the one-way failure rate on Serbian rural tiles is unmeasured. This is the single biggest open question in this survey; see §10.)*

### 2.4 The free bonus: a returnability oracle at selection time

§I, p. 325 (verbatim): "Note that the reachability of a vertex v from s is not enough to imply the existence of two edge-disjoint paths from s to v; **there exist two such paths if and only if there is no edge contained in all paths from s to v**."

That is exactly the Fallback-Loop condition — a bridge/cut edge every route home must cross. Today the fork discovers it the expensive way: run the hard-excluded bidirectional A\*, get nothing, run a second full bidirectional A\* on the soft leash (`route_action.cc:1698-1712`). A2 measured this at **6.29 fallbacks/request** overall and **8.4 of 12 candidates at 20–50 km**, costing 27 % of return-leg time — and concluded a "turnaround-returnability pre-filter … Needs returnability information at selection time (a reverse reachability check) — algorithmic, backlog-scale". **Suurballe's existence condition supplies precisely that, as a byproduct of the pass that also builds the loop.** Turnarounds with no disjoint return are skipped before any A\* runs, instead of consuming a build slot and two searches. *(Evidence: documented (the condition) + measured (the cost it would avoid, A2).)*

### 2.5 Bhandari

R. Bhandari's two-disjoint-paths method (*Survivable Networks: Algorithms for Diverse Routing*, Kluwer 1999; and "Optimal physical diversity algorithms and survivable networks", *Proc. 2nd IEEE Symposium on Computers and Communications*, 1997, pp. 433–441, DOI [10.1109/ISCC.1997.616037](https://doi.org/10.1109/ISCC.1997.616037)) is the same min-cost-flow idea implemented by **reversing the first path's arcs with negated lengths and re-running a negative-capable shortest-path algorithm**. **UNVERIFIED at primary-source level** — the book is not obtainable and the IEEE paper is paywalled; the algorithmic characterisation above comes from Suurballe & Tarjan's own treatment of the negative-length case rather than from Bhandari. On the merits it is strictly worse for us than Suurballe–Tarjan: negative arcs force Ford–Bellman, which Suurballe & Tarjan cost at `O(nm)` (§IV, p. 335: "If G contains no negative cycles, we can solve the shortest pairs problem in `O(nm)` time by finding shortest paths in G using the Ford–Bellman algorithm … instead of Dijkstra's algorithm"), and it has **no single-source bonus**. **Recommendation: do not pursue Bhandari; the potential transform is the same idea done faster.**

### 2.6 Length-bounded cycles are NP-hard — this is the wall

Everything above minimises *cost*, with no control of *length*. Our problem wants a loop of a target length. Adding that constraint changes the complexity class:

- **Short Path Packing** — "given a graph G, integers k and ℓ, and vertices s and t, whether there exist k pairwise internally vertex-disjoint s–t paths of **length at most ℓ**" — "has been proven to be NP-hard and fixed-parameter tractable parameterized by k and ℓ" (M. K. Huber et al., "How quickly can you pack short paths? Engineering a search-tree algorithm for disjoint s-t paths of bounded length", [arXiv:2404.10469](https://arxiv.org/abs/2404.10469), abstract).
- The fixed-length round trip is likewise NP-hard "even if the only optimization goal is length" (Gemsa/Zündorf — already on file in the [v3 survey §4](./2026-07-13-loop-route-algorithms.md)).

**Lagrangian relaxation** is the classical dual handle on exactly this shape of constraint: G. Y. Handler and I. Zang, "A dual algorithm for the constrained shortest path problem", *Networks* 10(4) (1980) 293–309, DOI [10.1002/net.3230100403](https://doi.org/10.1002/net.3230100403) — "a Lagrangian relaxation algorithm for the problem of finding a shortest path between two nodes in a network" subject to "a knapsack-type constraint", used "to reduce the value of k" in a k-shortest-paths enumeration, with "orders of magnitude savings when the approach is applied to large networks". *(Citation and these phrases verified from the Tel Aviv University institutional record; the full text is paywalled — **UNVERIFIED** beyond the abstract, and no duality-gap figure was obtainable.)*

**Fit verdict on the Lagrangian line: reject.** The relaxation replaces the length constraint with a multiplier `λ` on length and solves a sequence of *unconstrained* shortest-path problems, then closes the gap by enumerating k-shortest paths. Each `λ` iteration is a full pass, the gap is not guaranteed to close, and the fork already has a cheaper, measured mechanism for the same objective: harvest turnarounds in a distance band (`ScanBand` 0.55–1.18×, `route_action.cc:1011-1012`) plus a node-guarded one-shot distance correction (`:1041-1052`), currently reading `dist_err` 16 % on `b1-x02`. **The right use of the disjoint-path machinery is therefore not "solve the constrained problem", but "keep today's turnaround-in-a-band length control and swap the *closure* for an exact disjoint pair".** That is rung A above, and it inherits the `O(m log n)`-for-all-sinks property because the length constraint stays in the *selection* step, outside the algorithm.

### 2.7 Cost estimate at 300 km × K=12

Today (A1, H1, ADR-0037 §6): fresh build **p50 ~0.6 s / p95 ~2.5 s**; shared forward expansion **24 ms p50** amortised over K; per-candidate pre-return cost **3–4 ms**; return leg **20–50 ms** per candidate, of which `astar` = 45 % of request p50 (1 061 ms p95) and `astar_fb` = 11 % of p50 (521 ms p95) — **the return legs are 56 % of p50**.

Suurballe–Tarjan rung A replaces *both* the K hard-excluded bidirectional A\*s **and** the ~6.3 fallback A\*s with:

| step | work | estimate |
|---|---|---|
| Step 1 (shortest-path tree + `d(s,·)`) | **already paid** — it is the harvest `Dijkstras` | 0 |
| vertex-splitting | `O(m)` on the explored region, or done implicitly by an edge-entering predecessor (§IV) | small |
| Step 2 + the single "Dijkstra-like calculation" over `G_v` for all `v` | one more label-setting pass over the same pruned region | **≈ 1 × harvest ≈ 25–60 ms** |
| per-candidate pair construction | `O(1)` per edge on the paths (abstract, p. 325) | ~1–3 ms × 12 |

**Estimated net: −30 % to −50 % of p50**, i.e. the direction is plausibly *latency-negative*, which would fund the ratchet rather than spend it. **Confidence: low.** The estimate assumes the second pass costs about what the harvest costs (same region, same pruning, roughly double the vertex count after splitting), and it ignores the one-way repair and the length-band re-selection. It must be prototyped (#43 → prototype ticket), not trusted. *(Evidence: inferred; the component costs it is built from are measured.)*

---

## 3. Orienteering / arc-orienteering metaheuristics

### 3.1 The formal model, and why it is the *right* model

"Find a closed walk from `s` of length in `[(1−ε)L, (1+ε)L]` maximising accumulated curviness" is literally the **Arc Orienteering Problem** (value on arcs, budget on cost). Primary framing from G. Jossé, Y. Lu, T. Emrich, M. Renz, C. Shahabi, U. Demiryurek, "Scenic Routes Now: Efficiently Solving the Time-Dependent Arc Orienteering Problem", [arXiv:1609.08484](https://arxiv.org/abs/1609.08484), §1:

> "the NP-hard Orienteering Problem (OP) asks to find a path from a given source to a given destination abiding by a given cost budget but maximizing the value collected along the way. … The OP combines the NP-hard Knapsack and Traveling Salesman Problems, hence is NP-hard itself. … Another variation is the Arc Orienteering Problem (AOP) … In the AOP, [the value] is associated with the arcs. … The AOP … corresponds to tasks where the value is collected 'along the way'. Examples of such tasks are the routes of firefighting planes or the planning of scenic bike trips."
> "Due to their NP-hardness, static variations of the OP and AOP are usually solved employing metaheuristics."

The cycling-specific line the ticket names is real and on point: C. Verbeeck, P. Vansteenwegen, E. H. Aghezzaf, "An extension of the arc orienteering problem and its application to cycle trip planning", *Transportation Research Part E* 68 (2014) 64–78, DOI [10.1016/j.tre.2014.05.006](https://doi.org/10.1016/j.tre.2014.05.006) — extending Souffriau et al.'s GRASP work on cycle trips in East Flanders by allowing repeated node visits, with upper *and lower* limits on tour length. **UNVERIFIED beyond the citation:** the publisher has elided the abstract from every aggregator record I could reach, ScienceDirect returns 403, and the KU Leuven open-access handle redirects to a search page. Treat the "multiple node visits + two-sided length limits" formulation as **the closest formal match to our problem statement in the literature**, whose method detail I could not read at primary-source level.

### 3.2 What real implementations achieve at road-graph scale

Jossé et al. §7 + §7.1 + Fig. 10 (the only road-scale AOP timings I could source primarily):

- **Network:** "a time-dependent road network of Los Angeles, CA, USA … which contains about **500K vertices and 1M arcs**" (§7).
- **Baseline AOP solver:** "the AOP solution presented in [38] which employs an Iterative Local Search approach combined with spatial pruning techniques" — i.e. Lu & Shahabi's SIGSPATIAL 2015 algorithm — "we call this algorithm AOP-ILS" (§7.1).
- **Query shape:** "standard query budget is 200 % (i.e., 40 minutes)"; instances bucketed by fastest-path travel time `k ∈ {5, 10, 15, 20, 25, 30}` minutes.
- **Runtimes (Fig. 10):** AOP-ILS ≈ **200 ms at k=5 rising to ≈ 1 000 ms at k=30**; the time-dependent variant ≈ 250 ms → ≈ 2 100 ms, "2TD-AOP solutions can be computed in less than twice the processing time of AOP solution, usually under two seconds."
- **Scaling warning:** "A higher budget allows for greater detours which in turn increases the search space" (§7.1).
- For scale calibration on exact methods: "Even for small instances with at most 100 vertices, solving a less complex MIP often requires days" (§7).

**Translation to our regime.** A 300 km motorcycle loop is a **~4–5 hour** tour — 8–10× the longest instance above, at a 200 % budget on a network of comparable order (Balkans tiles). Even taking AOP-ILS's *best* number and scaling linearly in path length (their Fig. 10 is roughly linear at fixed budget ratio), one candidate lands at **~5–10 s**, and K=12 at **~1–2 minutes** — and that ignores the super-linear search-space growth the authors flag. *(Evidence: documented for the source numbers; **inferred** for the extrapolation.)*

### 3.3 Can a K=12 bank fill amortise it? Partly — and less than the ticket assumes

The ticket's premise is correct on the mechanics and optimistic on the budget:

- **The fill is genuinely off the request path.** `crates/api/src/serving/fill.rs:1` — "Bank Fill: the eager **background** same-seed K=fill_k expansion"; enqueue is "Synchronous and non-blocking: a full queue drops the fill" (`:98-99`, `:130-133`); the worker is a `tokio::spawn` single-flight loop (`:136-150`). Only the first rider in a cell pays a live build, at `SERVE_K`, not `fill_k` (ADR-0037 §7.7: "first rider per cell pays one 0.6–2.5 s live build").
- **But the box is 2 vCPU.** C1 measured, at prod spec (`docker --cpus 2`, `valhalla_service` concurrency 2), that raising client concurrency from 3 to 12 inflated absolute latency **4.6×** (OFF p50 0.867 s → **3.954 s**, p95 2.945 s → **8.723 s**) — "absolute generation latency degrades sharply under a 12-way concurrent burst on 2 vCPU". A background fill that costs 100× more than today's does not become free; it becomes a permanent 2-vCPU occupancy that competes with live first-rider builds and with the `/route` One-Way path.
- **And the ratchet is a rig gate on engine time**, not a rider-facing SLO (ADR-0037 §6: "governed by the rig-ratio gate (≤ 1.10×)"). A metaheuristic direction would have to *move the gate*, which is a #42 decision, not a survey finding.

**Verdict: reject arc-orienteering metaheuristics for the serving path (again, now with numbers).** The v3 survey rejected the from-scratch solver lane as "disproportionate" without measurement; this survey supplies the measurement basis: **~1–2 orders of magnitude over budget, on a box with no headroom to hide it.** Keep the AOP framing as the **eval-side reference optimum** — an offline AOP-ILS run on a handful of corpus cells would give Gate v2 (#42) an *upper bound on achievable curviness at a target length*, which no current meter provides. That is a legitimate, cheap use of this literature.

---

## 4. In-leg admissibility under a non-metric costing — the anti-ring toolbox

### 4.1 The literature already names the defect

Abraham, Delling, Goldberg, Werneck, "Alternative Routes in Road Networks" (SEA 2010, [PDF](https://renatowerneck.wordpress.com/wp-content/uploads/2016/06/adgw10-alternatives-sea.pdf)), §3, read in full:

> "A first condition for a path P to be **T locally optimal** (T-LO) is that every subpath P′ of P with `ℓ(P′) ≤ T` must be a shortest path. … **Note that a path that is not locally optimal includes a local detour, which in general is not desirable.**"
> "We say that a path P has `(1+ε)` **uniformly bounded stretch** ((1+ε)-UBS) if every subpath (including P itself) has stretch at most `(1 + ε)`."

Fig. 1's caption is our ring detour drawn: "The alternative through w is a concatenation of two shortest paths, s–w and w–t. Although it has high local optimality, it looks unnatural because there is a much shorter path between u and v."

**The crucial adaptation for us.** Each fork leg *is* a shortest path under the curvature-discounted costing, so it is trivially T-LO **for that cost function** — which is why the v3 survey concluded local optimality was "irrelevant per-leg". That conclusion is right about the *fork's* metric and wrong about the *rider's*. A ring detour is a violation of T-local optimality **measured against a reference metric** (length, or plain drive time). **The correct v4 formulation is: require each leg to be T-locally-optimal with respect to length/time while being cost-optimal with respect to curviness.** That is a two-metric admissibility condition, and it is exactly the shape the alternatives literature works in.

### 4.2 What it costs to test in-leg — and why exact testing is out

- **Exact is not known to be tractable.** §4: "stretch and local optimality are much harder to evaluate, requiring quadratically many shortest path queries (on various pairs of vertices). Ideally, we would like to verify whether a path P is locally optimal (or is `(1+ε)`-UBS) in time proportional to `|P|` and a few shortest-path queries. **We do not know how to do this.**" §7 confirms the practice: "reporting these numbers requires `O(|P|²)` point-to-point queries for each path P; this evaluation is not included in the query times." The 2010 conclusion still lists it as open ("are there efficient exact tests for local optimality and uniformly bounded stretch?"), and **upstream Valhalla still has not implemented one** — `validate_alternate_by_local_optimality` is still `// [TODO] NOT IMPLEMENTED` on master at 2026-09 (`src/thor/alternates.cc:194-197`), with `kAtLeastOptimal` still commented out at `:22`.
- **The T-test is the affordable approximation.** §4: "Take a via path `P_v` and a parameter T. Let `P1` and `P2` be the s–v and v–t subpaths … Among all vertices in `P1` that are at least T away from v, let x be the closest to v … We say that `P_v` **passes the T-test** if the portion of `P_v` between x and y is a shortest path." Lemma 1: passes ⇒ T-LO. Lemma 2: fails ⇒ not 2T-LO. "This test is very efficient: it traverses `P_v` at most once and runs a single point-to-point shortest-path query."
- **Plateaus are the linear-time cousin.** Lemma 4: "If P corresponds to a plateau v–w, P is `dist(v,w)`-LO", and "all plateaus can be detected in linear time" — but plateaus only certify *between* the two trees of one bidirectional search, so they attach to the return leg, not the Dijkstra-tree forward leg. And the paper's own verdict on the plain-BD versions: "neither method is fast enough for continental-sized road networks" (§4).
- **Reference cost at scale:** on Europe (18 M vertices, 42 M edges) with `ε = 25 %`, `γ = 80 %`, `α = 25 %`, the shipped variant x-REV found an alternative in 91.3 % of queries in **20.4 ms**, "4 to 12 times slower than a simple P2P query" (§7, Tables 1–2).

**Fit verdict.** A **T-test against a length/time reference metric, applied once per built leg**, costs one extra point-to-point query per leg. At K=12 that is 12–24 extra queries per fill. Against today's `astar` p50 of 230 ms per return leg, a plain (unpenalised, non-excluded, and therefore *much* cheaper) P2P probe between two points ~T apart is a small fraction of that — but it is not free, and it only **detects**; the repair (re-route the offending window, or reject and refill) is extra. **Ship it as a Defect-Gate detector before considering it as a search constraint.**

### 4.3 The costing-level leash: make a ring never pay

This is the cheap structural answer, and the fork is the only place in Valhalla where the guard is missing.

**Upstream already has the pattern, twice.** (i) `min_linear_cost_factor_` — "once all cost factors are filled, sort by range, precompute overall average and **store the overall minimum factor so it won't mess with the A\* heuristic**" (`dynamiccost.cc:239-244`). (ii) A config-supplied floor on user discounts — `e->set_factor(std::max(line.cost_factor(), min_allowed_factor))`, "apply the minimum allowed value specified in the config" (`route_action.cc:287-288`). **The curvature discount is subject to neither.**

Three implementable shapes, in increasing invasiveness:

| option | change | effect on the ring budget (c0.8) | effect on A\* admissibility | calibration impact |
|---|---|---|---|---|
| **A. Floor the factor** | `factor = std::max(factor, kMinFactor)` in `EdgeCost`, with `kMinFactor` config-exposed; feed the same constant into `AStarCostFactor()` the way `min_linear_cost_factor_` is fed | `kMinFactor = 0.65` → detour budget **2.30× → 1.31×** | restored to the same 1.18× mild inadmissibility as stock | **truncates** the top of the curviness scale — c0.8 and c1.0 converge |
| **B. Shift the table non-negative** | replace `kCurvatureFactor[c]` (0 … −0.30) with a *straightness penalty* `kStraightPenalty[c] = 0.30 − 0.30·c/15` so the curvy edge keeps `factor = 0.85` and the straight one pays | `2.30× → 1.56×` at c0.8 (`(0.85+0.48)/0.85`) | **fully restored** — `factor ≥ 0.85` everywhere, exactly stock | preference *ordering* preserved; the *strength* of the preference changes ⇒ needs a curviness re-calibration and `FINGERPRINT_VERSION` bump |
| **C. Ring-aware transition cost** | penalise a turn that re-enters a road within `R` metres of a node already on the path | none on the cost ratio; kills the *shape* directly | none | none, but needs per-search state (a node/geometry set on the label chain) — the deepest change |

**Option B is the theoretically clean one** — it converts a discount into a penalty, which is exactly what makes an A\* heuristic admissible again, and it is the only option that gives a *bounded* worst-case detour ratio derived from the table rather than from a tuning constant. It is also the one that most obviously changes what a rider gets, so it belongs in the #43 grilling, not in a patch. *(Evidence: inferred — the arithmetic is exact from the constants, the rider-visible effect is unmeasured.)*

**Note the interaction with §1.3.** Options A and B *also* fix the inadmissible heuristic, i.e. one change buys both the anti-ring leash and a return leg that is again a certified optimum. Option C fixes neither. The alternative to A/B for admissibility alone is upstream PR [#6257](https://github.com/valhalla/valhalla/pull/6257)'s shape — run plain Dijkstra inside a radius, A\* beyond it — which costs latency and fixes nothing about rings.

### 4.4 The missing meter: a self-proximity / return-to-junction detector

None of the five meters in §1.1 can see a ring. The detector that can is trivial and reuses machinery the harness already has (`metrics.py:366-399` `_grid`/`_shared_mask`, mirrored in the engine at `route_action.cc:1314-1345`):

> **Self-proximity.** On the decoded loop points `p_0…p_n` with cumulative distance `cum_i`: flag every pair `(i, j)` with `cum_j − cum_i ≥ D_min` and `haversine(p_i, p_j) ≤ R`. Report `ring_count`, `max_ring_km` (the excursion length `cum_j − cum_i`), and `ring_len_fraction`. `D_min ≈ 800 m`, `R ≈ 60–80 m` are the natural starting constants (`kStemCorridorRadiusM = 40 m` is the existing corridor radius; a ring rejoins a *junction*, which is wider than a corridor).

This one meter covers **three** of the five suspected residual classes at once: **ring-detour lollipops** (short `D_min`, both `i,j` in one leg), **figure-8s** (the self-intersection of a Second Via shows up as a proximity pair with a large `cum` gap), and **dual-carriageway out-and-backs** (two parallel carriageways are within `R` of each other over a run, with opposite travel direction — add a bearing test `|bearing_i − bearing_j| ≈ 180°` to separate them from a legitimate re-crossing). Cost: `O(n)` with a grid hash, i.e. the same order as the existing `stem_fraction` decode. **This is the highest value-per-line item in the whole survey and it belongs to #38/#41, not to v4's algorithm.**

Caveat to design around: a legitimate loop *does* re-approach its own start, and mountain switchbacks legitimately fold within `R`. The `D_min` floor and an exemption for the start disk (the existing `kStartExemptionMeters = 1500`) handle the first; switchbacks need the bearing test or a curvature-aware exemption. *(Evidence: inferred design; must be calibrated on corpus-v1 before it can gate anything.)*

---

## 5. Products not yet read from primary sources

*Retrieval note: Calimoto's and komoot's help centres are Cloudflare-gated and were read through the vendors' own Zendesk Help Center JSON API (`support.<vendor>.com/api/v2/help_center/…`), which returns the article body as published. cycle.travel is likewise gated and was read via Wayback snapshots of the first-party pages (original URL + snapshot both cited). All GitHub facts come from `gh api` against the upstream repositories.*

### 5.1 Headline: three of the five vendors publicly admit the defect classes v3 is fighting

| product | seed / direction | distance param | anti-reuse mechanism | vendor statement on defects |
|---|---|---|---|---|
| **Calimoto** | `Direction` + a "Random" mode (mechanism not published) | 50–500 km | **not published** | none |
| **cycle.travel** | not published | not published (user picks the far point) | not published; CH-based | **yes** — "sometimes it won't be different" |
| **BRouter** | `roundTripStartDirection`, else random or terrain-scored | `roundTripDistance` = **radius**, default 1 500 m | **none** — points on a circle | **yes** — a quality rewrite was merged and reverted; "detours and road reuse", "overshoots in length" |
| **komoot** | none | none | **none** — user waypoints only | **yes** — "this often results in an out-and-back route" |
| **GraphHopper / ORS** | `round_trip.seed`, `heading`, ±10 % jitter | `round_trip.distance` (GH default 10 km) / ORS `length` | `AvoidEdgesWeighting` **soft ×5**, per leg | two in-source TODOs, both still unfixed |

**Take: the fork is not behind the field on loop shape — it is ahead of every product whose method is public, and the two vendors closest to us both ship the exact defects riders are reporting here.**

### 5.2 BRouter — the most valuable finding in this section: a v3-shaped quality rewrite was merged and reverted **on latency**

Round trip is a genuine upstream server-side feature (`engineMode = 4`), added in [PR #759](https://github.com/abrensch/brouter/pull/759) (merged 2025-03-31) and shipped in [v1.7.8](https://github.com/abrensch/brouter/releases/tag/v1.7.8) (2025-07-12, release note: "round trip function (engineMode = 4)"). Documented parameters ([`docs/developers/android_service.md:128-133`](https://github.com/abrensch/brouter/blob/master/docs/developers/android_service.md)): `roundTripDistance` — "radius to the round trip points in meters (default 1500)" — and `direction` — "initial round-trip bearing; use a fixed angle for reproducible loops, -1/random otherwise".

**The shipped algorithm on master (`d5c0a75d29b5`, 2026-08-31) is a bare circle**: `RoutingEngine.doRoundTrip()` (`:517`) → `buildPointsFromCircle()` (`:554`), which places `roundTripPoints` (clamped 3..20) points on a circle of `searchRadius` around the start at bearings `startAngle − (90 − 180·i/points)` and routes through them. `getRandomDirectionFromData()` (`:569`) probes four 90° sectors with `AreaInfo` when the profile sets `consider_elevation`/`consider_forest`/`consider_river`, else `Math.random()*360`. There is **no anti-reuse mechanism at all** in the round-trip path, and no round-trip profile in `misc/profiles2/`; the only reuse control BRouter documents is the manual `alternatives` feature ("you are planning a roundtrip and don't want to go back the same way … BRouter can calculate alternatives", [`docs/features/alternatives.md`](https://github.com/abrensch/brouter/blob/master/docs/features/alternatives.md)). `allowSamewayback` (`:527`) is a deliberate out-and-back mode.

**And then the interesting part.** [PR #903 "Improve round-trip routing quality"](https://github.com/abrensch/brouter/pull/903) was **merged 2026-07-08** with, by its own description, a `GreedyRoundTripPlanner` (legs routed by real Dijkstra rather than beelines), an `IsochroneCandidateProvider`, a quality model comprising `ReuseClassifier`, `LoopQualityMetrics` and `CorridorOverlapIndex` ("parallel same-corridor detection"), and an `OsmPath` **anti-reuse refTrack penalty** gated behind a round-trip flag on `RoutingContext`. That is, independently, **most of v3's shortlist** — isochrone-derived candidates, a reuse classifier, a corridor-overlap meter, and a penalty leash. It was **reverted three days later** by [PR #945](https://github.com/abrensch/brouter/pull/945) (merged 2026-07-11, commit `84c4a47a5204`, 124 files). The stated reasons, from the maintainers' own comments:

- **latency**: "old logic 8–15 s vs new 26–67 s for ~180 km" (PR #944 body) — a **~3× regression at a distance comparable to our 200 km band**;
- Android API-23 incompatibility (`computeIfAbsent`, `putIfAbsent`, `sort`);
- devemux86: "old round trip algorithm must remain as option, especially as the new one is not production ready"; afischerdev: "Go back before #903, fix the other problems, publish, and restart roundtrip improvements with a clear situation."

Follow-ups #943, #944, #954 and #911 were all closed unmerged. Contributor jonnybbb names our defect classes exactly (2026-07-10, on #903): "Kurviger also has some detours and road reuse I would expect Brouter AUTO to resolve to get a higher quality loop", and "placement of waypoints in non-reachable segments, detours, overshoots in length".

**Why this matters for #43.** It is the only *independent, dated, public* measurement of what a quality-first loop rewrite costs in latency on a comparable engine: **≈3×, and it got reverted for it**. Our 1.10× ratchet is stricter than the bar BRouter failed. It is direct evidence that the v4 direction must be chosen on the *latency* axis first, and it is the strongest external argument for preferring shortlist item 4 (Suurballe, plausibly latency-negative) over any candidate-and-filter family. *(Evidence: documented — PR bodies, merge/revert commits, maintainer comments.)*

Client side: Bikerouter (a BRouter-Web fork) exposes radius 1–100 km and direction in 15° steps with a published radius→length table (5 km → 25–35 km; 20 km → 100–125 km, [docs.bikerouter.de](https://docs.bikerouter.de/en/roundtrip-planner/)); it documents no road-reuse avoidance. `nrenner/brouter-web` and the OsmAnd org have no `roundTrip`/`engineMode` hits.

### 5.3 GraphHopper / OpenRouteService — unchanged since 2016, and the two TODOs the v3 survey quoted are still open

**GraphHopper master `d9506cd7d36d`, 2026-09-03.** The mechanism is exactly as the v3 survey described (§2 there): `RoundTripRouting.Params` with `round_trip.distance` (default 10 000 m), `round_trip.seed` (default 0) and `round_trip.points` (default `min(20, 2 + distance/50 000)`); `MultiPointTour` bearings `initialHeading + 360·i/allPoints` with ±10 % distance jitter (`TourStrategy.slightlyModifyDistance`); geometric projection via `DistanceCalcEarth.projectCoordinate` with a ×0.95 shrink-and-retry on snap failure; anti-reuse by `AvoidEdgesWeighting.setEdgePenaltyFactor(5)` accumulating `previousEdges` per leg, i.e. **a soft ×5 multiplier, so reuse wins whenever the alternative costs more than 5×**.

**The delta the ticket asked for is: nothing.** Commits since 2026-07-01 touching `RoundTripRouting.java`, `MultiPointTour.java`, `TourStrategy.java`, `AvoidEdgesWeighting.java`: **zero, on all four**. Their last *functional* commits are `b8a010ee3973` (2023-02-07), `130870905667` (2016-06-06, both tour files — 2018's `e9545c102a3c` was formatting only) and `924cb98b9ba0` (2021-04-19). All 50 commits on master since 2026-07-01 were enumerated; the only one touching `Router.java` (`e6a535bc40be3781`, 2026-08-26) contains no round-trip lines. No PR since 2026-01-01 concerns round trips. **Both TODOs the v3 survey quoted are verbatim on master today** — `RoundTripRouting.java:70` "todo: no snap preventions for round trip so far" and `:119-120` "Later: remove potential route tail, maybe we can just enforce the heading at the start and when coming back". The only open round-trip issue is [#2050](https://github.com/graphhopper/graphhopper/issues/2050) (opened 2020-05-31, "Roundtrip feature does not properly work with CH") — open six years.

**ORS is a thin pass-through, now confirmed at source level** (the v3 survey inferred GH lineage but did not verify it). `RouteRequestRoundTripOptions.java:26-47` exposes exactly three fields — `length` ("a preferred value, but results may be different"), `points` ("Larger values create more circular routes"), `seed`. `RoutingRequest.computeRoundTripRoute()` (`:556-616`) copies them into `Parameters.Algorithms.RoundTrip.{DISTANCE,POINTS,SEED}` hints, passes `bearings[0]` as the GH heading, notes "Roundtrip not possible with preprocessed edges", sets `req.setAlgorithm(Parameters.Algorithms.ROUND_TRIP)` and calls `routingProfile.getGraphhopper().route(req)`. **ORS contributes no round-trip logic of its own and inherits every GraphHopper defect verbatim.** Server cap `maximum_distance_round_trip_routes` defaults to 100 000 m — **a third of our 300 km clamp**. One behavioural detail worth knowing: because ORS *always* sets `POINTS` explicitly (default 2, `RouteSearchParameters.java:60-62`), GraphHopper's distance-adaptive point count never fires on ORS — a 100 km ORS round trip gets **2** via points where native GraphHopper would use 4. That is a plausible mechanical explanation for the 8–15 % distance deviation Lewis & Corcoran measured on ORS versus 2–8 % for their own local search (v3 survey §2/§4). Commits touching either ORS file since 2026-07-01: **none** (ORS HEAD `de0edcc23a4e`, 2026-09-02).

### 5.4 Calimoto — parameters published, algorithm not

The motorcycle-market comparator publishes its **user-facing surface only**. ["How Do I Plan a Round Trip?"](https://support.calimoto.com/hc/en-us/articles/7989918956572-How-Do-I-Plan-a-Round-Trip) (updated 2026-09-03) lists exactly four inputs — `Length: Anywhere from 50km (30mi) up to 500km (300mi)`, `Starting Point`, `Direction.` (the whole bullet), `Routing profile: winding or twisty` — plus a fully-random mode ("just enter a starting point and keep the other options on 'Random'"). The German original matches (`Länge`, `Startpunkt`, `Himmelsrichtung`, `Routingprofil (kurvig oder superkurvig)`).

Curvature weighting is described only in the abstract: ["Our Twisty Roads Algorithm"](https://support.calimoto.com/hc/en-us/articles/10514787546908-Our-Twisty-Roads-Algorithm) says it uses "the curviness of the roads, the level of urbanization, the condition of the road surface, and many other parameters" — no mechanism. Four profiles are published (["Our Routing Profiles"](https://support.calimoto.com/hc/en-us/articles/9952806702492-Our-Routing-Profiles): Fastest with highways / Fastest without / Winding / Twisty, Winding by default), and the Calimeter scores curve intensity by "the time spent carving through corners, rather than just looking at the distance traveled" for routes > 20 km.

**UNVERIFIED — anti-reuse, waypoint placement, and seeding are not published.** A search of their Help Center for `round trip`, `same road`, `curvy`, `winding`, `algorithm`, `routing profile` returns no statement about avoiding riding the same road twice, avoiding out-and-backs, or how the loop's points are placed. There is no Calimoto engineering blog. **Two useful negatives for us: (i) their range tops out at 500 km, i.e. they solve a superset of our 20–300 km band; (ii) "Direction" as a first-class input is the same lever as our bearing sectors — nobody in this market has found a better user-facing control.**

### 5.5 cycle.travel — one honest sentence, and a widely-repeated misattribution corrected

cycle.travel's own help page ["Round-trips"](https://cycle.travel/advice/map) ([snapshot](http://web.archive.org/web/20260412172049/https://cycle.travel/advice/map)):

> "Journeys don't have to be A–B: you can plan circular round-trips too. Choose your start and end points as per usual, then click 'Round-trip'. cycle.travel will **try** to find you a different return journey. (Note that sometimes it won't be different, particularly on short journeys or in areas with few roads.)"

That parenthetical is a first-party admission of exactly the Fallback-Loop regime A2 measured here — **short distances in sparse networks**, where the fork sees ~8.4 of 12 candidates fall back at 20–50 km against 3.1 at 300 km. Independent corroboration that this class is network-structural, not a fork bug. The same page documents a separate "Suggest a ride" feature returning "Up to three circular routes". Routing basis is first-party too: "the maths behind cycle.travel's super-fast routing algorithm, it's known as Contraction Hierarchies" and "the speed comes from precalculating all the best routes" ([FAQ](https://cycle.travel/advice/map/faq)) — notable because CH and round trips are documented as incompatible in GraphHopper, so cycle.travel is doing something CH-compatible that is **not published**.

**Correction on the record.** The sentence "tries to get you back to your starting point using different roads from the outward part of the journey" is widely attributed to the developer; it is **not his**. In [cycle.travel/post/6044](https://cycle.travel/post/6044) ([snapshot](http://web.archive.org/web/20260905131344/https://cycle.travel/post/6044)) it is posted by an ordinary forum user (Martin Fox, 2024-05-29). Richard Fairhurst's only post in that thread (2024-06-02) is about UI. Checked and empty: [github.com/systemed](https://github.com/systemed) (34 repos, the routing engine is not among them — the only cycle.travel repo is `cycle.travel_translations`), his [OSM diary](https://www.openstreetmap.org/user/Richard/diary) (20 entries, none on routing or round trips), and the cycle.travel `/news/` archive (64 slugs, none on round trips or "Suggest a ride"). **UNVERIFIED: seed, direction, anti-reuse and the "Suggest a ride" method. No first-party description of the loop algorithm exists.**

### 5.6 komoot — the round trip is not a loop generator, and they say so

["Change the route direction and type"](https://support.komoot.com/hc/en-us/articles/10207909543066-Change-the-route-direction-and-type) (updated 2026-09-04), verbatim:

> "Round Trip ( ) routes bring you back to your starting point using the **fastest route** for your selected sport. With only a start point and destination, **this often results in an out-and-back route**. By adding waypoints, you can shape the route into a loop, create a different return route, or manually create a round trip from scratch."

That is komoot stating in current first-party docs that their "round trip" is A→B plus a fastest-route return, that it **often degenerates to an out-and-back**, and that loop shaping is delegated to the user. The older ["Planning round trips"](https://support.komoot.com/hc/en-us/articles/360024590552-Planning-round-trips) article (2022-12-16) confirms the model: the user must supply the far point — "This point should be placed the furthest away from your starting point along the route you want to plan" — then flip Route Type from `One way` to `Round trip`. **No distance parameter, no direction parameter, no seed, no anti-reuse mechanism, no engineering blog. NOT PUBLISHED — because there is essentially nothing to publish.**

### 5.7 What this section changes for v4

1. **No competitor has solved the ring-detour or dual-carriageway class either** — nobody even publishes a detector. The fork's Defect Gate (build → inspect → refill) is, as far as public sources go, unique in this market.
2. **BRouter's #903/#945 is the field's only dated latency measurement for a quality-first rewrite: ~3× at 180 km, and it was reverted for it.** Our ratchet is 1.10×. **This is the single most important external input to #43.**
3. **komoot's and cycle.travel's admissions independently corroborate the Fallback-Loop class as network-structural**, which supports serving a *tagged* dirty loop (a #40 product question) rather than engineering it away in sparse cells.
4. **Nothing in this section supplies a technique v4 could adopt.** Everything public is the via-point + soft-penalty family the v3 survey already mapped, minus the guards the fork already ships. The delta the ticket hoped for in "products" is empty; the delta is in §2 (Suurballe) and §4 (the anti-ring toolbox).

---

## 6. Valhalla upstream since 3.8.2 — what v4 can ride, and what it must rebase over

Read from `github.com/valhalla/valhalla` releases, `CHANGELOG.md` at master, and the `3.8.2…master` compare. The fork is at `3.8.2 + 54 commits` (`b4f514d7f`); upstream master at the time of writing was `7f372987b` (2026-09-02), **34 commits / 297 files ahead of 3.8.2**.

**Releases after 3.8.2: exactly one.** [3.8.3](https://github.com/valhalla/valhalla/releases/tag/3.8.3), published 2026-07-25 (CHANGELOG header says "Release Date: 2026-07-24"). No 3.9.x, no prerelease; `valhalla/valhalla.h` at master is still `3/8/3`.

**The window is empty for everything v4 cares about.** In `compare/3.8.2...master` the following are **unchanged**: `src/thor/alternates.cc`, `valhalla/thor/alternates.h`, `src/thor/bidirectional_astar.{cc,h}`, `src/thor/unidirectional_astar.cc`, `src/thor/dijkstras.cc`, `valhalla/thor/dijkstras.h`, `valhalla/thor/pathalgorithm.h`, `valhalla/sif/edgelabel.h`, `src/thor/route_action.cc`, `src/thor/worker.cc`. Grepping the post-3.8.2 changelog for `bidirection|astar|alternat|local optim|exclude|avoid|hierarchy|disjoint` returns nothing.

**Specifically:**

- **(a) Local optimality is still not implemented upstream.** `validate_alternate_by_local_optimality` on master is still `// [TODO] NOT IMPLEMENTED` and `kAtLeastOptimal` is still commented out. No PR since 2024 touches `alternates`. **v4 cannot wait for upstream on this.**
- **(b) Hierarchy limits: no change.** The customizable-limits lineage (#5010 in 3.6.0, #5080 in 3.6.1, #5812 in 3.6.3, #3156 in 3.1.3) is all pre-3.8.2 and already in the fork (`proto/descriptors/options.proto:352`, `dynamiccost.h:1034 RelaxHierarchyLimits`, byte-identical to master).
- **(c) Programmatic edge exclusion: unchanged, but one open PR is directly useful.** `AddUserAvoidEdges` / `IsUserAvoidEdge` / `user_exclude_edges_` are identical to the fork. Open PR [#6229](https://github.com/valhalla/valhalla/pull/6229) ("Fix/snap away from user excluded edges", opened 2026-07-22) adds a per-location `search_filter.exclude_avoided_edges` and — the load-bearing part — **syncs resolved exclude edges into the costing instance loki uses for correlation via `AddUserAvoidEdges`**, which is empty at correlation time today. If v4 ever re-snaps a location after excluding a corridor (the Second Via's `correlate_node` does exactly this), this is the pattern, and it is cherry-pickable now.
- **(d) No round-trip/loop feature upstream, but there is a dead spike worth reading.** PR [#6042](https://github.com/valhalla/valhalla/pull/6042) "Spike: alternates from unidirectional A\*" (opened **and closed** 2026-04-19, unmerged, +1937/−21) teaches `UnidirectionalAStar::GetBestPath` to return `1 + options.alternates()` paths by two mechanisms: **plateau** (keep expanding past the first destination-edge pop) and **penalty rerun** (seed an edge-penalty set from accepted paths, clear, re-run; penalised edges ×3, up to 4 reruns). Its file list (`test/gurka/test_twisty_roads.cc`, `src/sif/motorcyclecost.cc`, motorcycle curvature) shows a third party working our exact problem. It is **not upstream capability** — a closed draft — but it is the nearest prior art for a penalty-rerun loop family and worth reading before #43. *(Also: upstream issue #5101 "Optimized Route Round Trip" was a user question, closed same day, no code.)*
- **(e) One PR is a design signal for §1.3.** Open PR [#6257](https://github.com/valhalla/valhalla/pull/6257) adds `thor.costmatrix.dijkstra_distance` — disable the A\* heuristic up to a distance threshold — upstream's own admission that the heuristic makes results constellation-dependent, with "Dijkstra inside a radius, A\* beyond it" as the fix.

**Rebase hazards for a v4 branch** (the fork's delta vs 3.8.2 is 19 files / +3317 / −45):

| file | upstream churn | assessment |
|---|---|---|
| `src/sif/motorcyclecost.cc` | **#6214** ("Add `use_distance` to truck, bus, taxi, motorcycle and motor_scooter cost", merged 2026-08-19) | **Certain conflict.** It rewrites the exact last line of `EdgeCost` (`return {sec * factor, sec};` → `return Cost((sec * inv_distance_factor_ + edge->length() * distance_factor_) * factor, sec);`) and appends `c.cost *= inv_distance_factor_;` to both transition functions. **Numerically a no-op at the default `use_distance = 0`** — textual conflict only, no FINGERPRINT bump needed provided `use_distance` stays 0. |
| `valhalla/sif/dynamiccost.h` | **#6214** (+12) | Likely conflict, mechanical — new `kInvMedianSpeed`, `distance_factor_`/`inv_distance_factor_`, `use_distance_`; adjacent to but not overlapping the fork's `EdgeFactor`/`mark_edges_used`/`mark_rejoin_edges` block. |
| `proto/descriptors/options.proto` | **#6251** (`expansion_index = 9`) | Safe today: upstream's max field in `Costing.Options` is 97, the fork occupies 98–101 (`prefer_curvature = 101`). **Forward risk: the fork is squatting the next four field numbers upstream will hand out.** |
| `valhalla/baldr/graphtile.h` | **#6237** (`GraphTile::GetTileId` → `GraphId::FromTilePath`) | Mechanical rename; grep fork tools before rebasing. |
| root `CMakeLists.txt` | **#6210** (translations via `.po`, merged 2026-08-20) | **New build hazard:** a hard `find_program(PYTHON_INTERPRETER …)` + `FATAL_ERROR` and a `polib` dependency, with all `locales/*.json` removed. A minimal build image without python3 now fails at configure time. Upstream is walking it back in open PR [#6303](https://github.com/valhalla/valhalla/pull/6303). **Do not rebase past `49cd28bc5` until #6303 lands.** |

**Confirmed non-hazards:** no `GetBestPath` signature change; `Dijkstras` API intact (so `RoundTripExpansion : public Dijkstras` rebases untouched); `BDEdgeLabel` layout stable; tile format headers unchanged; no C++ standard bump (still C++20). **Watch-list:** [#6232](https://github.com/valhalla/valhalla/pull/6232) (C++20 bitfield init in `directededge.h`/`nodeinfo.h` — next to the fork's curvature bits) and [#5983](https://github.com/valhalla/valhalla/pull/5983) (reverse speed limit in `EdgeInfo` — **tile-content change ⇒ FINGERPRINT territory**).

---

## 7. Ranked shortlist for the "choose the v4 direction" grilling (#43)

"Structural" = the defect class cannot occur where the mechanism applies. "Probabilistic" = it becomes less likely. Cost is relative to today's fresh-build class of **p50 ~0.6 s / p95 ~2.5 s** at 300 km × K=12 (ADR-0037 §6), under the 1.10× rig ratchet with a measured **±12 % p50 noise floor** at `workers=1` (A1).

1. **Bound the curvature discount (anti-ring leash).** *Defect class:* **ring-detour lollipop** — plus, as a side effect, restores A\* admissibility on the return leg (§1.3). *Structural* for "a detour can never be net-cheaper than the straight edge", once the bound is chosen. *Fork hook:* costing — `MotorcycleCost::EdgeCost` factor floor **or** a non-negative curvature table, and `AStarCostFactor()`/`min_linear_cost_factor_` fed the same constant. *Cost:* **~0** (one `std::max`, or a table edit). *Evidence:* the mechanism is **inferred** from exact constants; the rider-visible cost of truncating the curviness scale is **unmeasured**. *Blocked on:* #41 proving rings exist and at what rate; #43 deciding whether curviness may be re-calibrated (⇒ `FINGERPRINT_VERSION` bump).
2. **Close the shortcut hole in the hard exclusion.** *Defect class:* **reuse** (and any lollipop that rides a laundered corridor). *Structural.* *Fork hook:* return leg — mirror upstream's `route_action.cc:308-327` in `route_leg`: for each corridor edge add `reader->GetShortcut(e)` if valid, and for a shortcut add `RecoverShortcut(e)`'s constituents. *Cost:* `GetShortcut` is flagged upstream as "an expensive operation, since we need to expand the graph a little" (`route_action.cc:322-323`) — `O(path)` graph pokes per candidate, on the order of the existing rejoin-marking pass (`rejoin_ms` is already instrumented). Estimate **+2–8 ms per candidate**; must be measured. *Evidence:* the hole is **code-proven**; its contribution to `edge_reuse_geom = 0.059` is **unmeasured**.
3. **Self-proximity / return-to-junction meter (the missing detector).** *Defect class:* **ring-detour lollipop + figure-8 + dual-carriageway mirror**, all three, as *measurement*. *Not a fix* — it is the instrument #38/#41/#42 need before any fix can be gated. *Fork hook:* harness first (`metrics.py`), engine Defect Gate second (post-build decode already exists). *Cost:* ~ms, `O(n)` on an existing grid hash. *Evidence:* **inferred** design; constants uncalibrated. **Do this first regardless of which direction #43 picks.**
4. **Suurballe–Tarjan disjoint-pair loop construction.** *Defect class:* **seam spike, start-stem lollipop, same-road reuse** — all structural in the vertex-disjoint variant; plus it removes the Fallback Loop *guessing* by deciding returnability before any search (§2.4). *Does not* cover ring detours or dual-carriageway mirrors. *Fork hook:* **new pass** replacing harvest-select-then-return-A\* with one Dijkstra-like second pass over the harvest region plus `O(path)` per-candidate construction. *Cost:* estimated **−30 % to −50 % of p50** (§2.7) — the only shortlisted item that might *fund* the ratchet rather than spend it. *Evidence:* the algorithm is **documented** (paper read in full, complexity and correctness quoted); the fit and cost are **inferred** and must be prototyped. *Open risks:* the pair-vs-cycle orientation problem (§2.3b), per-level node splitting (§2.3a), and the loss of per-candidate xcand penalties (a single pass cannot carry a *growing* cross-candidate penalty — see §10).
5. **Restore A\* admissibility on the return leg.** *Defect class:* none directly — it restores the *guarantee* the v3 survey assumed, so that "in-leg detours are impossible" becomes true rather than hoped, and it makes the return leg as curvy as its cost model intends. *Fork hook:* costing (subsumed by item 1's option A/B) **or** search (upstream #6257's "Dijkstra inside a radius"). *Cost:* free if it comes with item 1; a real latency cost if done by disabling the heuristic. *Evidence:* the inadmissibility is **code-proven + arithmetic**; the behavioural consequence is **unmeasured** and is a #39 question.
6. **Forbid the Second Via's opposite half-sector.** *Defect class:* **figure-8**. *Structural* for the two-lobe shape, at the cost of a weaker lollipop remedy. *Fork hook:* one constant — `kSecondViaSectorDeg = 180.0f` (`route_action.cc:1031`) admits any `V2` ≥ 90° from the turnaround; narrowing it to a ~60–90° window *adjacent to* the turnaround produces a fat single loop instead of two lobes. *Cost:* **0**. *Evidence:* **code-proven** shape argument; figure-8 rate **unmeasured** (needs item 3).

**Reading for #43.** Items 1–3 and 6 are v3.x patches: they are cheap, they do not change the pipeline's shape, and three of them are *measurement or constants*. Item 4 is the only genuine **new structural family** on the table, it is the only one that is plausibly latency-positive, and it is the honest replacement for the v3 survey's rejected Greedy Faces (same "sharing ≡ 0 by construction" ambition, on a directed graph, with a `O(m log n)` published algorithm instead of a planarisation pass). **The recommended framing is hybrid: patch 1+2+3+6 now, prototype 4 as the v4 direction.**

**And a warning from the field (§5.2).** The one comparable public attempt at a quality-first loop rewrite — BRouter's PR #903, which independently reinvented most of v3's shortlist — was reverted after three days for a **~3× latency regression at ~180 km**. Any v4 candidate that works by *generating more and filtering* (extra legs, extra searches, candidate-and-reject loops) walks the same path. Item 4 is attractive precisely because it works the other way: it replaces K bidirectional A\*s with one extra Dijkstra-like pass. **If the #43 grilling takes one thing from this survey, take the latency axis seriously before the quality axis.**

## 8. Defect-coverage matrix

Classes: **SS** seam spike · **SL** start-stem lollipop · **RL** ring-detour lollipop (mid-leg) · **DC** dual-carriageway / parallel-road out-and-back · **RU** road reuse · **F8** figure-8 · **FB** Fallback Loop (dirty return).

| # | Technique | SS | SL | RL | DC | RU | F8 | FB | Δ cost @300 km K=12 | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|
| — | **v3 as shipped** | struct. (turnaround) | struct. (same-edge) | **none** | none | struct. on hard-exclude success | none | tagged, not avoided | baseline | measured |
| 1 | Bound the curvature discount | — | — | **struct.** (bounded detour ratio) | reduces | — | — | — | ~0 | inferred |
| 2 | Shortcut-aware hard exclusion | reduces | reduces | — | — | **struct.** (closes the laundering hole) | — | may *raise* FB (fewer legal returns) | +2–8 ms/cand (est.) | code-proven hole, unmeasured effect |
| 3 | Self-proximity meter | detect | detect | **detect** | **detect** | detect | **detect** | detect | ~ms | inferred |
| 4 | Suurballe–Tarjan (vertex-disjoint) | **struct.** | **struct.** | — | — | **struct.** | **struct.** (single simple cycle) | **decided before search** | est. **−30…−50 % p50** | documented + inferred fit |
| 5 | Restore A\* admissibility | — | — | reduces | — | — | — | — | 0 (with #1) or + (with #6257) | code-proven |
| 6 | Narrow the Second Via sector | — | weakens the remedy | — | — | — | **struct.** | — | 0 | code-proven |
| — | *Arc-orienteering metaheuristic (rejected)* | reduces | reduces | reduces | reduces | reduces | reduces | reduces | **+1–2 orders of magnitude** | documented (§3.2) |
| — | *Length-bounded disjoint packing (rejected)* | struct. | struct. | — | — | struct. | struct. | — | NP-hard | documented |
| — | *Exact UBS / local-optimality verification (rejected)* | — | — | **struct.** | — | — | — | — | `O(\|P\|²)` P2P queries | documented |

## 9. Rejected for fit

- **Arc-orienteering / orienteering metaheuristics (GRASP, ILS, ejection chains) on the serving path.** NP-hard; the best road-scale primary numbers are 0.2–1.0 s per query for 5–30-minute paths on a 500 K/1 M network (arXiv:1609.08484 Fig. 10), against our 4–5-hour tours × K=12 on 2 vCPU where a 12-way burst already costs 4.6× (C1). Keep only as an **offline reference optimum** for Gate v2. *(This is the v3 survey's "Option C" rejection, now with a measurement basis.)*
- **Lagrangian relaxation of the length constraint.** Replaces one constrained problem with a sequence of unconstrained ones plus a k-shortest-paths gap closure; the fork already controls length more cheaply via the harvest band + one-shot correction, and the disjoint-path machinery is better used *inside* that band than as a solver for it (§2.6).
- **Length-bounded disjoint-path packing (Short Path Packing).** NP-hard even to decide (arXiv:2404.10469); FPT in `k` and `ℓ`, and our `ℓ` is 300 000 m.
- **Bhandari's negated-arc method.** Same objective as Suurballe–Tarjan, achieved with negative arcs and Ford–Bellman at `O(nm)` instead of a potential transform at `O(m log n)`, and with no single-source bonus (Suurballe & Tarjan §IV, p. 335). No reason to prefer it. Algorithmic detail **UNVERIFIED** at primary-source level.
- **Exact in-leg UBS / local-optimality verification.** `O(|P|²)` point-to-point queries per path by the authors' own account (SEA 2010 §7); still an open problem in their conclusion; still `NOT IMPLEMENTED` in Valhalla master. Use the T-test approximation instead, and only as a detector.
- **Plateau-based loop construction.** Plateaus certify local optimality between the two trees of *one* bidirectional search (Lemma 4), which does not attach to a Dijkstra-tree forward leg; and the plain-BD plateau method is "not fast enough for continental-sized road networks" (SEA 2010 §4).
- **Greedy Faces (re-affirmed).** The v3 survey's reasons stand unchanged (planarisation, undirected assumption, macro-face length overshoot). **Suurballe–Tarjan's vertex-disjoint pair is the directed-graph substitute for the same structural ambition, and it exists as a published `O(m log n)` algorithm.**
- **k-disjoint paths with distinct endpoint pairs.** The general two-paths problem in a digraph — "Are there two vertex-disjoint paths, one from `s1` to `t1`, the other from `s2` to `t2`?" — is NP-complete (Fortune, Hopcroft & Wyllie, "The directed subgraph homeomorphism problem", *Theoretical Computer Science* 10 (1980) 111–121 — spelled "Wylie" in Suurballe & Tarjan's reference list [3], where it is quoted as "the following two-paths problem, which is NP-complete" and used for a reduction, §IV, p. 335). Our problem is the *same-pair* case, which is the tractable one — do not generalise past it.

## 10. Open questions the audit (#39) and the census (#41) should answer

1. **Do ring detours actually occur, and in which leg?** §1.2–1.3 predicts they concentrate in the **forward** (exact-Dijkstra) leg and are suppressed in the return leg by the inadmissible heuristic. The self-proximity meter (§4.4) plus a per-leg breakdown settles it. **If the prediction fails, item 1 drops out of the shortlist.**
2. **How much of the residual `edge_reuse_geom = 0.059` is shortcut laundering?** Count, per return leg, how many of its `FormPath`-recovered base edges were in the `hard` exclusion set. A one-line counter answers a code-proven hypothesis (#39).
3. **Is the return leg actually suboptimal?** Re-run a sample of return legs with the heuristic disabled (or with `factor` floored at 1.0) and compare paths byte-for-byte. A1 showed `threshold_delta` is inert; this distinguishes "sharp penalty optimum" from "heuristic-bound".
4. **What fraction of turnarounds have no *road-disjoint* return at all?** This is Suurballe's existence condition (§2.4) and it should reproduce A2's fallback frequencies (8.4/12 at 20–50 km, 3.1/12 at 300 km). If it does, the disjoint-pair pass is validated as a returnability oracle before any of it is built.
5. **How lossy is the direction-symmetric approximation on Serbian tiles?** Measure: of the forward legs harvested today, what fraction is traversable in reverse (all edges have a legal opposing edge, `GetOpposingEdgeId` valid **and** auto-accessible)? This is the make-or-break number for shortlist item 4 rung A. It can be computed offline from existing saved runs.
6. **Can a single-pass construction keep cross-candidate distinctness?** ADR-0038/0039 ship an xcand penalty at strength 0.2 that *grows as the bank fills* (`bank_edge_count`, `route_action.cc:1602-1605,1673-1681`) — inherently sequential. A Suurballe pass produces all K at once. Options: rounds (re-run the pass with the previous round's edges penalised — the ATMOS-2013 penalty loop at pass granularity), or accept ADR-0040's rejected selection-time filtering with a better metric. **This must be settled in #43, not in execution.**
7. **What does the Start Exemption cost the rider?** A 1.5 km stem in a 20 km loop is 7.5 % of the ride and is exempt from both meters by design (ADR-0037 §3). The census should report stem length *inside* the exemption separately, so #40 can decide whether it shrinks, becomes reported-not-exempt, or gains a cap.
8. **Does the Second Via produce figure-8s, and at what rate per distance band?** `second_via_count` exists; a self-intersection test does not. Needed before item 6 can be justified as anything other than a free precaution.

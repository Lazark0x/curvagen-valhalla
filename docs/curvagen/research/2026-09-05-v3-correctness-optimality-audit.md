# v3 correctness and optimality audit — roundtrip_impl, harvest expansion, sif hooks

- **Date:** 2026-09-05 (curvagen-orchestrator [#39](https://github.com/Lazark0x/curvagen-orchestrator/issues/39), child of round-trip v4 map #36)
- **Scope:** the v3 round-trip pipeline on fork `curvature-costing` @ `b4f514d7f` (Valhalla 3.8.2 base): `src/thor/route_action.cc` (ADR-0037 helpers :992–1458, `roundtrip_impl` :1466–2189), `src/thor/roundtrip_expansion.cc` + `valhalla/thor/roundtrip_expansion.h`, `valhalla/sif/dynamiccost.h` / `src/sif/dynamiccost.cc` (EdgeFactor leash, rejoin grades, user excludes), `src/sif/motorcyclecost.cc`, and the upstream primitives they stand on (`bidirectional_astar.cc`, `dijkstras.cc`, `alternates.cc`, `triplegbuilder.cc`, `loki/worker.cc`, `worker.cc`). Read against ADR-0033/0037/0038/0039/0040, the defect atlas, A1/A2/H1, `tools/loopqual/metrics.py` (v1.3) and `gate_v1_proto.py`, and the prod costing recipe in `curvagen-orchestrator/crates/domain/src/costing.rs`.
- **Method:** read-only code audit. No build, no rig run, no request to api.curvagen.cc, no git state change. Every claim cites `file:line` at `b4f514d7f`. Anything that needs a run is marked **needs rig confirmation** with the gurka test I would write. Two questions: (1) does each mechanism do what ADR-0037 says in every graph situation; (2) does the search find the optimum of its own objective, and does the objective itself permit rider-visible defects. Prod costing assumed throughout: `prefer_curvature = c`, `reuse_penalty 0.8`, `curviness_continuity = c`, `prefer_elevation 0.3`, `top_speed = 120 − 40c` (`costing.rs:17–41`), c ∈ {0.5, 0.8}.

## TL;DR verdict

v3 does what ADR-0037 says for the defect class ADR-0037 defined — the **exact-mirror seam stub** — and its mechanisms are individually sound: hard exclusion is complete (including, by a fortunate accident of plumbing, against shortcuts), bounce rejection is complete for immediate U-turns in every candidate pool, every soft-leash return is tagged, the tile-boundary story is clean, and the output is deterministic per seed. The 0.00 % spike reading is real for that class.

Riders still see spikes and lollipops because two shapes fall outside both the engine's gate and the harness's meters, and the objective actively produces them:

1. **Ring-reversal harvest chains (F01, blocker).** The harvest is an edge-labelled Dijkstra forest: every homeward-pointing edge within `max_meters` gets a label, and its predecessor chain is "the cheapest legal way to be heading home here". Where a side road does not provide that (sparse terrain), it is *ride out to the nearest ring — roundabout, village loop, triangle junction, U-turn slot — reverse, ride back*. Such chains are not bounces (no immediate U-turn), pass the straight-line filter with up to ~35 % of their length retraced, can win node-dedup and the curviness rank *because* the retraced stretch is curvy, are not walked back, and decode with no exact-mirror apex (the apex is a ring). The served loop carries an out-and-back stub with a bulb at its tip, mid-route — which is exactly what a rider calls a lollipop or a spike. The harness reads it only as diffuse `edge_reuse_geom`.
2. **Parallel-carriageway retrace (F02, major).** Exclusion, leash, rejoin, xcand, the seam gate and the harness spike meter are all keyed on directed-edge ids or exact-mirror geometry. The opposite carriageway of a divided road is a different way: the return may ride back 10–30 m from the forward leg, undetected everywhere.

Two further optimality gaps: the return-leg A\* heuristic ignores the fork's discounts and overestimates by up to **1.9× at c0.5 / 2.9× at c0.8** (F03) — the return legs are provably not optimal under the costing (they are greedier and straighter than the objective wants), and the progress-graded rejoin penalty silently skips every junction exit that lives on a hierarchy twin (F04) — the anti-shadow mechanism is absent at precisely the class-mixed rural junctions where stems form. Second Via's two-lobe rebuild is a figure-8 through the start by construction, and its leg C is forced into a no-exclusion Fallback whenever leg B used the start's access road (F06).

## Findings table

Severity: **blocker** = produces rider-visible defects today · **major** · **minor** · **by-design-but-rider-visible (BDRV)**. "Explains?" = does it explain a rider-visible spike or lollipop.

| id | kind | where (file:line) | severity | repro sketch / gurka idea | fix shape | explains? |
|---|---|---|---|---|---|---|
| F01 | optimality gap (objective + harvest) | `roundtrip_expansion.cc:119–133`, `dijkstras.cc:124,205–217`, `route_action.cc:1498–1503,1449` | **blocker** | G1 `RingReversal`: road with a small ring at the far end; assert no undirected edge twice in `legs[0]` — predicted FAIL | reject non-simple chains at harvest (undirected edge revisited in the label chain); v4: edge-simple forward legs by construction | **yes** — bulb-tipped stub mid-route (lollipop) and non-mirror spike |
| F02 | BDRV / objective blind spot | `route_action.cc:1625–1635,1443–1455`; `metrics.py:266–268` | **major** | G2 `DualCarriagewayRetrace` — predicted FAIL | exclusion by physical road (shape buffer / way + bearing), not edge id | **yes** — spike on divided roads, invisible to gate and harness |
| F03 | optimality gap (A\* inadmissible) | `motorcyclecost.cc:291–293,474–483`; `dynamiccost.cc:202,241–244`; `dynamiccost.h:229` | major | numeric bound below; rig: compare bidir return cost vs Dijkstra-exact return on corpus | fold the minimum achievable factor into `AStarCostFactor()` (latency cost) | no (straighter, less curvy returns; not stubs) |
| F04 | bug | `route_action.cc:1650–1671` vs `:1084–1105` | major | G3 `MixedClassRejoin` — predicted FAIL | build the rejoin map with `for_each_junction_edge` | partial — stems via parallel lower-class roads |
| F05 | confirmed-correct (fragile) | `route_action.cc:390–396` vs `:558–582`; `thor/worker.cc:236–244`; `dynamiccost.cc:207–220`; `bidirectional_astar.cc:148–156,202–205`; `dijkstras.cc:131` | n/a | pin: assert no `is_shortcut()` edge in any served leg | if hierarchy limits are ever wired into roundtrip, add shortcut ids to `hard` (`loki/worker.cc:194–207` pattern) | no |
| F06 | BDRV | `route_action.cc:1888–1891,1932–1938,1631,1702–1715` | major | G4 `SecondViaFigure8` | exempt by edge id, not by rebased corridor distance; decide whether via-through-home is acceptable | **yes** — loop passes home mid-ride; leg C fallback drops all exclusion |
| F07 | BDRV (granularity) | `route_action.cc:997–999,1631–1635`; `dynamiccost.h:1081–1099` | minor→major (frequency unknown) | G5 `LongFirstEdge` — predicted FAIL | percent_along exclusion on the edge straddling 1500 m | partial — forced stem → lollipop count |
| F08 | latent bug | `bidirectional_astar.cc:439–504`; `motorcyclecost.cc:411`; `route_action.cc:1860–1861` | minor | G6 `RestrictedTurnBounce` | full decode for all loops, or forbid U-turns in the return search | rare mid-return spike |
| F09 | bug | `route_action.cc:1996–1999` vs `:1880,:2028` | minor | code reading; under-fill in Second-Via-heavy cells | grant the +want budget on the stall branch regardless of who widened | no (under-fill) |
| F10 | bug (minor) | `route_action.cc:1100`; `bidirectional_astar.cc:1014–1024` | minor | `motorcycle=no` exit at a turnaround | use the costing's access mask / `Allowed(edge, tile)` | rare |
| F11 | confirmed-correct | `route_action.cc:1631,997–998`; `metrics.py:327–358,454–456` | n/a | — | — | no |
| F12 | confirmed (doc gap) | `route_action.cc:1702–1718,1970,2126–2128` | minor | — | export the tag (or keep the log contract explicit) | no |
| F13 | confirmed-with-leaks | `route_action.cc:1387–1458,1850–1868` | n/a | leaks = F01, F02, F08 | gate on geometry (self-overlap, shadow), not exact mirror only | see F01/F02 |
| F14 | confirmed-correct | `roundtrip_expansion.cc:119–133`; `route_action.cc:1783–1803,1882,2034` | n/a | — | — | no |
| F15 | by-design (residual) | `route_action.cc:2034–2048,2099–2100` | minor | — | corridor-aware guard (sharing filter is OFF in prod) | no (near-twins) |
| F16 | by-design | `roundtrip_expansion.cc:39–52,60–61` | minor | — | v4: prune radius from target, not a constant | no (long loops stay tertiary+) |
| F17 | confirmed-correct | `roundtrip_expansion.cc:141–153`; `route_action.cc:1085–1096,1122,1208–1214,1651` | n/a | — | — | no |
| F18 | confirmed-correct | `route_action.cc:1505–1509,1546,1678–1684,2121–2122`; `dijkstras.cc:19` | n/a | — | — | no |
| F19 | confirmed-correct | `route_action.cc:1614–1717`; `dynamiccost.h:1266–1302` | n/a | — | — | no |
| F20 | optimality gap (ranking) | `route_action.cc:1866,1981,2149–2150,1546,1554–1584` | major | slot census (below) | rank the built loop (both legs, distance, fallback), not the harvest chain | **yes** — promotes F01 chains into slot 0 |
| F21 | by-design | `route_action.cc:2015–2062`; `roundtrip_expansion.cc:16`; `route_action.cc:1011–1012` | minor | — | distance-aware ranking / two-sided band | no ("asked 100, got 140") |
| F22 | by-design (objective) | `motorcyclecost.cc:474–503,507–578`; `dynamiccost.h:229–238` | n/a | inequality below | — | urban block-rings only |
| F23 | confirmed-correct | `dijkstras.cc:19,760–775` | n/a | — | — | no |

---

## F01 — Ring-reversal harvest chains (blocker)

**The premise to correct first.** The ticket asks about "one label per node, per level". The harvest is not node-labelled: `Dijkstras::ExpandInner` keys `EdgeStatus` by directed-edge id (`dijkstras.cc:124`) and creates one `BDEdgeLabel` per directed edge (`:205–217`). A predecessor chain may therefore pass the same physical junction any number of times as long as it enters through different edges. Hierarchy twins add nothing to this (every road edge lives on exactly one level; twins are node copies linked by transitions that `ExpandInner` walks with the same predecessor, `:247–253`).

**What the tree contains.** `ShouldExpand` prunes only on the predecessor's path distance (`roundtrip_expansion.cc:42–46`), so every directed edge reachable within `max_meters = 1.2 × target/2` gets a label — including every edge pointing *back toward the start* along every road the tree rode out on. Each such homeward edge `e'` at node N is labelled with the cheapest legal way to be traversing it. Under the costing there are two families: (a) arrive at N's far end from a side road; (b) ride out past N to some point where a legal *reversal* exists and come back. Immediate U-turns are only legal at a tile-flagged dead end (`motorcyclecost.cc:411`, `graphenhancer.cc:1355–1356`), and ADR-0037's bounce rejection drops chains containing one (`roundtrip_expansion.cc:119–121, :133` — `opp_local_idx == localedgeidx`). Any *other* reversal is legal and is not a bounce: a roundabout, a village loop road, a triangle junction (a Y whose two arms meet the crossing road 50 m apart), a dual-carriageway U-turn slot, or a block. In sparse terrain, (b) is the only way to label the homeward edges of the sole road out of a valley.

**Why every downstream guard passes it.**
- *Straight-line filter* (`roundtrip_expansion.cc:160–161`, `kMinStraightFraction 0.3`): with `d_out` ridden out, `d_back` retraced, path ratio ρ (straight/path) of the road, the condition `ρ(d_out − d_back) ≥ 0.3(d_out + d_back)` allows `d_back / pd ≤ (ρ − 0.3)/(2ρ)`: 35 % of the chain retraced on a straight road, 31 % at ρ = 0.8, 25 % at ρ = 0.6, 12.5 % at ρ = 0.4. Only roads with ρ < 0.3 are safe.
- *Node-dedup* (`route_action.cc:1498–1503`, best curviness per node): the homeward chain at N and the outbound chain at N (when both are in band) collide, and the homeward one **wins** whenever the mean curvature over (N→J twice + ring) exceeds the mean over S→N — i.e. whenever the road gets curvier beyond N, which is exactly the terrain the curvy-cost expansion is drawn into. The clean candidate at N is dropped in favour of the one carrying the stub.
- *Sector pick and served rank* (`:1539–1546`, `:2149–2150`): `curviness_per_km` (`roundtrip_expansion.cc:168`) is a length-weighted mean, so a curvy stretch ridden twice counts twice — the same promotion the atlas measured for bounced chains ("adds curvy switchback kilometers", atlas §9). Bounce rejection cured it for dead-end bounces; nothing cures it for ring reversals.
- *Walk-back* (`route_action.cc:1214–1219`): the tip node's homeward continuation is a "fresh exit" that leads somewhere branchy → no pop.
- *Return leg*: from N the corridor S→N and N→J (both directions) is barred (`:1625–1635`); if N has a side road the return is a hard-exclude success and the loop is `S→N→J→ring→J→N→(fresh)→S` with the N–J stretch ridden out and back **inside the forward leg**. If N has no side road the primary fails, the Fallback retraces N→S on the leash, and the loop is a pure out-and-back with a bulb at J (the atlas's gallery-B shape with the mirror apex replaced by a ring).
- *Defect Gate* (`:1443–1455`): needs `pts[i-1] == pts[i+1]`; the sequence `…a1, J, r1…rk, J, a1…` has no such index. **Neither the seam-window nor the full decode fires.**
- *Stem check* (`:1258–1380`) measures only the forward prefix against the return suffix; a self-overlap inside the forward leg is invisible. If the Fallback variant retraces all the way home the stem check *does* fire → Second Via rebuilds it two-lobed (F06) — and the stub with its bulb now sits inside a loop the harness reads as clean.

**Harness visibility.** `find_spikes` (`metrics.py:274–279`) is the same exact-mirror test; `corridor_stats` (`:401–481`) is start-side only; only `edge_reuse_geom` (`:327–358`) sees the retrace, as a per-loop number folded into gate 3's *mean* (≤ 0.12 @20 km …) and gate 5's `reuse > 0.30` fraction (≤ 8 %). ADR-0040 records both as *marginal pre-existing fails* (0.1220 vs 0.12; 8.07 % vs 8 %) — consistent with a population of loops that carry a large intra-leg retrace but no mirror.

**Which loops.** For a road S→V with a ring at V (distance d_V) and target T, the homeward labels at nodes N with `d_N ∈ [2d_V − 0.59T, 2d_V − 0.41T]` are in band; with `d_V ≈ 0.35T` that is nodes 15–29 km out on a 100 km ask, retracing 6–20 km each way. Every ring in the annulus 0.65–1.0 × target/2 seeds such a family along its approach road. Flex-band scans (0.55×) widen it.

**Fix shape.** At harvest, reject chains that revisit an undirected edge key (`min(e, opp(e))`) — O(chain) with the same lazy DP that already memoises `curv_sum`; or, stronger, require the chain to be node-simple on physical junctions. v4 must build forward legs that are edge-simple by construction rather than harvest them from a forest that is not.

**Needs rig confirmation** (G1 + census lines 1–2, 6–7). The atlas ran pre-v3, when this class was masked by the 43 % bounce class it sat under.

## F02 — Parallel-carriageway retrace (major)

`route_leg` excludes `e` and `GetOpposingEdgeId(e)` (`route_action.cc:1625–1635`); the leash marks the same pair (`:1641`, `dynamiccost.h:1273–1275`); rejoin grades hang off corridor *nodes* (`:1650–1671`); xcand registers the same pair (`:2109–2114`). For a divided road the opposite carriageway is a separate OSM way with its own nodes: none of these touch it. The return can therefore leave the turnaround via a U-turn slot or roundabout onto the other carriageway and ride back parallel to the forward leg at 10–30 m — physically the same road. The seam-window verdict (`:1443–1455`) sees no mirror (direction-distinct geometry), the harness spike meter documents the same blindness (`metrics.py:266–268`), and the stem meter (40 m corridor) sees it only when it is at the *start* side. A1's finding that hard exclusion gives a "sharp optimum" is exactly why: the search is sharply optimal under an objective that does not know what a road is. Frequency is bounded by where divided roads exist under `avoid_motorways` (urban primaries and trunks near Belgrade/Novi Sad/Niš — the atlas's "flat grid spikes are not mountain-only" line).

## F03 — The return-leg A\* is not admissible under the fork's costing (major)

`AStarCostFactor()` returns `kSpeedFactor[top_speed_] * min_linear_cost_factor_` (`motorcyclecost.cc:291–293`). `min_linear_cost_factor_` is initialised to 1 (`dynamiccost.cc:202`) and lowered only by user linear cost factors (`:241–244`); round-trip requests carry none. So `h = distance × 3.6 / top_speed` — pure time at top speed. The true edge cost is `sec × factor` with `sec = len × 3.6 / min(v_edge, top_speed)` (`motorcyclecost.cc:460–462`) and

`factor = kDensityFactor[d] + highway·kHighwayFactor + surface·kSurfaceFactor + curvature_factor_·kCurvatureFactor[curv] − prefer_elevation·|grade−6|·0.01 (+ SpeedPenalty ≥ 0, + toll)` (`:474–486`)

with `kDensityFactor[0] = 0.85` (`dynamiccost.h:229`), `kCurvatureFactor[15] = −0.30` (`motorcyclecost.cc:111`), `curvature_factor_ = 2c` (`:383`), elevation up to −0.027 (`:480–483`, grade deviation ≤ 9). Minimum factor on a paved, low-density, curvature-15, steep tertiary:

| costing | `top_speed` | `curvature_factor_` | f_min | h/true, edge speed ≥ top | h/true at 60 km/h | admissible only if edge speed ≤ |
|---|---|---|---|---|---|---|
| upstream motorcycle (c = 0) | 120 | 0 | 0.85 | 1.18× | 0.59× | 102 km/h |
| **c0.5 (prod default)** | 100 | 1.0 | **0.523** | **1.91×** | 1.15× | 52 km/h |
| **c0.8** | 88 | 1.6 | **0.343** | **2.92×** | 1.99× | 30 km/h |
| c1.0 (knob max) | 80 | 2.0 | 0.223 | 4.48× | 3.36× | 18 km/h |

Upstream is already mildly inadmissible (the density table); the fork multiplies the overestimate. Consequences in `bidirectional_astar.cc`: expansion order (`:322–323`), the connection threshold `c + 420` (`:894–905`, `kThresholdDelta :25`) and the reach-based prune (`:768–785`, `:825–842`, which subtracts the heuristic) are all unsound when `h` can exceed the true remaining cost — the search may settle a connection through geometrically direct roads and prune the genuinely cheaper (curvier, discounted) detour before it is found. The shape this produces on a return leg is *greedy-toward-home*: straighter returns that skip the curvy road next door, and returns whose curviness is systematically below the forward leg's. It does not produce stubs or rings.

The fork's own additions are admissibility-safe: leash 4.2× (`motorcyclecost.cc:60,386`), rejoin ≤ 1 + 3.2·0.5 = 2.6× (`route_action.cc:1655–1656`), xcand ≤ 1 + 0.2·4 = 1.8× (`:1679–1680`, prod strength 0.2 cap 4), all multiplied in `EdgeFactor` ≥ 1 (`dynamiccost.h:1266–1286`), and hard excludes remove edges — costs only rise, so `h` never becomes an overestimate where it was not already. Confirmed. The harvest is unaffected: `Dijkstras` has no heuristic (`dijkstras.cc:220`, sortcost = cost) and is exact.

**Fix shape:** `AStarCostFactor()` should multiply by the smallest factor the costing can emit (`kDensityFactor[0] + curvature_factor_·kCurvatureFactor[15] − elevation_factor_·0.09`, clamped > 0). Weaker heuristic → wider search → latency; A1 showed the return leg is already the 45 % hotspot. **Needs rig confirmation** of the magnitude: compare each corpus return leg's cost to a Dijkstra-exact return (set the factor to 0) — the delta is the optimality loss.

## F04 — Rejoin grades miss hierarchy-twin exits (bug, major)

The grade builder takes each corridor edge's end node (`route_action.cc:1636–1639`), fetches *that node's* tile and `NodeInfo`, and iterates `ni->edge_count()` (`:1654–1657`). A node where road classes meet exists once per level (commit `2612c1008`, the "T2 lesson"), and only the edges of *this* level are enumerated — the twins' edges are not. So a secondary (level 1) corridor gets no grade on its unclassified/residential (level 2) exits, and a primary (level 0) corridor gets none on its secondary/tertiary exits. `correlate_node` and the walk-back were fixed with `for_each_junction_edge` (`:1084–1105`, `:1146`, `:1186`, `:1214`); the rejoin builder was not. The gurka `MotorcycleRoundTripRejoin` (`test_motorcycle_roundtrip.cc:949–1016`) is all-secondary, so it cannot see it. The mechanism ADR-0037 relies on to keep the return off "parallel streets home" is silently absent at class-mixed junctions — the normal case on a tertiary corridor in the highlands, whose parallel roads are unclassified. Partial explanation for stems that the stem meter does count. One-line fix.

## F05 — Hard exclusion vs hierarchy: complete, by accident (confirmed, fragile)

The round-trip branch (`route_action.cc:393–396`) leaves `route()` before `check_hierarchy_limits` is ever applied (`:558–582`, `:824`); `thor_worker_t::parse_costing` only creates the costing (`thor/worker.cc:236–244`). `DynamicCost`'s constructor fills every level with `kUnlimitedTransitions` / `kMaxDistance` when the request carries no `hierarchy_limits` (`dynamiccost.cc:207–220`), which the orchestrator's costing JSON never does (`costing.rs:28–40`). `BidirectionalAStar::Init` then sets `ignore_hierarchy_limits_ = true` (`bidirectional_astar.cc:148–154`), and:

- shortcuts are **never** expanded (`:202–205`: "Skip shortcuts if hierarchy limits are disabled"), so `RecoverShortcut` (`:1295–1298`) never runs and no path can contain a forward-corridor base edge hidden inside a shortcut;
- downward transitions are always allowed and `StopExpanding` never fires (`:468–469`, `:721–733`, `:759–764`), so the return search runs the full base graph at all levels.

The harvest never uses shortcuts either (`dijkstras.cc:131`). Therefore the edge-id sets — `hard`, `used_edges_`, `rejoin_factor_edges_`, `bank_edge_count` — are complete with respect to the graph the return actually searches. This also *explains A1*: the `bidirectional_astar.hierarchy_limits` and `threshold_delta` config knobs were byte-identical because the first is never applied to round-trip and the second only shortens over-search after a connection.

Fragility: the exclusion set is built from base ids only (`route_action.cc:1625–1635`), unlike loki's avoid path, which also inserts the covering shortcut (`loki/worker.cc:194–207`). If v4 (or an upstream default) ever gives the round-trip costing finite hierarchy limits, shortcuts become expandable beyond `expand_within_distance` (20 km / 5 km bidir defaults, `hierarchylimits.h:28–29`) and the return can ride the forward corridor through a level-0/1 shortcut that `IsUserAvoidEdge` (`dynamiccost.h:1068–1070`) does not know. Pin it with a gurka that asserts no served leg edge `is_shortcut()`.

## F06 — Second Via: figure-8 through home; leg C's exemption is defeated (BDRV, major)

- V2 eligibility is `sep ≥ 180 − 90 = 90°` (`route_action.cc:1888–1891`, `kSecondViaSectorDeg 180`): the entire opposite half-plane. Leg B (turnaround → V2) therefore crosses the start's neighbourhood by construction; the two-lobe loop is a figure-8 whose crossing is home. The gurka `OverStemLoopRebuiltTwoLobed` (`test_motorcycle_roundtrip.cc:762–777`) encodes this (east bulb, west ring, both hanging off A).
- Leg C's corridor is `fwd` + leg B with `path_distance += fwd_total` (`:1932–1938`), and the exemption test is `pi.path_distance > 1500` (`:1631`). Any start-access edge that leg B rode (inbound to home, outbound to the west lobe) re-enters the corridor with a rebased distance far above 1500 and is **hard-excluded for leg C** — the Start Exemption is applied by position in the concatenated corridor, not by edge identity. Where the west lobe shares the start's access road, leg C's primary search cannot reach the destination, and the Fallback (`:1702–1715`) drops *every* hard exclusion, not just the offending stem: leg C may then retrace leg B or the east lobe on the soft leash alone. The only check that sees this is `seam_stub_m(corridor, leg_c, 0)` at the V2 seam (`:1965`), which is exact-mirror and thus blind to ring-reversals at V2 (F01 again); `stem_fraction(fwd, ret2)` (`:1967`) looks at the start side only.
- V2 is not checked against `built_nodes`/`built_separated`, so a two-lobe loop can share a lobe with a served sibling.

Rider-visible as "the route goes past my house halfway". v4 should represent a two-via loop as three legs with edge-id exemption, and decide explicitly whether a via through home is a shape it wants.

## F07 — Long first edge forces a Fallback (BDRV)

Exclusion granularity is the whole edge: an edge whose cumulative forward distance *ends* beyond 1500 m is barred entirely (`route_action.cc:997–998`, `:1631`). Rural edges between junctions are routinely 2–5 km. When the start's own edge (or the only access road) is such an edge, the destination side of the return is unreachable in the primary search (`AvoidAsDestinationEdge` with percent 0 excludes any position, `dynamiccost.h:1094–1099`), and the loop is a Fallback Loop for a reason the design did not intend. The stem meter then counts the part of that edge beyond 1500 m; a 3 km access edge in a 20 km loop reads 0.15 stem — over the Second Via trigger, which cannot fix a network-forced stem. Fix: give the boundary edge a `percent_along` at 1500 m instead of 0 (`AvoidAsOriginEdge`/`AvoidAsDestinationEdge` already honour it, `:1081–1099`; `Allowed()`'s `IsUserAvoidEdge` does not, `:1068–1070`, so this only helps the origin/destination edges — which is the case that matters). **Needs rig confirmation** of frequency (census line 8).

## F08 — Artificial dead ends in the return search (latent bug, minor)

`BidirectionalAStar::Expand` evaluates the U-turn edge only when *no* other edge could be expanded (`bidirectional_astar.cc:439–458`, `:493–504`), and then marks the predecessor `deadend` so `Allowed()` accepts the U-turn (`motorcyclecost.cc:411`). `ExpandInner` returns false for a hard-excluded edge (`:261–266` via `IsUserAvoidEdge`). So every node whose non-U-turn exits are all forward-corridor edges is a dead end *for the return search*, and a bounce there is legal. A bounce is on the optimal path only when it enables something a direct transition forbids (a restricted turn, a one-way arm), so it is rare — but a hard-exclude success gets only the seam-window verdict (`route_action.cc:1860–1861`), and a mid-return mirror is outside it. The atlas measured return-side bounces at 2/2 602 *without* hard exclusion, which adds these dead ends. G6.

## F09 — Early widen forfeits the fresh attempt budget (bug, minor)

The stall branch grants `attempt_cap = attempts + want` only when it is the one calling `widen_pool()` (`route_action.cc:1996–1999`). Second Via (`:1880`) and the distance correction (`:2028`) also call `widen_pool()`, which sets `widened = true` (`:1786`). If either fires before the primary queue stalls, the later stall finds `widened` already true, grants nothing, and the loop breaks at `want + kAttemptSlack` (`:2000–2005`) — precisely in the hard cells the comment at `:1014–1017` promises to protect. Dedup and sharing-filter `continue`s also consume attempts (`:2009` precedes them). Effect is under-fill (K < 12), not defects.

## F10 — Junction walker uses `kAutoAccess`, and origin edges bypass `Allowed()` (minor)

`for_each_junction_edge` filters `forwardaccess() & kAutoAccess` (`route_action.cc:1100`); the costing's mask is `kMotorcycleAccess` (`motorcyclecost.cc:332`). Bidir A\* never calls `Allowed()` on origin edges (`bidirectional_astar.cc:1014–1082` — only `AvoidAsOriginEdge`), so a car-legal, motorcycle-forbidden exit can open the return leg and can count as a "fresh exit" in the walk-back. Rare in Serbia; trivial to fix with `cost->Allowed(edge, tile)`.

## F11 — Start Exemption: engine and harness agree to within one edge (confirmed)

Engine: forward-leg cumulative path distance at the edge's end (`route_action.cc:1631`, `:997–998`), forward edges only; xcand registration uses the same test on both legs (`:2115–2120`). Harness: segment midpoint within 1500 m of ride start *or* ride end along the loop polyline (`metrics.py:355`), and stems discounted by 1500 m each (`:454–456`); `PARAMS["start_exemption_m"] = 1500` (`:48`) matches `kStartExemptionMeters` (`route_action.cc:999`). The two differ only at the straddling edge (engine excludes it whole; harness discounts its first half) and in the corner where a return passes the start area mid-path (engine allows the exempt forward edges anywhere along the return; the harness only discounts the last 1500 m). Consistent for gate purposes. The gate is not reading something the engine did not enforce — it is reading *less* than the engine permits (F07's forced fallbacks are counted honestly).

## F12 — Fallback tagging is complete; the tag never leaves the engine (confirmed)

`fell_back` is set only on the retry path (`route_action.cc:1702–1715`); Second Via propagates `fb_b || fb_c` (`:1970`); corrections go through `attempt_build`; the dirty stash carries the flag (`:1865–1866`). No other call produces a soft-leash return (the primary always carries `hard` unless the forward leg is shorter than 1500 m, impossible at ≥ 20 km targets). The tag is consumed by the gate (`:1860`) and the correction threshold (`:2025`) and surfaces only as a request-level log count (`:2126–2128`); `tools/loopqual` does not parse it (no `fallback` reference in the harness). ADR-0037's "tagged and gate-counted" is half true.

## F13 — Defect Gate: the seam-window verdict is sound for what it defines (confirmed, three leaks)

For a hard-exclude success every corridor edge beyond 1500 m is barred, so an exact-mirror retrace at the seam is impossible and the ±1.5 km decode (`route_action.cc:1396–1409`) is a final verdict for that class; any mirror ≥ 30 m within the window is found even if the stub is longer than the window (`:1443–1455`). Fallbacks get the full decode (`:1860`). What the definition cannot see: F01 (ring apex), F02 (direction-distinct retrace), F08 (mid-return bounce). The stem check's window (`:1268–1282`) is trigger-exact for start-side stems by construction and deliberately excludes mid-loop shadow (`:1266–1267`).

## F14 — Bounce rejection is complete for immediate U-turns (confirmed)

`ScanBand` flags any label whose chain contains `pred.opp_local_idx() == de->localedgeidx()` (`roundtrip_expansion.cc:119–121`) and drops it (`:133`); the primary band, the flex re-scan (`route_action.cc:1789`), the backfill (`:1554–1565`), the refill queue (`:1572–1585`), the Second Via pool (`:1882`) and the correction pick (`:2034`) all draw from `cands`, which only `ScanBand` fills. Level-crossing U-turns cannot occur (the opposing edge is on the same level). `opp_local_idx` saturates at `kMaxEdgesPerNode` (`directededge.cc:512–519`) — the same limitation `Allowed()` has upstream; negligible. What bounce rejection does *not* define as a bounce is F01.

## F15 — Distance correction: one-node convergence prevented, neighbour convergence allowed (by-design)

The guard rejects the original node, any already-built node, anything within `min_separation_m` of a built turnaround, and anything outside ±45° (`route_action.cc:2034–2048`); `built_nodes`/`built_lls` are updated on commit with the served candidate (`:2099–2100`), so a second off-target build in the same sector re-aims to the *next* node ≥ 0.05·target away — on the same road, sharing the corridor. Distinct in node identity, near-twins in corridor (the ADR-0038 blind spot; the sharing filter that would catch it ships OFF). Also: the pick may already have been tried and failed, or may sit in the queue and be built anyway later (wasted A\*); one shot, ≤ 8/request, fallbacks only above 0.40 (`:2025`, `:1052`).

## F16 — `kFullExploreRadiusM` (by-design)

`ShouldExpand` prunes expansion *from* a level-2 label whose predecessor's path distance exceeds 90 km (`roundtrip_expansion.cc:49–50`; the test uses the predecessor's distance, `:42–44`, so labels overshoot by one edge — benign). Beyond 90 km the tree carries level-0/1 chains plus single dangling level-2 leaves: for 200–300 km targets (primary band 82–118 / 123–177 km) no turnaround chain may contain two consecutive unclassified/residential/service edges beyond 90 km. Curvy roads that are `unclassified` in OSM (common in the Serbian highlands) are unreachable as turnarounds on long loops; the flex band (0.55×: 55–83 / 83–124 km) partly escapes. Accepted in ADR-0033; a constant that changes candidate quality at 150 km is a v4 design smell.

## F17 — Tile boundaries (confirmed)

Every end-node lookup fetches the node's own tile: `ScanBand` (`roundtrip_expansion.cc:141–153`, post-`28a77c201`), `for_each_junction_edge` (`route_action.cc:1085–1096`), `correlate_node` (`:1122`), walk-back (`:1208–1214`), rejoin nodes (`:1651`); edge shapes are read from the edge's tile (`:1235–1238`, `:1415–1418`); `GetOpposingEdgeId` handles the crossing (`graphreader.cc:684–692`). No residue of the `28a77c201` class found.

## F18 — Determinism per seed (confirmed)

`best_by_node` (unordered) is re-sorted by label index (`route_action.cc:1505–1509`); `queued` is membership-only; the xcand merge is a max-reduction (`:1678–1684`) and registration a count (`:2121–2122`); `rejoin` is a lookup table; `served_sigs` is an ordered set; the keylen sums are order-independent up to float associativity on a fixed libstdc++ (deterministic for a fixed binary). The seed enters only at `:1546`. Per-request state is private, the reader is read-only, `multipath_` is false (`dijkstras.cc:19`) — no dependence on the 3-worker count. A1's byte-identical reruns agree.

## F19 — Cross-mechanism order in `route_leg` (confirmed)

`clear_used_edges` (drops leash *and* rejoin, `dynamiccost.h:1299–1302` — intended, both are per-leg) → leash both directions (`route_action.cc:1641`) → rejoin grades skipping corridor edges (`:1659–1665`) → xcand merged max-wise (`:1678–1684`) → request-level avoids restored then `hard` added (`:1687–1689`) → A\* → on failure drop `hard` only (`:1710`) → restore and clear (`:1716–1717`). No double counting between leash and rejoin. The xcand merge does not apply the corridor filter, so an *exempt* corridor edge registered by an earlier loop pays leash × xcand (4.2 × ≤ 1.8); mild, and ADR-0038's "exemption disk never surcharged" holds only per loop (the disk is path-distance, not straight-line). Note that the Fallback keeps leash, rejoin and xcand — only the hard set is dropped.

## F20 — Ranking and selection (optimality gap, major)

The served order is a stable sort on `Loop::curviness` (`route_action.cc:2149–2150`), which is `cands[ci].curviness_per_km` copied at build time (`:1866`, `:1981`) — the **harvest label chain's** score: computed before the walk-back pops the tip, ignoring the return leg entirely, ignoring distance error and the Fallback tag. It promotes exactly the chains F01 describes, ranks a Fallback Loop with a curvy forward leg above a clean loop, and lets a 40 %-off loop take slot 0 (K = 3 direct serve reads slots 0–2). Seed rotation permutes only inside each sector's top-8 (`:1546`); the backfill and the refill queue are seed-invariant curviness sorts (`:1554–1584`), so sparse cells serve the same slot 0 for every seed (atlas §9). Bearing sectors by straight-line bearing put adjacent-sector chains on the same trunk (H1).

## F21 — Distance targeting floor (by-design)

The ±18 % band bounds the *forward* leg only (`roundtrip_expansion.cc:16`); the return is whatever the penalized search yields, so loop length is `pd + ret` with `ret` unconstrained (often > `pd` under exclusion). One correction per loop, ≤ 8 per request, none for two-lobe loops, fallbacks only above 0.40 (`route_action.cc:2025–2027`); flex fills lean short (0.55×, `:1011–1012`). The achievable floor is the return-length variance; measured mean 0.178 (ADR-0038). Because ranking is distance-blind (F20), the served-slot order does hide off-target loops.

## F22 — The objective and rings (by-design; the inequality)

For a chain arriving on `e_in` at junction J and continuing on `e_out`, the ring `J→ρ_1…ρ_m→J` is preferred for labelling `e_out` iff

`Σ cost(ρ_i) + T(e_in→ρ_1) + Σ T(ρ_i→ρ_{i+1}) + T(ρ_m→e_out) < T(e_in→e_out)`

with `cost(ρ) = len·(3.6/min(v,top))·f(ρ)` and `f ≥ f_min` (0.523 at c0.5, 0.343 at c0.8, 0.223 at c1.0 — F03 table). Costs are strictly positive for every prod costing, so there are no negative cycles and the harvest Dijkstra is exact. Two regimes:

- **Reversal** (`e_out` = opposite of `e_in`): `T = ∞` unless `pred.deadend()` (`motorcyclecost.cc:411`), so *any* ring is preferred — F01. This is not a cost anomaly; it is the definition of a legal turnaround.
- **Non-reversal**: `T(e_in→e_out)` ≤ `kTCUnfavorableSharp 3.5 × stopimpact × kTransDensityFactor` (up to 3.5 at density 15, `dynamiccost.h:236–238`) + continuity break `8c` (`motorcyclecost.cc:63,567–576`) + `maneuver_penalty` (5 s at c0.5) — tens of seconds in dense cores, a few seconds rural. A 300 m curvature-15 ring at 30 km/h costs `36 s × 0.343 ≈ 12 s` at c0.8, so "around the block instead of a sharp left" is cost-optimal in dense urban graphs (upstream has this too; the fork's discount makes the ring 2–3× cheaper). Rural non-reversal rings are not optimal.

The A\* return leg has the same edge-based labels and the same reversal rule; its rings appear only where a U-turn is needed (F08).

## F23 — Duplicate start location (confirmed)

`locations = [start, start]` seeds `SetOriginLocations` twice (`dijkstras.cc:760–775`) into one tree (`multipath_ = false`, `:19`); the second set of origin labels duplicates the first at equal cost and settles as already-permanent. Harmless.

---

## What v4 must not inherit

1. **Harvesting turnarounds from an edge-labelled forest without a chain-simplicity test** (F01). A forward leg must be edge-simple on physical roads by construction; "not bounced" is not "not retraced".
2. **Directed-edge ids as the notion of "the same road"** for exclusion, leash, rejoin and xcand (F02, F05 fragility). Physical identity (shape buffer, or way + bearing) or an explicit divided-road model.
3. **Ranking by the harvest chain's curviness** (F20). Rank the built loop: both legs, distance error, fallback status, self-overlap.
4. **An A\* heuristic that ignores the costing's discounts** (F03) — either make it admissible and pay the latency, or stop calling the return leg optimal.
5. **Exact-mirror as the only spike definition in the build-time gate** (F13). Gate on decoded geometry: intra-leg self-overlap, cross-leg shadow anywhere, heading reversals off-seam.
6. **Two-via loops as a concatenated corridor with position-based exemption** (F06). Three legs, edge-id exemption, and an explicit decision on via-through-home.
7. **Single-level junction reads** (F04). Always the physical junction.
8. **Budget coupling to whoever widened first** (F09).
9. **Whole-edge exclusion at the exemption boundary** (F07).
10. **A constant prune radius that changes candidate quality above 150 km** (F16).
11. **Fallback = "drop every hard exclusion"** (F06, F12). A fallback should relax the *offending* constraint (the start stem), keep the rest, and export its tag.

## Needs rig confirmation

Each is a gurka test in `test/gurka/test_motorcycle_roundtrip.cc` style (1000 m/char unless noted; curvature set via `test::customize_edges`; `reuse_penalty 0.0` unless the mechanism needs the leash). "Predicted FAIL" = the current code should fail the assertion, which is the confirmation.

- **G1 `MotorcycleRoundTripRingReversal` (F01).** Map: `A------B--C--D` with a triangle at D (`D-E`, `E-F`, `F-D`, 1 km each) and a fresh return road `B-G-A` (G below B). AB 6, BC 2, CD 2 km. Curvature 15 on `CD`/`DC` and the ring, 0 elsewhere. Target 34 000, K = 1. The homeward labels at C (pd 15) and B (pd 17) are in band with straight-line 8 / 6 ≥ 0.3·pd; the outbound label at D (pd 10) is not. Assertions: (a) no undirected way name appears twice in `legs[0]` — **predicted FAIL** (`CD` ×2, `BC` ×2); (b) the served loop rides `EF`. Variant: add a curvature-10 clean arm `B-H` (H north, pd 15–17) and K = 1 → assert the clean arm wins — predicted FAIL (the reversal chain ranks higher).
- **G2 `MotorcycleRoundTripDualCarriageway` (F02).** 30 m/char. Two 100-char parallel one-way ways `A>…>B` (east) and `D<…<C` (west, 1 char south), links `B-D` and `C-A`, plus a longer two-way fresh road `B-E-F-A` far south. Curvature 15 on the carriageways. Target so that B is the turnaround. Assert the return uses `EF` and no return point lies within 40 m of a forward point beyond the exemption — **predicted FAIL** (return rides `DC`).
- **G3 `MotorcycleRoundTripRejoinMixedClass` (F04).** The existing `Rejoin` map with `AM`, `MB`, `BT` as `primary` and `QM`, `MX` as `unclassified`. With `reuse_penalty 0.5`, assert `count(QM) == 0` as the existing test does — **predicted FAIL** (M's level-2 twin holds `QM`/`MX`; no grade).
- **G4 `MotorcycleRoundTripSecondViaFigure8` (F06).** The `SecondVia` map with the start moved onto the connecting road (`P-S-A`, S between the lobes) and the west ring reachable only through `S-P`. Assert: `legs[1]` contains no edge twice, and the polyline's minimum distance to S between 20 % and 80 % of the ride is > 200 m — **predicted FAIL** (leg B passes S; leg C falls back and re-rides `SP`).
- **G5 `MotorcycleRoundTripLongFirstEdge` (F07).** The `CulDeSac` map with `AB` = 3 km. Assert every non-`AB` edge is ridden once — **predicted FAIL** (whole `AB` excluded → Fallback → the primary corridor `BC`/`CD` may be retraced on the leash).
- **G6 `MotorcycleRoundTripRestrictedTurnBounce` (F08).** A return that needs a left turn at junction K forbidden by a `no_left_turn` relation; the only alternative is to overshoot into a spur that ends on the corridor and U-turn. Assert `legs[1]` has no exact-mirror stub ≥ 30 m — predicted FAIL if the bounce is cheaper than the long way round.
- **G7 shortcut pin (F05).** Any map with a level-0/1 chain long enough to form a shortcut; assert no served leg edge `is_shortcut()`. Passes today; pins the accident.
- **F03 magnitude.** Corpus-v1 with a debug build where `AStarCostFactor()` returns 0 (Dijkstra) vs stock: per-loop return-leg cost delta and curviness delta. Any loop with a lower-cost return under the exact search is a proven optimality loss.

## What the census should look for in real loops

1. **Intra-leg self-overlap:** undirected 1e-5 segment reuse computed within `legs[0]` alone and within `legs[1]` alone (the harness only computes cross-loop reuse). Rate, stub length, terrain, distance. F01 signature.
2. **Off-seam heading reversal:** points where distance-from-start decreases monotonically for ≥ 1 km inside `legs[0]` (a ring-tipped stub), excluding the seam.
3. **Bulb at the reversal:** small closed rings (< 1 km) at the apex of (2) — count roundabouts / triangles / village loops.
4. **Parallel retrace:** return points within 40 m of forward points with heading within 30° of anti-parallel, beyond the exemption. F02 signature.
5. **Passes home mid-ride:** minimum distance from the polyline (10–90 % of the ride) to the start < 200 m; join with the `second_vias` log count. F06 signature.
6. **Decompose `reuse > 0.30`** (gate 5's 8.07 %): where the reuse sits — fwd–fwd, ret–ret, cross-leg. Mostly fwd–fwd confirms F01.
7. **Slot distribution** of loops flagged by 1–4: over-representation in slots 0–2 confirms F20's promotion.
8. **First forward edge longer than 1500 m** vs Fallback count (log) and `stem_frac`. F07 signature.
9. **Return vs forward curviness** per loop (curviness_geom of `legs[1]` / `legs[0]`), by fallback status. A systematic deficit beyond what exclusion explains is F03.
10. **Rejoin exits by level:** for corridor nodes with transitions, whether the return's first shadowing edge is on a twin level. F04 signature.

# P2 — Suurballe–Tarjan whole-loop construction on the P1 sidecar

- **Date:** 2026-09-06/07 (curvagen-valhalla [#11](https://github.com/Lazark0x/curvagen-valhalla/issues/11), P2 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); direction locked 2026-09-06, hybrid, whole-loop scope)
- **Scope:** a **throwaway prototype** on branch `proto/v4-p2`, cut from `proto/v4-p1.1` @ `3ddb8db48` (the scoped-budget knee). One mechanism: replace *harvest → select → hard-excluded return A\** with *harvest → **disjoint-pair pass** → select on the pair → close the loop from the pair*, the Suurballe–Tarjan single-source shortest-pairs algorithm run over the harvest forest with the P1 sidecar folded in. Everything else is the P1.1 knee unchanged (`roundtrip_gate_refill_budget = 2` scoped to the geometry gate, `roundtrip_fallback_rungs = false`, built-loop ranking, switchback-safe twins). Nothing pushed; no production traffic; `valhalla-local` (:8002) and the :8791 results server never addressed.
- **Method:** the P1.1 §16 paired protocol on corpus-v2 (552 requests, K = 12, engine mode, 3 workers, way pass on) against the prod-equivalent Serbia tiles (`tileset_last_modified 1785932567`): baseline `valhalla-curvature:prod-equivalent-b4f514d7f` on :8004, P2 on :8003, one engine at a time, baseline brackets around the candidate. Detectors are the P1.1 scripts with the fixed D3 counter **plus the switchback-aware D1b** (§9), re-run over the census and the P1.1 knee so every column in this document is read by byte-identical code. Ledger fields are the engine's own (`roundtrip pair-pass:`, `roundtrip pair-select:`, `roundtrip pair-ranks:`, `roundtrip pair-loop:`, and the P1.1 lines).
- **Session note:** the build and the eight corpus runs of §7.1 were driven by an AFK agent on 2026-09-06/07 (the session ended after the last run, before any analysis); the detector passes, the distinctness block (§7.8), the gurka run, the galleries and this write-up were completed 2026-09-07 in the continuation session.

## TL;DR verdict

**P2 wins the half P1.1 could not — latency with zero spikes — and loses the half P1.1 had: bank distinctness.**

- **Latency:** wall p50 **0.959×** the pooled baseline (1.029 s vs 1.073 s, three brackets, sd 5.1 %), engine-stage median 0.927×, engine-stage mean 1.069×, with `spike_ge_30m` / `spike_ge_500m` **0.00 %** at every level and fills **552/552**, failures **0**. The P1.1 knee read 1.227× for the same absolutes. The pair pass costs 295 ms/request on average and pays for itself by making the return A\* almost never fail (`astar_fb` 147 → 28 ms; attempts 12.62 → 13.50 against the knee's 14.15). **The price is the tail:** p95 **1.331×**, and 300 km asks 1.29× at p50 / 1.60× at p95 — the pass is superlinear in harvest size.
- **Cleanliness:** the near-mirror mechanism (F02) is **0.0 % in every block and every slot** (knee, same read: 0.4 %, slots 0–2 0.6 %); Vračar D1b ≥ 500 m **0.0 %** (knee 1.3 %); same-pavement D1 Vračar **0.0 %** (knee 6.9 %); the worst loop in the corpus by unseen near-mirror metres is **913 m** (knee 4 002 m) and it is a ring, not a retrace. **On the demand cells P2 is not cleaner than the knee by the count:** D1b ≥ 500 m 8.5 % vs the knee's 7.0 % on the same read (the ticket's 8 % bar was set on the knee's legacy 8.2 %), mean 73 m vs 59 m — and every one of those entries is a sub-kilometre ring at one cell, not a retrace. Fallback rungs halve (23.2 → 9.4 % of return legs), the D4 fallback proxy 14.3 → 6.1 %, `clean` 9.1 → 15.0 %. Curviness is *up* at the demand levels (c0.7 1.278×, c1.0 1.225×, c0.5 1.050×), c0.8 0.948× (advisory, unmoved since P1).
- **Distinctness:** `bank_overlap_mean` **0.6214** and `near_dup > 0.6` **62.2 %** with the penalty off, against the rig baseline's 0.4931 / 34.2 % (1.260×) and prod-with-penalty's 0.4407 / 24.0 % (**1.410×**, bar ≤ 1.10×). With the prod penalty on the repairs 0.5387 / 43.3 % (1.222×); the ADR-0040 built-loop filter reaches the bar (0.4762, **1.081×**, near-dup 18.8 %) but only by refusing to fill — **468/552** banks full, attempts 13.5 → 31.4 per request — so **no configuration on the branch meets ratchet 9 and the fills bar at once.** The pair-keyed K × K sharing filter (§5) is blind to the served return: 92.4 % of returns are rebuilt by `route_leg` after selection, so the filter judges a path the rider never rides; and the bank the pair pass selects is less diverse to begin with — on both legs: the penalty restores the return legs to prod's level (0.241 vs 0.247), the forward legs stay at 0.297 vs 0.246 because the pair-eligible sinks cluster on the same trunk roads out of the start (§7.8, §11). Under the prod condition (both engines at xcand 0.2, uncontended) P2 reads **0.927×** wall p50, 0.988× engine mean, 1.30× p95.
- **The pair is not a loop in this network:** only **7.6 %** of built loops could ride the pair's second path home unrepaired (no city or mountain probe produced one); **14.5 %** of return arcs have no rideable opposite (one-ways; 28–36 % in Belgrade); the Suurballe pass is therefore a *selector and a certificate* (existence, length, twin test, cost) and P1.1's `route_leg` is still the return builder. Bridging the non-reversible stretches locally (v1) reintroduces 100–270 m stubs (`spike_ge_30m` 4.48 %) and is dead.
- **Gurka 49 green**, including the return-vs-return twin ride the ticket asked for (P2b).
- **Verdict for the ladder:** keep the pair pass as the selection stage (P2's mechanism is exactly what §14 of P1.1 asked for), and treat distinctness as the next lever (§11, §12) — the mechanism that fixes it is a built-loop filter or the xcand penalty on the repairs, both already on the branch. The keep/shelve call is Andrey's, on the galleries (Appendix B).

---

## 1. The formulation

### 1.1 What Suurballe–Tarjan gives, restated for the loop

Suurballe & Tarjan (Networks 14, 1984; delta survey §2): on a directed graph with source `s` and non-negative costs, after a shortest-path tree `T` with distances `d(s,·)` is known, transform every arc `c'(u,w) = c(u,w) − d(s,w) + d(s,u)` (non-negative, zero on tree arcs, order-preserving), and one Dijkstra-like labeling pass over `T` computes, **for every sink `v` at once**, `d'(v)` = the cost of the cheapest pair of edge-disjoint `s→v` paths in `G` (`∞` = no such pair: some arc lies on every route to `v`). The pair for any `v` is then constructed in `O(1)` per arc from the implicit `(p, q)` representation (paper §II end). The labeling step (paper §II): pick the unlabeled vertex of minimum tentative `d`, label it, split its unlabeled subtree of `T`, and for every nontree arc `(u,w)` with `u` in that subtree and `u = v` or `u, w` now in different subtrees, relax `d(w) ← d(v) + c'(u,w)`, `p(w) ← (u,w)`, `q(w) ← v`. §III's implementation — incidence lists sorted by preorder number of the far end, unlabeled-subtree children lists, the concurrent "all but one" subtree traversals — is what makes it `O(m log n)`; it is implemented as written (`src/thor/roundtrip_pairs.cc`, `ShortestPairs`), and the paper's own Fig. 1–3 example is a gurka pin (`RtP2SuurballeCore.PaperFigure1`: `d = {d:1, e:2, f:2, b:12}`, the pair for `f` is `(s,a,c,f) + (s,d,f)`).

The two things the survey said do not transfer for free were both true, and a third one turned up.

### 1.2 The graph the pass runs on — the junction graph `H`

The harvest is an edge-labelled, turn-cost-aware `Dijkstras` over directed edges; Suurballe needs a vertex graph with a shortest-path tree. `H` is built from the settled label forest (`RoundTripPairPass::Run`):

| element | definition | count, Belgrade 50 km / 300 km |
|---|---|---|
| **vertex** | one physical junction the expansion settled an edge into — hierarchy-level node twins folded (min `GraphId` over the node and its transitions); plus a virtual source `S` | 86 k / 339 k |
| **potential** `d(J)` | the cheapest settled label ending at `J` — the harvest's own turn-aware distance | — |
| **tree arc** `parent(J) → J` | one arrival label per junction (§1.3), the junction of the label's predecessor as parent; strictly decreasing potential along the tree keeps it acyclic; `c' = 0` | 86 k / 339 k |
| **nontree arc** `A → B` | one per road-and-direction between two junctions, merged by `(A, B)`: every settled label `A→B` contributes a forward arc; every settled label `B→A` whose opposite was never settled contributes a *return-only* arc `A→B`; `c' = max(0, (d_label(B-arrival) − d_label(its pred)) − d(B) + d(A))` | 93 k + 20 k / 363 k + 80 k |
| arc flags | `fwd_label` — the settled edge that rides `A→B` (the forward leg may use it); `ret_edge` — the rideable edge `B→A` (the return may ride it, backwards along the arc) | two-way 81 % / 85 % |
| Start Exemption | a parallel zero-cost copy of every tree arc whose label ends within `kStartExemptionMeters` and has a `ret_edge` — capacity two, ADR-0037 §3's "the network-forced first stretch may carry both legs" (paper §IV, multidigraphs) | 575 / 107 |
| sink | the junction itself; the pair for `J` = `(P_J, Q_J)`, two arc-disjoint `S→J` paths | pairs exist for 64 % / 65 % of junctions |

Over the whole corpus (552 requests, ledger means): 103 k junctions per request (p50 95 k, max 339 k), 239 k arcs (max 781 k), pairs exist for **63.5 %** of junctions, 1.1 % of arcs clamped, tree arcs 88.8 % two-way / 11.2 % one-way, the pass holds 37 MB (p50 31 MB, max 117 MB at 300 km).

The loop for a junction is the pair: one path ridden **out** on its `fwd_label` edges, the other ridden **home backwards** on its `ret_edge` edges. Directed-arc disjointness in `H` means: the return never rides the forward leg's road in the opposite direction (the mirror / spike), because that would be the *same* arc `A→B` used twice (forward via `fwd_label`, return via `ret_edge`). It does not forbid the return riding a forward road in the *same* direction (the second arc `B→A`), which is rare and which the built-loop meter still scores.

### 1.3 Approximations, named

1. **Turn costs are dropped from the reduced costs.** The potentials `d(J)` are the harvest's turn-aware label costs; an arc's own cost is `d_label(arrival) − d_label(pred)` (edge cost plus *that* chain's transition), so `c'` can come out slightly negative for a sharper turn than the tree's — it is clamped at zero and counted (`clamped=` in the ledger: 1.7 % of arcs at 50 km, 0.9 % at 300 km, 1.1 % over the corpus). Exactness of the pair cost is lost by at most a turn cost per arc; the pair is still a valid disjoint pair.
2. **Direction-symmetric cost for the return.** The return rides `ret_edge` (the opposite direction) at the forward arc's cost. Speeds and turn costs differ per direction; the built loop is re-costed edge by edge with the real costing before it is served (`cost_path`), so only the *selection* sees the symmetric number.
3. **The second phase is offered return-rideable arcs only** (`roundtrip_pair_return_legal`, default on): a nontree arc with no `ret_edge` — a one-way pointing away from the start — is not offered to `Q`. The first path still reaches such roads through the tree. **Tree arcs are traversed by `Q` for free whether or not they can be ridden back** — the paper's invariant (a vertex is reachable at zero cost inside its unlabeled subtree) does not survive a per-arc traversal ban, so this is the one place the formulation cannot be made direction-aware. §1.4 is about what that costs.
4. **Two-way arrivals are preferred as tree arcs** (`roundtrip_pair_two_way_tree`, default on): among the labels arriving at a junction from a strictly lower potential, a two-way one is the tree arc even when a one-way one is cheaper. Where that happens (`not_cheapest=`: 198 of 86 k junctions at Belgrade 50 km, 448 of 339 k at 300 km, 0.17 % over the corpus) the tree path is a legal but not a cheapest route and its reduced cost is still treated as zero. It changes the composition of one-way tree arcs by very little (16 % → 16 % at Belgrade): a junction on a one-way street is reached by one-way streets.
5. **The second path lives inside the harvest region.** Only settled labels become arcs, so the return can use no node whose tree distance exceeds `1.2 × target/2`. The gurka maps had to be sized for it (§6, P2b), and it is one reason a share of in-band sinks have no pair at all.
6. **Twins are a post-check, not a capacity.** The two carriageways of a dual carriageway have distinct junctions; folding them into one vertex (`roundtrip_pair_twin_join_m`, join twin endpoints within 60 m through the sidecar's pairs) *would* make the twin ride a capacity conflict by construction, and the code carries it, but the union-find chains through whole neighbourhoods of parallel village streets (19 649 of 86 472 junctions folded at Belgrade, 8 300 of 94 723 at Zlatibor) and every folded junction is a gap the ride cannot cross without a real crossover (`gap=` failures on 100 % of forward legs at Zlatibor). **Measured with the fold off** (`twin_join_m = 0`); the twin exclusion is the selection-time reject of §3.2, which reads the pair's arcs against the sidecar exactly as P1.1's gate reads a built loop — return-vs-return included.
7. **Orientation is chosen after construction.** Either path may be the forward leg (every arc needs a `fwd_label`); the tree path is the default, and the pair is swapped when only the swap can be ridden out or when it leaves fewer return stretches to repair (`swapped=` in the ledger: 23 649 of 943 754 evaluations, 2.5 %).

### 1.4 The third thing that does not transfer — the cycle is not a pair (one-ways)

The survey's §2.3(b) named the risk and called the one-way failure rate "the single biggest open question". Measured on corpus-v2 (v2 run, 6 809 pair-built loops):

| | value |
|---|---|
| return arcs walked | 3 046 406 |
| return arcs **without a rideable opposite** (`no_return_edge`) | 441 711 = **14.5 %** |
| return arcs failing on a turn the costing refuses the other way round | 35 646 = 1.2 % |
| pair-built loops whose second path is **fully reversible** (`reversible=`) | **517 of 6 809 = 7.6 %** |
| loops needing a full `route_leg` return (`full_repair=`) | 6 292 = 92.4 % |
| `repair_failed` | 0 |

Per block — single-request probes on the idle engine (v2 config, seed 7, K = 12; block C origins in corpus-v1's motorways-avoided mode; `~/.curvagen-scratch/p2/eng-probe.log`):

| probe | junctions in `H` | junctions with a pair | shortlist rejects `no_pair` | return arcs without a rideable opposite | loops fully reversible | pass build + run + eval | `H` |
|---|---|---|---|---|---|---|---|
| Belgrade 50 km c0.5 | 88 820 | 64 % | 50 % | 28.0 % | 0 / 12 | 84 + 26 + 26 ms | 30 MB |
| Belgrade 300 km c0.5 | 337 325 | 65 % | 48 % | 7.3 % | 0 / 12 | **482 + 260 + 335 ms** | **117 MB** |
| Vračar 40 km c0.7 | 76 656 | 64 % | 39 % | **35.9 %** | 0 / 12 | 75 + 24 + 34 ms | 27 MB |
| demand cell #1 50 km c0.5 | 87 508 | 64 % | 44 % | 28.0 % | 0 / 12 | 83 + 25 + 31 ms | 30 MB |
| demand cell #2 100 km c0.7 | 150 407 | 66 % | 31 % | 19.7 % | 0 / 15 | 150 + 55 + 49 ms | 55 MB |
| Novi Sad 100 km c1.0 | 98 572 | 73 % | 38 % | 22.4 % | 0 / 13 | 99 + 37 + 97 ms | 33 MB |
| Zlatibor 50 km c0.5 | 19 688 | 49 % | 55 % | 2.6 % | 0 / 13 | 18 + 4 + 7 ms | 7 MB |
| Vlasina 50 km c0.5 | 1 451 | **0 %** | 100 % | — | — (rescue pass: 11 built of 32 attempts) | 0 | 0 |

In the city a quarter to a third of every return's arcs are one-way; in the mountains almost none — and still **no probe produced a single fully reversible pair**, because one refused turn or one one-way stretch anywhere on a 50-arc return voids the whole path as a ride. The 7.6 % of reversible pairs in the corpus are the sparse-lowland origins. Vlasina is the tree case of §3.4: 1 451 junctions, none with a pair.

What the number means: the pair's second path is a shortest `s→J` path under reduced costs, and it borrows tree arcs for free; in a city the tree is the one-way arterial grid, so `Q` rides one-way streets *outward* that the return cannot ride back — and one non-reversible arc anywhere on a 50-arc return is enough to void the whole path as a ride. Formally the oriented loop is a 2-commodity flow (forward on `G`, return on `G^R`, shared road capacities), not a 2-unit flow on one graph, and the single-graph pass is exact only where the network is symmetric. Where it is not, the pair is a *proxy* — existence, length, curviness and the twin test are still read from it at selection — and the return has to be repaired:

| repair | what | knob |
|---|---|---|
| **bridge** (v1) | each non-reversible stretch (missing `ret_edge`, a turn refused the other way round, a gap) is replaced by a local hard-excluded bidirectional A\* between the junctions either side of it, with the rest of the loop and its twins barred (`route_leg` on a synthetic corridor whose return items carry "metres to the ride end" as their path distance, so the exemption reads the return as it reads the forward leg) | `roundtrip_pair_bridge`, cap `roundtrip_pair_max_bridges` (12) |
| **full** (v2) | the whole return rebuilt with P1.1's `route_leg` on the pair's forward leg — the closure-only rung of the ticket | automatic past the cap, or `roundtrip_pair_bridge = false` |

Both variants are measured (§7): **v1** bridges (6.2 bridges per request, 34 bridge searches, 249 ms), **v2** goes straight to the full repair. v1 is dead on spikes (§7.4, §11); **every quality number below is v2 unless marked.**

## 2. The sidecar and the twin joint disjointness

The sidecar (P1/P1.1's `RoadTwinIndex`, switchback test on) enters in three places:

1. **Chain hygiene** — a junction whose tree arrival chain is twin-non-simple (F01 with twins) is not a sink (`dropped=` in the ledger: 382 per request).
2. **Selection-time twin test** (§3.2) — the pair's arcs are walked once; a return arc whose road (canonical `min(edge, opposite)`) or any sidecar twin of it is on the forward leg beyond the exemption, **or on the return leg already** (the H3 return-vs-return shape of the same-road study), counts as overlap; `≥ roundtrip_gate_twin_ride_m` (500 m) rejects the sink before anything is built. This is the geometry gate's twin-ride verdict moved from after the A\* to before it, plus the return-vs-return term P1.1 lacked (`route_action.cc` `score_built` now carries it too, in pair mode only, so the built-loop meter and the selection test agree).
3. **The K × K sharing filter** (§5) keys on the same canonical ids and their twins.

The structural version (twin-folded junctions) exists behind `roundtrip_pair_twin_join_m` and is off for the reasons in §1.3(6).

## 3. Selection by pair

### 3.1 Candidates are junctions

The harvest's `ScanBand` candidates (labels with path distance in `target/2 ± 18 %`, straight-line ≥ 0.3 × path distance, bounce-free, F01-simple) are re-hung on their junction's tree arrival (`rehung=`, 1 947 per request), listed once per junction (`merged=`, 12 696), and dropped when the tree chain is non-simple (`dropped=`, 382). Distance is then judged on the **pair**: `|tree_len + other_len − target| / target ≤ roundtrip_pair_band` (0.20 — the built-loop tolerance). Bearing sectors (K of them), the seed rotation and the min-separation guard are P1.1's.

### 3.2 The per-sector shortlist and its rejects

Per sector, the top-`M` (`roundtrip_pair_shortlist = 8`) candidates by harvest-chain curviness have their pairs constructed and judged, in this order, each a reject that costs a path walk and never a search:

| reject | test | ledger | corpus-v2, share of 943 754 evaluations |
|---|---|---|---|
| `no_pair` | `d'(J) = ∞` — Suurballe's existence condition: some road lies on every route home (a bridge, a valley road, the far side of the harvest region) | `no_pair=` | **60.9 %** |
| `band` | pair length outside `target ± 20 %` | `band=` | 16.2 % |
| `fwd_illegal` | neither path can be ridden out (a return-only arc on both) | `fwd_illegal=` | 8.8 % |
| `share` | near-duplicate of a loop already chosen this request (§5) | `share=` | 7.6 % |
| `twin` | twins-aware self-overlap ≥ 500 m (§2) | `twin=` | 2.0 % |

1 710 evaluations per request (p50 730) for 11.06 chosen and 12.34 built. Survivors are ranked by **pair score** = curviness over both legs (`Σ curvature·len / Σ len / 15`, the built-loop meter's own numerator) discounted by the pair's distance error `1 / (1 + Wd·dist_err)`, i.e. P1.1's built-loop score with overlap = 0 by construction, and the seed rotates among them. The pair cost is what the construction minimised and is surfaced in `roundtrip pair-ranks:` (`slot:paircost/surplus/fwd-m/ret-m/bridges/full`), not in the score — the costing already discounts curviness, so cost-per-metre and curviness are the same axis. The backfill and the refill queue apply the same tests lazily on dequeue; a queue entry that fails them is a `continue`, not a spent attempt (P1.1's `kAttemptSlack` budget stays for real builds).

### 3.3 The return, from the pair

`attempt_pair_build`: orientation (§1.3(7)), the forward leg re-costed edge by edge with the request's costing (`cost_path`: edge cost, transition cost, `Allowed()` between consecutive edges, physical connectivity), the turnaround correlated on the forward leg's arrival edge exactly as P1.1 does (U-turn door dropped), the return assembled from the other path's `ret_edge`s in reverse and validated the same way, bridged or rebuilt where it fails (§1.4), then P1.1's `finish_build`: seam gate, geometry gate with the scoped budget, built-loop score. The dirty tier survives: twin-rejected pairs are remembered and, when the queue runs dry with the bank short, built anyway so the gate can stash them for the per-slot last resort — a ranked-last dirty loop, never a silent clean tier and never a 442 where a loop exists (gurka P2b, §6). On corpus-v2 the twin last resort never fired (`twin_last_resort = 0`).

### 3.4 The rescue pass (added after the first corpus run)

The first v2 binary failed **29 requests** (400, no loop) and under-filled 22 more: every Vlasina request at c0.5 and c0.8, Djerdap 20 km (3 routes), Golija 20 km (7), Zlatibor 300 km (5–6), Djerdap 300 km (5–7). Where the network around the start is a tree — one lake road in and out — **no junction in the band has a disjoint pair at all**, and a pass that only builds pairs has nothing to build. `97e774095` adds the **rescue pass**: when the pair queue runs dry with the bank short, the remaining `ScanBand` candidates are built P1.1's way (harvest chain out, hard-excluded `route_leg` home), through the same gates and the same ranking. On corpus-v2 it fires on **51 requests** (9.2 %) and builds **477 of the 6 624 served loops** (7.2 %); every request fills 12/12 (§7.3). Rescue loops are P1.1 loops — they carry no pair certificate, which is what the Vlasina lollipop numbers of §7.7 are.

## 4. Gate and rank

Unchanged from the P1.1 knee: the seam gate (unbudgeted), the geometry gate (twin ride 500 m, mid-return bounce 30 m) with `roundtrip_gate_refill_budget = 2`, fallback rungs off, tiers `r0 < r1 < r2 < gated`, built-loop score `curviness / ((1 + 4·overlap_frac)(1 + dist_err))`. What P2 adds: the return-vs-return overlap term (pair mode only) and the ledger's `gate_fires=` per request, so the ticket's "count how often the gate fires at all" is a direct read: **881 gate rejects over 552 requests (1.60 per request; 69 requests see any)** against the knee's 2 582 (4.68 per request; 479 requests) — the selection-time rejects (§3.2) took the gate's work.

## 5. xcand — the post-pass K × K sharing filter

The per-candidate return-leg surcharge (`roundtrip_xcand_penalty`) cannot ride a single pass. P2 replaces it with a **twins-aware K × K near-duplicate filter applied at selection and again at dequeue**: each pair's ridden roads beyond the exemption (canonical ids → metres, plus the set of their twins) are kept; a candidate whose ridden metres on roads that a chosen/built pair rode — *or whose twins it rode* — exceed `roundtrip_sharing_frac` (0.6) of its length is rejected (`share=`, 7.6 % of evaluations). The filter is best-effort past `roundtrip_pair_eval_cap` (600) evaluations per request. The xcand penalty itself still applies to every A\* the repairs run, so the "penalty on 0.2" reading of §7.8 is the honest prod comparison: both engines with the penalty on, P2 additionally with the filter.

**Measured, the filter does not do the job (§7.8, §11):** it keys on the *pair's* roads (`pair_built_keys` holds `pair_cache[cand].keys`), and 92.4 % of served returns are not the pair's second path but a `route_leg` repair. Two candidates whose pairs share < 60 % can be served with returns that converge on the same corridor home, and nothing in P2 sees it. The ADR-0040 item-4 filter that keys on the *built* loop (`roundtrip_sharing_filter`, `loop_keylen`, off by default and off in the measured run) is the built-loop version of the same test; it costs a refill per reject. Measured (§7.8): the pair filter buys 0.018 of `bank_overlap` (0.6398 → 0.6214, near-dup 67.5 → 62.2 %); the penalty on the repairs 0.083 more (→ 0.5387, 1.222× prod); the built-loop filter reaches 0.4762 (1.081×) at 468/552 fills and 31 attempts per request.

## 6. Gurka — 49 green

`gurka_roundtrip_audit` **24/24** (4.7 s) · `gurka_motorcycle_roundtrip` **22/22** · `gurka_roundtrip_distinctness` **3/3**, on the measured binary (`83837debc`, 2026-09-07).

| test | pins | result |
|---|---|---|
| `RtP2SuurballeCore.PaperFigure1` (new) | the labeling pass and the reconstruction on Suurballe & Tarjan's Fig. 1–3: `d(d)=1, d(e)=2, d(f)=2, d(b)=12`, `a/c/g` no pair, the pair for `f` = `(s,a,c,f)+(s,d,f)`, for `b` = `(s,b)+(s,a,e,g,b)`, arc-disjoint | PASS |
| `RtP2DualCarriageway.P2a_NoPairThroughTheTwinCarriageway` (new) | P1.1c's map: the only way home from the corridor is the twin carriageway. **With the geometry gate OFF**, P1.1 serves the twin ride (the P1.1c control); P2 serves the clean lobe — `B` has no second disjoint arrival, so the corridor is never built. Structural, not a gate effect | PASS |
| `RtP2ReturnVsReturn.P2b_TwinRideInsideTheReturnIsRejectedAtSelection` (new) | the H3 shape: the only way home rides road B and then its twin A inside the RETURN leg. P1.1 serves it clean (`r0`, self-overlap 0 — its twin bar reads the forward leg only); P2 rejects the pair at selection (return-vs-return twin overlap 900 m) and serves the clean lobe; on a second map with no lobe the twin loop is still served, as the dirty last resort | PASS |
| `RtP2OneWayRepair.P2c_NonReversibleStretchIsBridged` (new) | a one-way on the second path (and on the forward road, so no orientation dodges it): the reversible parts of the return are kept and the one-way stretch is bridged through the two-way bypass, no corridor retrace | PASS |
| `RtP2Grid.P2d_DistinctSinksInsideTheBand` (new) | a 5 × 5 lattice, K = 6: ≥ 3 pair-built loops, distinct turnarounds, every loop inside the band, no way ridden both ways beyond the exempt cells | PASS |
| `gurka_roundtrip_audit` 19 P1/P1.1 tests · `gurka_motorcycle_roundtrip` 22 · `gurka_roundtrip_distinctness` 3 | unchanged (pair mode is `false` by default; the corpus enables it by config) | PASS |

P2b is the proof the ticket asked for: the return-vs-return twin ride is caught **at selection**, from the pair, before any A\* — on the same dual-carriageway shape P1.1 served clean-tier because its overlap set was built from the forward leg only (`route_action.cc:1883-1914` on `proto/v4-p1.1`).

## 7. corpus-v2 — before/after

### 7.1 The runs

All on the prod-equivalent Serbia tiles, corpus-v2 (552 requests, K = 12), engine mode, 3 workers, way pass on; one engine at a time. Baseline = `rt-p11-base` (:8004) with `roundtrip_stage_timing` on; P2 = `rt-p1-build` (:8003), binary `proto/v4-p2` @ `83837debc` unless noted.

| run | binary / config | results dir | purpose |
|---|---|---|---|
| baseline **K**, **L**, **M** | `b4f514d7f`, rig `valhalla.json` + stage timing | `census-v2-p2-base{K,L,M}/` | the three latency brackets (K before v2, L between v2 and its repeat, M before v1) |
| P2 v2, first binary | `0e4654056` (no rescue pass), `v8003-p2v2.json` | `p2-v2/` | **29 failed requests, 6 142 loops** — the tree-network failure of §3.4; kept as evidence, not measured further |
| **P2 v2** | `83837debc`, `v8003-p2v2.json` (`roundtrip_pair_pass true, roundtrip_pair_bridge false, roundtrip_pair_twin_join_m 0`, knee otherwise) | `p2-v2b/` | **the measured configuration** |
| P2 v2, repeat (tight) | same | `p2-v2b-tight/` | the latency repeat; `loops.jsonl` byte-identical in every meter (deterministic engine) |
| P2 v1 (bridging) | `83837debc`, `v8003-p2v1.json` (`roundtrip_pair_bridge true`, cap 12) | `p2-v1b/` | the bridged-return variant — dead on spikes (§7.4) |
| P2 v2 + xcand 0.2 | `v8003-p2v2-xcand.json` (+ `roundtrip_xcand_penalty true, cap 4, strength 0.2`) | `p2-v2-xcand/` | ratchet 9 apples to apples, the prod condition (baseline side: `census-v2-scoped-xcandon/` from P1.1 §16, deterministic) |
| P2 v2, pair filter off | `v8003-p2v2-noshare.json` (`roundtrip_pair_sharing false`) | `p2-v2-noshare/` | what the pair-keyed filter does at all |
| P2 v2 + xcand 0.2 + built-loop filter 0.6 / 0.5 | `v8003-p2v2-xcand-filter{,05}.json` (+ `roundtrip_sharing_filter true`, `roundtrip_sharing_frac`) | `p2-v2-xcand-filter{,05}/` | the ADR-0040 item-4 filter on the *built* loop (§5, §11) |
| baseline **X** + xcand 0.2 · P2 v2 + xcand 0.2, **uncontended** | `v8004-xcand.json` · `v8003-p2v2-xcand.json` | `census-v2-p2-baseX/` · `p2-v2-xcand-uncontended/` | the prod condition's latency with no analysis load (§7.8) |

The distinctness block (the xcand / filter-off / built-loop-filter rows) ran with the detector passes on the same box; its latency is not read — the last row re-reads the prod condition uncontended.

### 7.2 Latency — paired, three baseline brackets, two instruments

| run | wall p50 | wall p95 | engine stage mean | engine stage p50 | attempts/req |
|---|---|---|---|---|---|
| baseline bracket **K** | 1.091 s | 2.760 s | 754.0 ms | 679.0 ms | 12.62 |
| baseline bracket **L** | 1.012 s | 2.698 s | 735.1 ms | 657.5 ms | 12.62 |
| baseline bracket **M** | 1.116 s | 2.931 s | 780.5 ms | 693.0 ms | 12.62 |
| **baseline pooled (n = 3)** | **1.073 s** (sd 5.1 %) | **2.796 s** | **756.5 ms** | **676.5 ms** | **12.62** |
| P2 v2, run 1 | 1.049 s | 3.614 s | 811.0 ms | 633.5 ms | 13.50 |
| P2 v2, run 2 (tight) | 1.009 s | 3.831 s | 806.1 ms | 620.5 ms | 13.50 |
| **P2 v2 pooled (n = 2)** | **1.029 s** | **3.723 s** | **808.6 ms** | **627.0 ms** | **13.50** |
| **ratio, v2** | **0.959×** | **1.331×** | **1.069×** | **0.927×** | +0.88 |
| P2 v1 (bridging), single run | 1.152 s | 3.850 s | 842.9 ms (+249 ms of bridge searches not in the stage sum) | 733.5 ms | 14.71 |
| ratio, v1 | 1.074× | 1.377× | — | 1.084× | +2.09 |
| *P1.1 scoped knee, its own session (§16.4)* | *1.434 s vs 1.168 s* | | | | *14.15* |
| *ratio, knee* | ***1.227×*** | *1.076×* | *1.172×* | | |

The three brackets sit within 5.1 % sd — tight enough to resolve the bar, and both instruments put v2 **under 1.0× at the median**: wall 0.959×, engine-stage p50 0.927×. The mean tells the other half — engine-stage mean **1.069×** — because the pair pass is superlinear in harvest size and the long asks pay for it:

| ask | n/run | base p50 | P2 v2 p50 | × | base p95 | P2 v2 p95 | × |
|---|---|---|---|---|---|---|---|
| 20 km | 40 | 0.283 s | 0.247 s | 0.873× | 0.795 s | 1.339 s | 1.684× |
| 25 km | 20 | 0.706 | 0.744 | 1.054× | 1.549 | 1.175 | 0.759× |
| 30 km | 20 | 0.766 | 0.906 | 1.183× | 1.226 | 1.246 | 1.016× |
| 40 km | 20 | 1.102 | 0.906 | 0.822× | 1.721 | 1.322 | 0.768× |
| 50 km | 116 | 0.861 | 0.695 | **0.807×** | 1.633 | 1.518 | 0.930× |
| 60 km | 60 | 0.887 | 1.014 | 1.143× | 1.524 | 1.492 | 0.979× |
| 70 km | 40 | 1.210 | 1.199 | 0.991× | 1.946 | 1.642 | 0.844× |
| 100 km | 120 | 1.363 | 1.277 | 0.937× | 2.219 | 2.797 | 1.260× |
| 150 km | 10 | 3.127 | 3.171 | 1.014× | 4.324 | 5.006 | 1.158× |
| 200 km | 56 | 1.293 | 1.251 | 0.968× | 2.810 | 4.072 | 1.449× |
| **300 km** | 50 | 2.433 | 3.128 | **1.286×** | 4.467 | 7.136 | **1.597×** |
| block A Vračar | 100 | 0.926 | 0.920 | 0.994× | 1.655 | 1.355 | 0.819× |
| block B demand | 220 | 1.344 | 1.278 | 0.951× | 3.245 | 4.280 | 1.319× |
| block C v1 | 232 | 0.843 | 0.748 | 0.887× | 2.854 | 3.831 | 1.342× |
| **all** | 552 | 1.065 | 1.032 | 0.969× | 2.810 | 3.769 | 1.341× |

(Per-request wall from each response's `meta.latency_s`, three baseline runs pooled against the two v2 runs.) **The rider's asks — Vračar, the demand cells, 25–100 km — are flat or faster; the p95 regression is the 200–300 km block**, where the pass builds a 339 k-junction graph (up to 117 MB) and runs it for 0.5–0.9 s. The worst single request goes from 5.3 s (baseline max) to 10.9 s. §10 has the anatomy; §12 the levers.

### 7.3 Fills, failures, the rescue pass

| | baseline | P1.1 knee | **P2 v2** | P2 v1 |
|---|---|---|---|---|
| fills 12/12 | 550/552 | 552/552 | **552/552** | 552/552 |
| failures | 0 | 0 | **0** | 0 |
| `underfill` before the rescue pass (`cap` / `queue`) | — | 0 / 2 | 14 / 37 | 12 / 39 |
| requests the rescue pass fired on / loops it built | — | — | **51 / 477** | 51 / 475 |
| loops topped up from the gate's stash | — | 2 | 9 (7 requests) | 9 |
| gate rejects seam / twin / bounce | — | 537 / 1 949 / 96 | **159 / 722 / 0** | 159 / 2 907 / 79 |
| geometry refills (budgeted) / denied → gated tier | — | 767 / 745 | **127 / 436** | 844 / 1 983 |
| requests with any gate reject | — | 479 (86.8 %) | **69 (12.5 %)** | 448 |
| return-leg rungs r0 / r2 / none | — | 76.7 / 23.2 / 0.1 % | **90.6 / 9.4 / 0.0 %** | 56.6 / 42.8 / 0.6 % |

The engine is deterministic: the two v2 runs produced byte-identical meters and identical ledgers. Every quality number below is a single measurement without sampling error.

### 7.4 The absolute: spikes

| | `spike_ge_500m` | `spike_ge_30m` | stub p50 / p90 / max | where |
|---|---|---|---|---|
| baseline `b4f514d7f` | 0.00 % | 0.00 % | 0 / 0 / 0 m | — |
| P1.1 scoped knee | 0.00 % | 0.00 % | 0 / 0 / 0 m | — |
| **P2 v2** | **0.00 %** | **0.00 %** | **0 / 0 / 0 m** | at every level, in every block |
| P2 v1 (bridging) | 0.00 % | **4.48 %** (c0.5 4.0 %, c0.7 16.7 %) | 108 / 158 / 271 m | slots 5–11 only |

v1's stubs are the bridges: a local hard-excluded A\* between the two junctions of a non-reversible stretch reaches the bypass by riding out and back along a side street for 100–270 m. The seam gate sees stubs ≥ 500 m at the seam; the geometry gate's mid-return bounce (30 m) fired 79 times and the budget denied 61 of them. That is exactly the class the harness's 30 m spike meter exists for, and it puts v1 outside Gate v1.3's absolutes. The bridges also ride the same pavement both ways: v1's F01 mechanism is 12.6 % of loops (slots 9–11 33 %), its D1b ≥ 500 m 9.1 % (demand 20.4 %), its D4 fallback proxy 36.6 %. Dead on every axis; v2 is the branch.

### 7.5 Whole-bank quality — census → P1.1 knee → **P2 v2** (switchback-aware D1b, all three re-read)

| detector | ALL | block A Vračar | block B demand | block C v1 |
|---|---|---|---|---|
| **D1b unseen near-mirror ≥ 500 m** | 43.1 → 3.7 → **3.9 %** | **60.8 → 1.3 → 0.0** | **63.9 → 7.0 → 8.5** | 15.6 → 1.6 → 1.1 |
| D1b unseen ≥ 2 km | 37.3 → 0.2 → **0.0 %** | 59.3 → 0.6 → 0.0 | 56.3 → 0.1 → 0.0 | 9.8 → 0.1 → 0.0 |
| D1b unseen m, mean | 4 139 → 56 → **53** | 5 735 → 42 → **0** | 6 595 → 59 → 73 | 1 120 → 59 → 58 |
| D1b unseen / ride, mean | — → 0.001 → **0.001** | — → 0.001 → **0.000** | — → 0.001 → 0.001 | — → 0.001 → 0.001 |
| **D1 same-pavement run ≥ 500 m** | 29.0 → 11.3 → **9.2 %** | **32.6 → 6.9 → 0.0** | 23.9 → 4.0 → 5.7 | 32.3 → 20.2 → 16.6 |
| D1 ≥ 1 km ridden both ways | 27.5 → 7.7 → **5.5 %** | 31.4 → 5.3 → **0.0** | 22.4 → 1.1 → 0.2 | 30.7 → 15.0 → 13.0 |
| D3 ≥ 1 near-rejoin ring | 64.1 → 45.6 → **31.2 %** | 80.4 → 50.1 → 39.5 | 75.2 → 55.2 → 40.2 | 46.6 → 34.5 → 19.2 |
| D3 ring inside a single leg | 20.8 → 12.3 → 14.6 % | 14.2 → 7.2 → 6.0 | 29.2 → 21.7 → **29.7** | 15.7 → 5.5 → 4.0 |
| D3b figure-8 (crossing lobe ≥ 25 %) | 22.2 → 13.3 → **3.7 %** | 32.7 → 14.1 → 1.8 | 23.2 → 12.9 → 2.6 | 16.6 → 13.3 → 5.6 |
| D4 reuse outside the exemption (Fallback proxy) | 28.5 → 14.3 → **6.1 %** | 30.5 → 13.8 → **0.0** | 20.0 → 8.9 → **0.2** | 35.6 → 19.6 → 14.3 |
| D6 cross-leg shared corridor ≥ 25 % | 22.1 → 1.8 → **1.5 %** | 49.2 → 0.1 → 0.0 | 23.0 → 0.0 → 0.0 | 9.6 → 4.2 → 3.7 |
| D6 stem hidden by the 120 m gap (≥ 1.5 km at 1 km) | 20.1 → 2.1 → 3.4 % | 8.8 → 0.0 → 0.0 | 34.2 → 5.0 → 6.1 | 11.7 → 0.3 → 2.4 |
| D2 exempt-zone corridor ≥ 5 % of the ride | 2.5 → 2.2 → 2.2 % | 0.0 → 0.0 → 0.0 | 0.5 → 0.0 → 0.0 | 5.5 → 5.1 → 5.3 |

(All three columns are the switchback-aware read — §9 has what it changed on the knee, which is more than block C.)

Mechanism classification (share of loops / share of ride km), census → knee → **P2 v2**:

| mechanism | ALL | Vračar | demand |
|---|---|---|---|
| **near-mirror / parallel carriageway (F02)** | 35.6 % / 3.82 % → 0.4 / 0.01 → **0.0 / 0.00** | 53.7 / 13.53 → 0.7 / 0.05 → **0.0 / 0.00** | 53.9 / 6.95 → 0.2 / 0.00 → **0.0 / 0.00** |
| same-pavement retrace mid-route (F01) | 5.3 / 0.72 → 0.1 / 0.01 → 0.1 / 0.00 | 3.1 / 0.43 → 0.0 → 0.0 | 3.1 / 0.28 → 0.1 / 0.00 → 0.2 / 0.01 |
| intra-leg ring / near-rejoin (F22) | 5.2 / 0.19 → 11.3 / 0.22 → 14.2 / 0.23 | 2.8 / 0.23 → 5.2 / 0.41 → 6.0 / 0.39 | 7.8 / 0.16 → 21.4 / 0.38 → **29.5 / 0.48** |
| fallback-like heavy reuse | 11.9 / 1.26 → 9.1 / 0.89 → **5.0 / 0.81** | 8.4 / 0.78 → 9.2 / 0.52 → **0.0** | 4.5 / 0.26 → 3.0 / 0.06 → **0.0** |
| exempt-zone stem | 12.8 / 0.13 → 14.8 / 0.13 → 9.5 / 0.11 | 0.0 → 0.0 → 0.0 | 9.8 / 0.13 → 12.4 / 0.11 → 5.6 / 0.07 |
| seam residue · figure-8 through home | 0.3 · 0.1 → 0.0 · 0.0 → **0.0 · 0.0** | 0.6 · 0.0 → 0.0 → 0.0 | 0.2 · 0.0 → 0.0 → 0.0 |
| `clean` | 4.9 → 9.4 → **15.0 %** | 7.4 → 15.7 → **24.2** | 4.7 → 9.7 → 15.4 |

The classifier is exclusive (a loop is filed under its worst mechanism), so with F02 and the fallbacks gone, loops fall to the ring row: the *detector* prevalence of rings fell (D3 45.6 → 31.2 %, 0.59 → 0.38 rings/loop), the *residual* is now rings. On the demand cells 29.5 % of loops carry a ring in one leg — the F22 shape the census ranked fourth is what is left once the retraces are gone, and it is the Gate v2 question (§12).

### 7.6 Served-slot cleanliness, and the worst 20

| | census | P1.1 knee | **P2 v2** |
|---|---|---|---|
| **slots 0–2** near-mirror + same-pavement mechanism | 31.8 % | 0.6 % | **0.0 %** |
| slots 0–2 D1b unseen ≥ 500 m / mean m | 32.9 % / 3 462 | 3.4 % / 44 | **2.9 % / 44** |
| slots 0–2 D1 same-pavement run ≥ 500 m | 46.6 % | 11.4 % | 10.8 % |
| slots 0–2 `clean` | 8.5 % | 14.3 % | **19.1 %** |
| **slots 3–5** near-mirror + same-pavement | 35.0 % | 0.2 % | **0.0 %** |
| slots 3–5 D1b unseen ≥ 500 m / mean m | 40.2 % / 2 662 | 3.4 % / 45 | 5.0 % / 69 |
| slots 9–11 D1 same-pavement run ≥ 500 m | 60.9 % | 21.1 % | **7.6 %** |
| slots 9–11 fallback-like heavy reuse | 7.7 % | 22.6 % | **5.6 %** |
| ALL near-mirror + same-pavement / D1b mean m | 40.9 % / 4 139 | 0.5 % / 56 | **0.1 % / 53** |
| ring share (F22, detector) | 64.1 %, 1.09/loop | 45.6 %, 0.59/loop | **31.2 %, 0.38/loop** |
| ranking ledger: gated tier in slot 0 / slot 11 | — | 3.6 % / 35.3 % | 3.6 % / **10.5 %** |
| ranking ledger: rung r2 in slot 11 | — | 31.5 % | **0.5 %** |

The 3.6 % gated loops in slot 0 are the same 20 requests on both binaries — network-forced (nothing clean exists) — not a regression. The deep bank is the change: P1.1's slots 9–11 were where the denied twin rides and the rung-2 fallbacks lived; P2's slot 11 is 88.9 % rung-0.

Worst 20 by D1b unseen near-mirror metres:

| | P1.1 knee | **P2 v2** |
|---|---|---|
| worst loop | 4 002 m / 10.2 % of a 39.4 km ride, slot 11 | **913 m / 1.0 % of a 91.5 km ride, slot 4** |
| the twenty | slots 7–11: thirteen F02 near-mirror (Vračar 40–60 km, two demand cells, 1.1–4.0 km), seven fallback-like (Novi Sad 20 km, Belgrade 50 km) | **all twenty the same feature** — demand cell #2 (44.796, 20.437), 100 km, c0.5 and c0.7, slots 4–8: an `intra-leg ring / near-rejoin (F22)` whose two sides run within 25 m of each other for 913 m |
| slot spread | none in slots 0–6 | none in slots 0–3 |

There is no retrace left to rank; the near-mirror detector's worst hits are the two sides of a ring. Galleries in Appendix B.

### 7.7 Curviness, distance, compactness, lollipops

Curviness (`curviness_geom_clean` mean per level), baseline → knee → **P2 v2**, retention vs baseline:

| level | baseline | P1.1 knee | **P2 v2** | retention | note |
|---|---|---|---|---|---|
| c0.5 | 287.1 | 290.0 | **301.5** | **1.050×** | demand level |
| c0.7 | 206.3 | 219.9 | **263.6** | **1.278×** | demand level |
| c0.8 | 344.2 | 325.4 | 326.2 | 0.948× | advisory; block C's twin-exclusion cost, unmoved since P1 |
| c1.0 | 233.3 | 243.7 | **285.7** | **1.225×** | demand level |

Selecting on the pair's curviness over both legs is what the score of §3.2 does, and the bank shows it — the same axis, as §7.8 shows, on which the loops become each other's near-duplicates.

| v1.3 meter | census | P1.1 knee | **P2 v2** |
|---|---|---|---|
| `distance_error` mean / p90 / max | 0.163 / 0.283 / 8.35 | 0.155 / 0.308 / 2.32 | **0.123 / 0.193** / 3.62 |
| `compactness` mean / share < 0.10 | 0.226 (c0.5) / 21.8 % | 0.300 / 8.4 % | **0.317 / 3.6 %** |
| `edge_reuse_geom` mean | 0.0342 | 0.0155 | **0.0111** |
| `is_lollipop` | 0.7 % | 0.9 % | 0.9 % |
| Gate v1.3 **6a** lollipop ≤ 2 % per level | c0.8 1.3 % | c0.8 3.39 % | c0.5 1.07 %, c0.7 0, c1.0 0, **c0.8 4.17 %** (advisory level) |
| Gate v1.3 **6b** worst cell ≤ 55 % | c0.5 vlasina-50 48.3 %, c0.8 20.8 % | c0.5 50.0 %, c0.8 54.2 % | c0.5 vlasina-50 **41.7 %**, **c0.8 vlasina-50 66.7 %** (advisory level) |

The `distance_error` max is one loop: Djerdap 20 km at c0.5, slot 11, a 92 km loop served as the last resort on all five seeds (the census served 187 km in slot 8 for the same ask). Vlasina at c0.8 is the rescue pass's block (§3.4): the lake road is a tree, every loop there is a P1.1 rescue loop, and two thirds of the 50 km bank are stem-lollipops — worse than the knee's 54 %. It is an advisory level and a cell corpus-v1 pinned for continuity, but it is honest: where no pair exists P2 has nothing better than P1.1, and slightly less, because its rescue candidates are what the pair pass left.

### 7.8 Distinctness — ratchet 9, apples to apples

Ratchet 9 as re-fit at the P1.1 close: `bank_overlap_mean` ≤ 1.10× the baseline **with** `roundtrip_xcand_penalty` on at prod's 0.2 (0.4407 → bar 0.485). The penalty applies to every A\* P2 runs for a repair (92 % of returns), so "P2 + xcand 0.2" is the honest prod comparison.

| configuration | `bank_overlap_mean` | vs prod | `near_dup > 0.6` | Δpp vs prod | fills 12/12 | attempts/req | spikes |
|---|---|---|---|---|---|---|---|
| baseline `b4f514d7f`, xcand off (= the rig) | 0.4931 | 1.119× | 34.2 % | +10.2 | 550/552 | 12.62 | 0 |
| **baseline, xcand on 0.2 (= prod)** | **0.4407** | — | **24.0 %** | — | 550/552 | — | 0 |
| P1.1 scoped knee, xcand off | 0.5442 | 1.235× | 46.4 % | +22.4 | 552/552 | 14.15 | 0 |
| P1.1 scoped knee, xcand 0.2 | 0.4795 | 1.088× | 31.3 % | +7.3 | 552/552 | — | 0 |
| **P2 v2, xcand off, pair filter on** (§7.2's run) | **0.6214** | **1.410×** | **62.2 %** | **+38.2** | 552/552 | 13.50 | 0 |
| P2 v2, xcand off, pair filter **off** | 0.6398 | 1.452× | 67.5 % | +43.5 | 552/552 | 13.3 | 0 |
| **P2 v2, xcand 0.2** | **0.5387** | **1.222×** | **43.3 %** | **+19.3** | 552/552 | 13.1 | 0 |
| P2 v2, xcand 0.2 + built-loop filter 0.6 | 0.4762 | **1.081×** | **18.8 %** | −5.2 | **468/552** | **31.4** | 0 |
| P2 v2, xcand 0.2 + built-loop filter 0.5 | 0.4647 | 1.054× | 21.1 % | −2.9 | **404/552** | **41.1** | 0 |

Per level (`bank_overlap` / `near_dup > 0.6`):

| level | prod (0.2) | P2 off | P2 + 0.2 | P2 + 0.2 + filter 0.6 |
|---|---|---|---|---|
| c0.5 | 0.4613 / 27.4 % | 0.6291 / 62.5 | 0.5451 / 42.1 (1.182×) | 0.4775 / 18.5 (1.035×) |
| c0.7 | 0.3800 / 13.6 % | 0.5962 / 61.2 | 0.4986 / 37.7 (1.312×) | 0.4553 / 16.7 (1.198×) |
| c0.8 | 0.5537 / 46.4 % | 0.7098 / 73.2 | 0.6642 / 64.1 (1.200×) | 0.5690 / 33.8 (1.028×) |
| c1.0 | 0.3753 / 12.1 % | 0.5841 / 58.6 | 0.5039 / 44.2 (1.343×) | 0.4611 / 17.4 (1.229×) |

**Three readings, in order of what they cost:**

1. **The pair-keyed filter is nearly inert** (0.018) — §5's diagnosis: it judges the pair, the rider gets the repair.
2. **The penalty recovers a third of the gap** (1.410× → 1.222×) and no level passes. Which leg carries the excess — the same length-weighted best-sibling overlap as `bank_overlap`, split by the leg the shared metres lie on (`leg_overlap.py`):

| run | best-sibling overlap | on the forward leg | on the return leg | loops whose return shares > 50 % of its length with a sibling's | forward > 50 % |
|---|---|---|---|---|---|
| baseline `b4f514d7f` | 0.4931 | 0.2462 | 0.2469 | 47.4 % | 57.2 % |
| P1.1 scoped knee | 0.5442 | 0.2636 | 0.2806 | 55.0 % | 65.6 % |
| **P2 v2** | **0.6214** | **0.3035** | **0.3179** | **71.4 %** | **73.6 %** |
| P2 v2 + xcand 0.2 | 0.5387 | 0.2973 | **0.2414** | **47.7 %** | 72.3 % |
| P2 v2 + xcand 0.2 + filter 0.6 | 0.4762 | 0.2665 | 0.2097 | 34.1 % | 63.9 % |

   P2's excess sits on **both** legs (+0.057 forward, +0.071 return over the baseline). **The penalty fixes the return** — with xcand 0.2 P2's return legs are as distinct as prod's (0.241 vs 0.247; 47.7 % vs 47.4 % of loops share half their return) — **and leaves the forward leg** at 0.297 against 0.246: the tree paths out of the start. That is the pass's *selection*, not its score: 61 % of the curviest shortlist candidates are refused for having no pair (the cul-de-sac and valley-road turnarounds P1.1 served through a fallback), and the sinks that survive sit on the same well-connected trunk roads, so the K loops leave the start together. The pair-keyed filter sees the forward leg exactly (the tree path *is* the served forward leg) and still cannot bite: its 0.6 threshold is on the whole pair, and a forward trunk shared over 60 % of the forward leg is 30 % of the loop.
3. **The built-loop filter meets the bar by refusing to serve.** At 0.6 it rejects 5 104 built loops to keep 6 103, hits the attempt cap on 361 of 552 requests, leaves 84 banks short (404 short at 0.5), and takes 31 attempts per request against 13.5 — the A\* stage goes from 354 ms to over a second. Fills and the 1.10× latency bar both fall. On this network at K = 12, the bank the pair pass selects does not contain twelve corridor-distinct clean loops; the filter can only discover that.

One caveat the ratchet carries: its baseline's distinctness is partly *dirty*. The census served 25.6 % Fallback Loops and the knee's slots 9–11 were 31.5 % rung-2 — loops that ride home on a corridor no clean loop uses, and so read as distinct from every clean sibling. P2 serves none of those (slot 11: 0.5 % rung 2) and its clean bank crowds the same corridors. That is an argument for Gate v2 to write ratchet 9 against a clean baseline, not a P2 pass.

Latency in the penalty block: the P2 + xcand run of this block overlapped the detector passes (its pass stage read 270 ms against 191 in the uncontended runs, on a stage the penalty does not touch), so its 1.486 s p50 is not a reading. **Re-measured uncontended** (phase 4, both engines at xcand 0.2, one bracket each, identical meters): baseline **1.189 s** p50 / 3.12 s p95, engine stage 860.7 ms mean / 757 ms p50, attempts 12.56; **P2 v2 1.102 s / 4.05 s**, 850.2 ms / 637 ms, attempts 13.06 — **0.927× wall p50, 0.988× engine-stage mean, 1.30× p95.** The penalty costs the baseline more A\* than it costs P2 (554 vs 485 ms against 387 vs 356), so under the prod condition P2 is under 1.0× on the mean as well as at the median.

### 7.9 The selection ledger

Per request, means over 552 (v2): 1 710 pair evaluations (p50 730) → 11.06 chosen + backfill → 12.34 built → 12 served. `no_pair` 1 042 / `band` 278 / `fwd_illegal` 150 / `share` 130 / `twin` 34 per request. Pass: build 127 ms (p50 108, max 580), run 54 ms (p50 34, max 350), evaluation 113 ms (p50 52). `sinks_with_pair` 65.5 k of 103 k junctions. `rev_fail` tile/noopp/noret/turn/gap = 0 / 0 / 441 711 / 35 646 / 0 (v1 adds 18 008 gaps). `swapped` 2.5 % of evaluations. `pair-ranks` per slot: pair-built share 92–94 % (the rest rescue), full repair 90–94 %, pair cost 9.0–11.3 k with a surplus (`d'`) of 1.6–2.2 k over `2·d(s,J)` — the second path costs 17–20 % more than the tree path it is disjoint from.

## 8. Kill criteria

| # | criterion (ticket, locked 2026-09-06) | bar | P1.1 knee | **P2 v2** | verdict |
|---|---|---|---|---|---|
| 1 | wall p50 latency vs the census baseline | ≤ 1.10× | 1.227× | **0.959×** (engine-stage p50 0.927×, mean 1.069×); prod condition, both engines at xcand 0.2, uncontended: **0.927×** (engine mean 0.988×) | **PASS** |
| 1b | wall p95 | report | 1.076× | **1.331×** (300 km: 1.60×; Vračar 0.82×) | regression, tail |
| 2 | seam spikes | 0 | 0 | **0** (`spike_ge_30m` 0.00 % at every level; v1 4.48 %, dead) | **PASS** |
| 3 | fills K = 12 | 552/552 | 552/552 | **552/552** (rescue pass on 51 requests) | **PASS** |
| 4 | failures | 0 | 0 | **0** | **PASS** |
| 5 | Vračar near-mirror (D1b ≥ 500 m) | ≤ 1.5 % | 1.3 % (1.5 % legacy read) | **0.0 %** (mean 0 m) | **PASS** |
| 6 | demand near-mirror (D1b ≥ 500 m) | ≤ 8 % | 7.0 % (8.2 % on the legacy read the bar was set on) | **8.5 %** by count (1.5 pp above the knee), mean 73 m vs 59; every entry a sub-km ring at one cell; F02 mechanism **0.0 %** (knee 0.2 %) | **FAIL by the count, PASS by mechanism** — Gate v2 decides which the bar means (§7.6, §12) |
| 7 | slots 0–2 near-mirror + same-pavement | ≤ 5.5 % | 0.6 % (5.5 % legacy read) | **0.0 %** | **PASS** |
| 8 | beats P1 on D1 / D1b ride-fraction, Vračar + demand (P1 re-read with the same detector) | — | — | D1b/ride Vračar **0.000** (P1 0.005; mean 0 vs 167 m), demand 0.001 (P1 0.001; mean 73 vs 75 m — a tie); D1 both-ways Vračar **0.0 %** (P1 10.5 %), demand **0.2 %** (P1 7.0 %) | **PASS** on D1 and on Vračar; tie on demand D1b |
| 9 | bank distinctness vs baseline-with-penalty 0.2 | ≤ 1.10× (`bank_overlap` ≤ 0.485) | 1.088× (knee + 0.2) | penalty off **1.410×** / +38 pp; penalty on **1.222×** / +19 pp; + built-loop filter 0.6 **1.081×** / −5 pp but fills **468/552** and 31 attempts/request | **FAIL** — met only by a configuration that fails bars 1 and 3 |
| 10 | curviness retention, demand levels | ≥ 0.95× | 1.010 / 1.066 / 1.044 | **1.050 / 1.278 / 1.225** | **PASS** |
| 10b | c0.8 | advisory | 0.945× | 0.948× | advisory |
| 11 | Gate v1.3 absolutes (1, 2, 3, 4, 5, 7) | pass | pass | **pass at every level** | **PASS** |
| 11b | Gate v1.3 6a lollipop ≤ 2 % | — | c0.8 3.39 % | c0.5 1.07 %, c0.7 / c1.0 0; c0.8 4.17 % | FAIL at c0.8 (advisory) |
| 11c | Gate v1.3 6b worst cell ≤ 55 % | — | c0.8 54.2 % | c0.5 41.7 %; **c0.8 66.7 %** (vlasina-50, rescue loops) | FAIL at c0.8 (advisory) |
| 12 | fallback share | report | rung 2 23.2 %; D4 14.3 % | rung 2 **9.4 %**; D4 **6.1 %** (Vračar 0.0, demand 0.2) | improved |
| 13 | rings (F22) | report | D3 45.6 %, 0.59/loop | D3 **31.2 %**, 0.38/loop; residual mechanism now rings (demand 29.5 %) | improved; the next residual |
| 14 | gurka | green | 44 | **49** | **PASS** |

**Ten of the numbered bars pass outright; the demand near-mirror count misses by half a point above the bar (and 1.5 pp above the knee on the same read) while its mechanism is clean; the tail latency and the advisory c0.8 lollipop rows regress; ratchet 9 fails — the one configuration that meets it breaks fills and more than doubles the A\* work (§7.8).** **For the ladder:** P2 is the first binary to hold every absolute at ≤ 1.10× p50 — the thing P1.1 measured twice as impossible on its own mechanism — and the first to serve the rider's blocks with no retrace mechanism left at all. It fails ratchet 9 as written, and the failure is structural (the bank the pass selects), not a knob. The keep/shelve call is Andrey's on the galleries (Appendix B); this write-up's recommendation is §12's: keep the pass as the selection stage and put diversity into the selection, where the pass already holds every pair before any search.

## 9. The switchback-aware D1b

The same-road study (2026-09-06, #13) found that 15 of P1.1's 20 worst loops by D1b were Zlatibor hairpins: the near-mirror detector matched a run of the ride against its own index range (the road folding back on itself inside one pass) and called it a retrace. `lqbs_lib.antimirror_runs` now drops a run whose partner index range overlaps its own (`~/.curvagen-scratch/p2/lqbs_lib.py`, `SWITCHBACK_AWARE`, env `LQ_D1B_LEGACY=1` restores the old read byte for byte). Every D1b number in this document — census, P1.1 knee, P2 — is the new read.

What it changed, legacy → switchback-aware:

| | census | P1.1 scoped knee |
|---|---|---|
| D1b ≥ 500 m, ALL | 49.6 → **43.1 %** | 7.8 → **3.7 %** |
| D1b ≥ 500 m, block C | 30.9 → **15.6 %** | 10.1 → **1.6 %** |
| D1b ≥ 500 m, Vračar | 61.1 → 60.8 % | 1.5 → 1.3 % |
| D1b ≥ 500 m, demand | 64.1 → 63.9 % | 8.2 → **7.0 %** |
| D1b unseen m, mean | 4 139 → 4 139 | 117 → **56** |
| F02 mechanism, ALL / slots 0–2 | 35.6 % / 31.8 % (unchanged) | 3.0 / 5.5 → **0.4 / 0.6 %** |
| worst loop by D1b | 48 596 m (unchanged) | 4 002 m (unchanged) |

**On the census the mountain block halves and the rider's blocks move by ≤ 0.3 pp — on the knee, where the retraces were already gone, the hairpins were most of what the legacy read had left, and the demand block moves 1.2 pp, the mechanism classifier from 3.0 % to 0.4 %.** Two consequences: the P1.1 §16 tables overstate what P1.1 left behind (its slots 0–2 were 0.6 % near-mirror, not 5.5 %), and the ticket's demand bar of 8 % was set on a number that reads 7.0 % on the detector every P2 column uses. For Gate v2 the consequence is the one #13 predicted: a D1b threshold set on the legacy read would have been set against hairpins; and a bar written as a *count over 500 m* now separates rings from retraces no better than the legacy read separated hairpins — the mean-metre or the mechanism read does (§12).

## 10. Latency anatomy

Mean engine stage ms/request, baseline pooled (3 brackets) → **P2 v2** pooled (2 runs):

| stage | baseline | **P2 v2** | Δ |
|---|---|---|---|
| harvest (expansion + `ScanBand` + F01 forest pass) | 71.4 | 91.0 | +19.6 |
| widened `ScanBand` | 16.2 | 7.1 | −9.1 |
| rejoin map build (+ twins, parallels) | 2.1 | 4.5 | +2.4 |
| **pair pass** (junction graph build 127 + labeling 54 + …) | — | **189.2** | **+189.2** |
| **pair evaluation** (construct + walk the shortlist's pairs) | — | **105.8** | **+105.8** |
| **return-leg A\*** | 486.7 | 354.1 | **−132.6** |
| **fallback A\*** (rung ladder off on P2) | 146.7 | 28.0 | **−118.7** |
| seam decode | 1.3 | 0.6 | −0.7 |
| Second Via (baseline) / geometry gate decode (P2) | 8.5 | 4.1 | −4.4 |
| TripLegBuilder | 23.6 | 24.4 | +0.8 |
| **total (mean)** | **756.5** | **808.6** | **+52.1 (1.069×)** |
| **total (p50)** | **676.5** | **627.0** | **−49.5 (0.927×)** |

The two A\* lines give back 251 ms and the pass takes 295 ms — a wash on the mean, a win at the median, because the A\* saving is per attempt (13.5 attempts that almost all succeed: `astar_fb` 147 → 28 ms is the fallback A\* nearly vanishing, and it vanishes because a sink with a certified pair has a way home the hard-excluded A\* finds first time) while the pass cost is per junction in the harvest (95 k at the median, 339 k at 300 km, and `bytes` 31 → 117 MB). The knee's +140 ms was 87 % A\* (the seam refills the scoping bought back); P2's A\* is **cheaper than the baseline's** (354 vs 487 ms) with the same absolutes held — that is the mechanism the ticket named, measured: **gate-rejects happen at selection, so the replacement build is never paid for** (1.60 gate fires per request against the knee's 4.68).

Against the P1.1 knee in its own session (stage mean 950.8 ms vs its baseline 811.1): P2 v2 at 808.6 vs 756.5. Cross-session stage totals are not comparable in absolute terms (the box's load differs: the same baseline binary read 811 ms there and 756 ms here), only the ratios: **1.172× → 1.069×** on the mean, **1.227× → 0.959×** on wall p50.

The v1 anatomy is different and worse: bridge searches 249 ms/request on top (34 local A\* per request), rejoin 82.6 ms, attempts 14.71, and the stubs of §7.4.

## 11. What did not work

1. **Bridging (v1).** Local hard-excluded A\* across each non-reversible stretch: 6.2 bridges per request, 249 ms, and 100–270 m stubs on 4.5 % of loops (16.7 % at c0.7) that no engine gate sees and the 30 m spike meter does. The stub is the bridge itself reaching the bypass. A bridge that is also barred from riding *its own* side street both ways would need the seam-stub logic at every bridge end — at which point it is the full repair with extra steps. Dead.
2. **The pair as a rideable loop.** Only 7.6 % of pairs ride home unrepaired (§1.4); the return the rider gets is `route_leg`'s in 92.4 % of cases. The pass earns its keep as a *selector* — existence, band, twin test (return-vs-return included), cost, curviness over both legs, all before any search — not as a constructor. Everything the ticket credited to "jointly optimal construction" is in fact delivered by *selection on a certificate*; the certificate's second path is then discarded. That is a cheaper mechanism than the survey costed (no per-candidate min-cost circulation was needed), and it is why the latency half was won.
3. **The pair-keyed K × K sharing filter (§5).** `share=` rejects 7.6 % of evaluations and the served bank is still 1.26× less distinct than the rig baseline (1.41× vs prod): it tests the pair's roads against previous pairs' roads, and the served returns are repairs. A filter that cannot see the served loop cannot bound its overlap. The two mechanisms that can — the xcand penalty on the repair A\* and the ADR-0040 built-loop filter — are measured in §7.8: the penalty recovers a third of the gap (1.410× → 1.222×), the filter the rest (1.081×) at the price of 84 short banks and 31 attempts per request. The bank is less diverse *before* any filter, on both legs; the penalty restores the return legs to prod's level, and the forward excess is the selection's — the pair-eligible sinks cluster on the same trunk roads out of the start (§7.8).
4. **Twin folding (`roundtrip_pair_twin_join_m`).** Union-find over sidecar pairs at 60 m folded 23 % of Belgrade's junctions and 9 % of Zlatibor's into vertices the ride cannot cross (no crossover exists at a folded junction), and every forward leg through one failed on `gap=`. Left in the code, off by default, not measured on the corpus.
5. **The rescue pass is not optional.** The first binary — pairs only — failed 29 requests and under-filled 22: a tree-shaped network (Vlasina's lake road, Djerdap's gorge road at 20 km) has no disjoint pair in the band at all. The rescue pass (P1.1's build on the candidates the pair pass left) is what fills them, and it fills them with P1.1 loops: the c0.8 Vlasina lollipop share is the price (§7.7).
6. **The 300 km tail.** The pass scales with the harvest (339 k junctions, 117 MB, 0.5–0.9 s) and the return A\* saving does not: 1.29× p50 / 1.60× p95 at 300 km, worst request 10.9 s. No demand exists at 300 km (prod cache reading: never asked), but the corpus carries it for continuity and Gate v2 will have to say whether the ratchet is per-ask or pooled.
7. **The demand-cell ring residual.** With retraces gone, 29.5 % of demand-cell loops carry an intra-leg ring (knee 21.4 %) — the F22 shape the census ranked fourth. The pair pass does not see rings (a ring is disjoint from itself), the harvest's F01 hygiene drops only twin-non-simple chains, and the built-loop score has no ring term. Not a regression the pass caused, but the residual it exposes.

## 12. Open questions

**For the ticket's close (Andrey, gallery):** keep or shelve — the mechanism passed every quality and latency bar it was set except the tail; distinctness is the open half and the levers are on the branch.

**For P2.1 / the v4 build (if kept):**
1. **Distinctness at selection, not by refill.** Neither lever on the branch meets ratchet 9 inside the fills bar (§7.8): xcand 0.2 lands at 1.222×, the built-loop filter at 1.081× with 84 short banks. The lever not on the branch is selection-time diversity *on the forward leg*, which the pass knows exactly (the tree path is the served forward leg): a per-leg sharing threshold at selection (a forward trunk shared over half the forward leg with a chosen candidate rejects the sink — the whole-pair 0.6 cliff cannot see it), a diversity term in the pair score instead of a cliff, and the xcand penalty for the return, which §7.8 shows is enough for that leg — all walks, no builds. A cheaper variant than the item-4 refill: key `pair_built_keys` on the *built* loop (`loop_keylen` of `built->fwd/ret`) instead of `pair_cache[cand].keys` — one line at `route_action.cc:3098` — so the selection-time test at least sees the returns already served this request; it cannot see a candidate's own future repair, so it bounds only half the overlap.
2. **The 300 km pass cost.** The junction graph is rebuilt per request from the label forest; a coarser graph (fold degree-2 junctions into single arcs — the harvest forest is mostly chains) would cut vertices by an order of magnitude at no loss to the pairs; alternatively cap the pass at a harvest size and fall through to the rescue pass.
3. **Ring-aware selection.** A pair's forward path can be walked for self-proximity (the D3 test) at the same cost as the twin walk; whether rings are a costing phenomenon (ADR question, out-of-scope exception on the map) or a selection one, the pass is the first place with the whole loop in hand before any search.
4. **Return-legal tree arcs (§1.3(3)).** The one place the formulation is not direction-aware. A two-pass variant (run the pass on the return-legal subgraph only for the `Q` phase) would raise the reversible share above 7.6 % and make the pair a loop more often; worth measuring only if repairs turn out to cost quality (they do not, on this corpus).

**For Gate v2:** D1b threshold on the switchback-aware read only; ring (D3) becomes the first residual meter once the D3 counter fix is trusted; whether the latency ratchet is pooled p50 (P2 passes), per-block (P2 passes on every rider block), or per-ask p95 (P2 fails at ≥ 200 km); the demand D1b bar written as a mean-metre bound (P2 73 m) rather than a count over 500 m; provenance (D4) on the wire, since the rescue-vs-pair provenance is now the meaningful tier.

## Appendix A — commands

```bash
# build: the warm audit container (rt-p1-build, :8003), source docker-cp'd, incremental make
docker cp src/thor/roundtrip_pairs.cc rt-p1-build:/src/valhalla/src/thor/   # etc.; then
docker exec rt-p1-build bash -lc 'cd /src/valhalla/build && make -j8 valhalla_service gurka_roundtrip_audit'
# gurka (2026-09-07, all three suites): ~/.curvagen-scratch/p2/gurka-p2.log
docker exec rt-p1-build bash -lc 'cd /src/valhalla/build && ./test/gurka/gurka_roundtrip_audit --gtest_brief=1'

# engines: configs layered into /tmp inside each container; start/stop by exec (never PID 1)
docker exec -d rt-p1-build /tmp/start_engine.sh v8003-p2v2.json p2v2b      # -> /tmp/eng-p2v2b.log
docker exec -d rt-p11-base /tmp/start_engine.sh v8004.json baseK

# the paired protocol (one engine at a time, 3 workers, way pass on): ~/.curvagen-scratch/p2/p2_lib.sh
~/.curvagen-scratch/p2/p2-phase1.sh    # base K -> v2 (first binary, failed) -> ...
~/.curvagen-scratch/p2/p2-phase1b.sh   # base L -> v2b -> base M -> v2b-tight -> v1b   (the measured block)
~/.curvagen-scratch/p2/p2-phase2.sh    # v2 + xcand 0.2 -> v2 pair filter off
~/.curvagen-scratch/p2/p2-phase3.sh    # v2 + xcand 0.2 + built-loop filter 0.6 -> 0.5
~/.curvagen-scratch/p2/p2-phase4.sh    # the prod condition uncontended: baseline + xcand 0.2 -> v2 + xcand 0.2
~/.curvagen-scratch/p2/p2_probes.sh && python3 ~/.curvagen-scratch/p2/probe_agg.py eng-probe.log probes.list   # §1.4 per-block probes
python3 ~/.curvagen-scratch/p2/leg_overlap.py baseline=census-v2-p2-baseK knee=p1-1-scoped-knee p2v2=p2-v2b ...  # §7.8 which leg shares

# detectors (P1.1 scripts + fixed D3 + SWITCHBACK-AWARE D1b), re-pointed by env; LQ_D1B_LEGACY=1 = old read
~/.curvagen-scratch/p2/p2-analyse-all.sh p2-v2b p2v2b p2-v2b-tight p2v2bt p2-v1b p2v1b p1-1-scoped-knee p2knee
LQ_RUN=results/census-v2-b4f514d7f LQ_PREFIX=p2cen python3 census_agg.py det   # census re-read

# ledgers and latency
python3 ~/.curvagen-scratch/p2/p2_ledger.py ~/.curvagen-scratch/p2/eng-p2v2b.log
python3 ~/.curvagen-scratch/p2/ledger_agg.py ~/.curvagen-scratch/p2/eng-p2v2b.log
python3 ~/.curvagen-scratch/p2/stage_total_p2.py ~/.curvagen-scratch/p2/eng-{baseK,baseL,baseM,p2v2b,p2v2bt,p2v1b}.log
python3 ~/.curvagen-scratch/p2/latency_table.py baseK=census-v2-p2-baseK p2v2b=p2-v2b ...
python3 ~/.curvagen-scratch/p2/p2_summary.py base=census-v2-p2-baseK p2v2b=p2-v2b ...   # kill-criteria inputs

# compare tables and galleries
./loopqual compare results/p1-1-scoped-knee/report.json results/p2-v2b/report.json > results/p2-v2b/compare/vs-p1-1-scoped-knee.md
LQ_RUN=results/p2-v2b LQ_PREFIX=p2v2b LQ_RUN_B=results/p1-1-scoped-knee LQ_PREFIX_B=p2knee \
  python3 ~/.curvagen-scratch/p2/p2_gallery.py results/p2-v2b/gallery-p2-worst20.html results/p2-v2b/gallery-p2-vracar-paired.html
cd tools/loopqual/results && python3 -m http.server 8791 --bind 127.0.0.1     # then /p2-v2b/gallery-p2-vracar-paired.html
```

## Appendix B — artefacts

| Path | What |
|---|---|
| **`tools/loopqual/results/p2-v2b/`** | **the measured run**: 552 responses, `loops.jsonl` (6 624), `report.{json,md}`, way pass on |
| `tools/loopqual/results/p2-v2b-tight/` | the latency repeat (byte-identical meters) |
| `tools/loopqual/results/p2-v1b/` | the bridging variant (dead: §7.4) |
| `tools/loopqual/results/p2-v2/` | the first binary's run — 29 failures, the §3.4 evidence |
| `tools/loopqual/results/census-v2-p2-base{K,L,M}/` | the three baseline brackets (pooled p50 1.073 s, sd 5.1 %) |
| `tools/loopqual/results/p2-v2-xcand/` · `p2-v2-noshare/` · `p2-v2-xcand-filter{,05}/` | the distinctness block (§7.8) |
| `tools/loopqual/results/census-v2-p2-baseX/` · `p2-v2-xcand-uncontended/` | the prod condition (both engines at xcand 0.2), uncontended latency (§7.8) |
| `~/.curvagen-scratch/p2/{eng-probe.log,probes.list,probe_agg.py,leg-overlap.txt}` | the §1.4 probes and the §7.8 per-leg overlap read |
| **`tools/loopqual/results/p2-v2b/compare/`** | `loopqual compare` vs census-v2, vs the P1.1 scoped knee, v2 vs v1 |
| **`tools/loopqual/results/p2-v2b/gallery-p2-vracar-paired.html`** | **the rider's view: every Vračar 25–60 km ask at c0.5 / 0.7 / 1.0 (seed 7), served slots 0–2, P1.1 knee loop then P2 loop** |
| `tools/loopqual/results/p2-v2b/gallery-p2-worst20.html` | 20 worst P2 by D1b beside 20 worst P1.1 knee (same axis as the P1 / P1.1 galleries) |
| `~/.curvagen-scratch/p2/eng-{baseK,baseL,baseM,p2v2b,p2v2bt,p2v1b,p2v2x,p2v2ns,p2v2xf,p2v2xf5}.log` | engine ledgers (pair-pass / pair-select / pair-ranks / pair-loop / pair-repair + the P1.1 lines) |
| `~/.curvagen-scratch/p2/{p2_lib.sh,p2-phase*.sh,p2-analyse-all.sh,analyse.sh}` | the drivers |
| `~/.curvagen-scratch/p2/{p2_ledger,ledger_agg,stage_total_p2,latency_table,p2_summary,slot_clean,p2_gallery}.py` | ledger, latency, kill-criteria, slot and gallery tooling |
| `~/.curvagen-scratch/p2/lqbs_lib.py` (+ `lqbs_*.py`, `census_*.py`) | the detectors with the switchback-aware D1b |
| `~/.curvagen-scratch/{p2v2b,p2v2bt,p2v1b,p2knee,p2cen}*.jsonl` · `~/.curvagen-scratch/p2/agg-*-{det,mech,v13,slot}.txt` | detector output and tables: P2 runs, the knee re-read, the census re-read |
| `~/.curvagen-scratch/p2/gurka-p2.log` | the 49-green run |

Containers: `rt-p1-build` (P2 build + engine, :8003) and `rt-p11-base` (baseline, :8004) are left **running** (`sleep infinity`; the engines inside are stopped/started by exec). `valhalla-local` (:8002) and the :8791 results server were never addressed.

## Appendix C — the branch

`proto/v4-p2`, cut from `proto/v4-p1.1` @ `3ddb8db48`; **nothing pushed**.

| commit | what |
|---|---|
| `0e4654056` | the Suurballe–Tarjan pass on the junction graph (`src/thor/roundtrip_pairs.{cc,h}`, `ShortestPairs` + `RoundTripPairPass`), selection by pair, pair-built returns with bridging / full repair, twins-aware selection rejects, the pair-keyed K × K filter, the ledger; gurka P2 core / P2a / P2b / P2c / P2d |
| `97e774095` | the rescue pass (P1.1's build when the pair pass leaves the bank short), pair twin test decoupled from the gate knob, twin fold off by default, gurka maps sized to the harvest region |
| `1506af16d` | the rescue pass gets its own stall grant |
| `83837debc` | the twins-aware post-build K × K sharing filter (the ticket's xcand replacement, §5) and its ledger count — **the measured binary** |

`git diff --stat proto/v4-p1.1..proto/v4-p2`: 8 files, +2 709 / −74 — `roundtrip_pairs.cc` 921 lines, `route_action.cc` +1 064, `test_roundtrip_audit.cc` +519, headers and `worker.{cc,h}` knobs. Knobs (all `thor.`): `roundtrip_pair_pass` (false), `roundtrip_pair_bridge` (true — the corpus ran it false), `roundtrip_pair_sharing` (true), `roundtrip_pair_band` (0.20), `roundtrip_pair_shortlist` (8), `roundtrip_pair_twin_join_m` (0), `roundtrip_pair_max_bridges` (12), `roundtrip_pair_return_legal` (true), `roundtrip_pair_eval_cap` (600), `roundtrip_pair_two_way_tree` (true), `roundtrip_pair_twin_reject` (true), `roundtrip_sharing_filter` (false — ADR-0040 item 4, now twins-aware in pair mode), `roundtrip_sharing_frac` (0.6). This document is committed on the branch as the measurement (P1 / P1.1 precedent) and copied to `curvature-costing` on Andrey's go.

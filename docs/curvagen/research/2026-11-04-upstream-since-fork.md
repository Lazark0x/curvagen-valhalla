# Upstream Valhalla since the fork's base: what we can use

_Research, 2026-11-04. Question: what has upstream Valhalla added since our fork's base that could improve round-trip route generation (quality, variety) or performance (latency, memory, tile/build time)?_
_Sources: upstream git history (`git fetch upstream`), upstream `CHANGELOG.md` at `master`, GitHub releases, and the PR pages and diffs on `github.com/valhalla/valhalla` (each linked inline). Conflict claims come from `git merge-tree` runs against `curvature-costing`; no branch was modified and nothing was built._

## TL;DR

1. **The base is exactly 3.8.2.** `git merge-base curvature-costing upstream/master` = `17af0d031` (Release 3.8.2, 2026-07-08). The fork carries **no** upstream commits beyond it, so `49cd28bc5` is *not* in the fork; it was only ever the "do not rebase past" marker. Upstream `master` is at `2085e78a5` (2026-11-03): **100 commits, 3 releases (3.8.3, 3.9.0, 3.9.1), 443 files** ahead.
2. **The #6210 rebase hazard is resolved.** [#6303](https://github.com/valhalla/valhalla/pull/6303) merged 2026-09-08 and shipped in 3.9.0. It removes the hard `find_program(PYTHON_INTERPRETER …)` + `FATAL_ERROR` from the root `CMakeLists.txt` and puts the generated `locales/*.json` back in the repo, which our Dockerfiles' `posix_locale` loop reads. **Rebasing is unblocked.**
3. **Upstream has added nothing for our hot path.** Between 3.8.2 and `master` there is no A\*, bidirectional-A\*, Dijkstra, alternates, hierarchy, edge-label or tile-cache performance work. `alternates.cc`, `dijkstras.h`, `pathalgorithm.h`, `bidirectional_astar.h` and `edgelabel.h` are byte-identical. `bidirectional_astar.cc`, `unidirectional_astar.cc` and `dijkstras.cc` changed only for destination-only/restriction correctness and `linear_cost_factors` access. The memory pressure from round-trip v4 (~1 070 MiB) is fork-owned. Upstream does not help with it.
4. **The real wins are in the tile build, and they need a rebuild.** Hilbert-curve node ordering ([#6342](https://github.com/valhalla/valhalla/pull/6342)) improves the page locality of `NodeInfo` (mean 4 KiB-page span 7 546 m → 3 303 m on a France extract) and suits our mmap-tar serving. Dropping unused shortcut/bridge/tunnel/ferry elevation ([#6340](https://github.com/valhalla/valhalla/pull/6340)) cuts about 0.7 % of tile bytes. A shortcut-dedup fix ([#6008](https://github.com/valhalla/valhalla/pull/6008)) and several build-time speedups also land. None of these has a published serving-latency benchmark, so we have to measure them on the rig.
5. **Rebase blocker #1 is our own proto field numbers.** Upstream took `Costing.Options` field **99** (`steps_factor`, [#6394](https://github.com/valhalla/valhalla/pull/6394), merged 2026-10-08), which is our `curviness_continuity`. The open [#6449](https://github.com/valhalla/valhalla/pull/6449) claims **100**, our `prefer_elevation`. The 2026-09-05 survey predicted this collision, and it has now happened. **Move the fork's fields to a private range before rebasing.** That is wire-neutral for JSON clients, so no FINGERPRINT bump.
6. **The most relevant open PR is upstream's own motorcycle curvature option.** [#6449](https://github.com/valhalla/valhalla/pull/6449) `use_curvature` (open since 2026-10-30, maintainer-authored, one maintainer "lgtm", review still required) makes curvy roads preferable by penalising straight roads (factor ≥ 1) instead of discounting curvy ones. That keeps the A\* heuristic admissible, which is the exact fix our return-leg admissibility gap needs. Watch it, and do not take it blindly: it would collide with our names and field numbers.
7. **Recommendation:** first relocate the proto fields (a fork-only commit). Then rebase onto **3.9.1** in a worktree, boot the new engine on the **existing** tiles (the tile layout is stable), run Gate v2 and bump FINGERPRINT. Treat a tile rebuild on the new mjolnir as a **separate**, separately measured step. Details in §6.

## 1. Where we stand

| | sha | date | note |
|---|---|---|---|
| merge-base | `17af0d031` | 2026-07-08 | `Release 3.8.2 (#6192)`; tag `3.8.2` |
| fork serving HEAD | `5b29936f0` | 2026-09-07 | `curvature-costing`, 70 fork commits on 3.8.2 |
| upstream 3.8.3 | `a60c7cbfc` | release 2026-07-25 | 14 commits |
| upstream 3.9.0 | `a3a5631c4` | release 2026-09-19 | |
| upstream 3.9.1 | `bafb69902` | release 2026-10-06 | "Latest" on GitHub; 67 commits after 3.8.2 |
| upstream master | `2085e78a5` | 2026-11-03 | 33 commits after 3.9.1 |

Release dates are from `gh release list -R valhalla/valhalla`. The fork's own diff against 3.8.2 touches 30 non-doc files (+9 971 / −46), and nearly all of it is new fork files (`roundtrip_*`, `road_twin_index.*`, gurka suites).

## 2. Build hazards in the window

| Change | PR | Effect on our images |
|---|---|---|
| Python required at configure time | [#6210](https://github.com/valhalla/valhalla/pull/6210) (2026-08-20) | **Resolved by [#6303](https://github.com/valhalla/valhalla/pull/6303)** (2026-09-08, in 3.9.0). Python is no longer required for `libvalhalla`; `locales/*.json` are back in the repo, and `src/odin/CMakeLists.txt` compiles them via `ValhallaBin2Header.cmake`. `Dockerfile`/`Dockerfile.local` need no change for this. |
| prime_server pinned + built with CMake | [#6105](https://github.com/valhalla/valhalla/pull/6105) (2026-09-17) | `scripts/install-linux-deps.sh` now clones prime_server **0.12.0** and builds it with CMake (it was unpinned HEAD with autotools before). This invalidates the slow deps layer in `Dockerfile.local`, so the first build is a full one. It also adds `--no-prime-server`, which we must not pass. |
| `valhalla_service` uses `inproc://` by default | [#6105](https://github.com/valhalla/valhalla/pull/6105) | `--inproc-sockets` defaults to `true`: loopback, interrupt and the loki/thor/odin proxies use zmq shared memory instead of ipc sockets. Only `tcp://` listen addresses are accepted. Our entrypoint (`valhalla_service ${CONFIG_FILE} ${server_threads}`) still parses. See the latency note in §3.1. |
| New submodule `third_party/flatbush` | [#6156](https://github.com/valhalla/valhalla/pull/6156), [#6360](https://github.com/valhalla/valhalla/pull/6360) | The `build-amd64` CI checks out `submodules: recursive`, so it is fine. `Dockerfile.local` copies host submodules, so a local build needs `git submodule update --init --recursive` after the rebase. |
| C++20 `std::format` replaces `boost::format` | [#6363](https://github.com/valhalla/valhalla/pull/6363) | Needs GCC ≥ 13. `ubuntu:24.04` ships GCC 13, so this is fine. Boost stays a dependency (`libboost-all-dev`, `find_package(Boost 1.71)`), so the fork's `test_roundtrip_audit.cc` `#include <boost/format.hpp>` still compiles. |
| C++20 bitfield initialisers in `DirectedEdge` / `NodeInfo` / `GraphTileHeader` | [#6232](https://github.com/valhalla/valhalla/pull/6232) (2026-10-26) | Layout-neutral, so tiles stay compatible (§3.2). Expect merge noise only. |
| Python bindings default `ON` | (unchanged since 3.8.2) | Same as today. |

## 3. Change catalogue by pain point

"Output change" means the change can alter what `/route` or `roundtrip` returns, which needs a FINGERPRINT bump in the orchestrator plus a Gate v2 re-run (`extension-api.md`, FINGERPRINT rule). "Pick" is a dry `git merge-tree --merge-base <c>^ curvature-costing <c>`: **clean** means it applies alone with no conflict. **dep** means it conflicts only because earlier upstream commits are missing, so take it with a rebase.

### 3.1 Serving path: latency, routing correctness (thor / sif / loki / service)

| Change | PR, merged | Pain point | Expected benefit | Conflict with fork | Output change | Pick |
|---|---|---|---|---|---|---|
| Destination-only zones via `<access>:conditional=destination @ …`; `Allowed()` loses its `is_dest` parameter and gains `bool* edge_destonly`; `TransitionCost*` gain `edge_destonly`; Dijkstra/A\* labels carry it | [#6364](https://github.com/valhalla/valhalla/pull/6364), 2026-10-06 (3.9.1) | route correctness (bidir A\* used to fail on chains of dest-only edges) | Fewer failed or odd routes near dest-only zones. No perf claim. | **High.** It conflicts textually in `motorcyclecost.cc::TransitionCost`, and it is a **semantic compile break** at `route_action.cc:2677` (`cost->Allowed(de, false, *pred, …)`, fork code in the rejoin/walk pass), which `merge-tree` does not flag. The fork's `RoundTripExpansion` inherits `Dijkstras::ExpandInner` and picks up the new label semantics silently. | **Yes** (rare; only where such tags exist) | dep |
| `EvaluateRestrictions` no longer returns early on timed restrictions | [#6157](https://github.com/valhalla/valhalla/pull/6157), 2026-10-09 (master only) | correctness | Restrictions after a timed one are now evaluated. | `dynamiccost.h`, mechanical | **Yes** (rare) | dep |
| `use_distance` for motorcycle | [#6214](https://github.com/valhalla/valhalla/pull/6214), 2026-08-19 (3.9.0) | API (time-vs-distance) | A new knob. It does not help loops (it trades curvy time for short distance), but it is usable for a "direct" one-way mode. The heuristic stays admissible (`1/16` s/m per metre ≥ `kSpeedFactor[top_speed]`). | **Certain** in `motorcyclecost.cc` (the `EdgeCost` return line, both `TransitionCost*` tails, adjacent to the fork's continuity block) | No at default `use_distance=0` | dep |
| `valhalla_service` zmq `inproc://` default | [#6105](https://github.com/valhalla/valhalla/pull/6105), 2026-09-17 (3.9.0) | latency (stage hand-off) | The source comment says "switch to shared mem its faster than sockets", with no numbers. Each request passes the `Api` protobuf through loki→thor→odin, so a K=12 round-trip response is the largest message we move. Expect a sub-millisecond to low-millisecond saving. Unmeasured. | none (fork does not touch `valhalla_service.cc`) | No | dep (build changes) |
| `linear_cost_factors` resolved in **loki** instead of thor; `allow_types` (`"all"`); a failed edge walk **warns** instead of failing the request; location defaults for the edge walk; zero factor no longer drops later features; empty-shape check | [#6371](https://github.com/valhalla/valhalla/pull/6371) 2026-10-02, [#6350](https://github.com/valhalla/valhalla/pull/6350) 2026-10-13, [#6443](https://github.com/valhalla/valhalla/pull/6443) 2026-10-27, [#6327](https://github.com/valhalla/valhalla/pull/6327)/[#6328](https://github.com/valhalla/valhalla/pull/6328)/[#6329](https://github.com/valhalla/valhalla/pull/6329) 2026-09-16 | **variety** (Shuffle), admissibility probe | See §3.4. This hardens an API we already have (3.8.2) and that already reaches `roundtrip_impl`. | `thor/worker.{h,cc}` (fork's v4 knob block sits where upstream deletes `min_linear_cost_factor`/`max_linear_cost_edges`), `route_action.cc` (upstream deletes `add_cost_factor_edges`, which the fork's `route()` calls before `roundtrip_impl`). `test_roundtrip_audit.cc:1114-1250` (the decoy-factor A\* admissibility probe) asserts `cost_factor_edges_size() > 0`; re-verify it once resolution moves to loki. | No (unless a request sends features) | #6328 **clean**; rest dep |
| loki: don't advance the bounding-circle iterator past the end | [#6241](https://github.com/valhalla/valhalla/pull/6241), 2026-07-27 (3.9.0) | correctness (UB) | Our prod tiles have **no** bounding circles (`build_bounding_circles` absent; R3 audit). In 3.8.2, `bin_handler_t` and `edges_in_bounds` increment an iterator over an empty span for every candidate edge on every correlation. The increment is undefined behaviour but has been harmless in practice. | none | No | **clean** |
| loki: `search_.clear()` between repeated searches in one request | [#6231](https://github.com/valhalla/valhalla/pull/6231), 2026-07-23 (3.8.3) | correctness | Affects `exclude_locations`, `/locate` with two modes and `auto_pedestrian`. The orchestrator sends none of these today. | none | No (for our requests) | **clean** |
| Preserve embedded elevation across mid-edge `through` locations | [#6203](https://github.com/valhalla/valhalla/pull/6203), 2026-07-15 (3.8.3) | elevation | Not used: we get profiles from `/height` (skadi DEM), not from route `elevation`. | none | No | clean |
| Error codes from matrix/isochrone/**trace** (446 instead of 171) | [#6416](https://github.com/valhalla/valhalla/pull/6416), 2026-10-28 (master) | API | `/navigate` uses `trace_route`. Check the orchestrator's error mapping before taking it. | none | No (error bodies only) | clean |
| CostMatrix work (`dijkstra_distance`, filtered edges, recost distance, hierarchy limits for `/optimized_route`) | [#6257](https://github.com/valhalla/valhalla/pull/6257), [#6236](https://github.com/valhalla/valhalla/pull/6236), [#6258](https://github.com/valhalla/valhalla/pull/6258), [#6445](https://github.com/valhalla/valhalla/pull/6445) | — | Off our path (we call no matrix endpoint). #6257 is a design signal for a return-leg heuristic (Dijkstra inside a radius), as already noted in the 2026-09-05 survey. | — | — | — |
| `expansion_index` in `/expansion` | [#6251](https://github.com/valhalla/valhalla/pull/6251), 2026-08-03 | debugging | The order in which edges settled. Useful for visualising stock `/route` A\*. `roundtrip_impl` does not go through `/expansion`. | none | No | clean |

**Negative result, stated explicitly.** Our round-trip latency is A\* runs, harvest and pair evaluation, and upstream ships no speedup for any of them. The thor changes in the window are correctness changes (#6364, #6157) or API changes (#6350). `alternates.cc` is unchanged, and `validate_alternate_by_local_optimality` is still `// [TODO] NOT IMPLEMENTED`, so one-way alternates quality gets nothing either.

### 3.2 Tiles and memory (needs a tile rebuild unless noted)

**Compatibility first.** `GraphTileHeader`, `DirectedEdge` and `NodeInfo` layouts are unchanged in the window (#6232 only adds default member initialisers). The new `Use` values `kCycleStreet = 12` and `kLadder = 36` occupy previously unused enum slots. **A rebased engine can serve the existing Balkans tiles.** The engine and the tiles can therefore move, and be measured, separately.

| Change | PR, merged | Pain point | Expected benefit | Conflict | Output change |
|---|---|---|---|---|---|
| **Hilbert-curve node ordering on all three levels** (it replaces the old level-2-only `grid_divisions_within_tile` grid sort; that config key is gone and Hilbert is unconditional) | [#6342](https://github.com/valhalla/valhalla/pull/6342), 2026-09-27 (3.9.1) | memory (page working set), cold latency | Published numbers cover locality only. France extract, mean bounding-box diagonal of the 128 `NodeInfo` sharing one 4 KiB page: **7 546 m → 3 303 m** (all levels), **34 236 m → 12 359 m** (level 0); p50 distance between consecutive nodes **120 m → 42 m**. **No latency benchmark.** Maintainers expect "p99 latency to go down a bit" under cache pressure; another reviewer saw "a significant difference on routes on small spatial extracts but not on large ones". We serve a ~364-tile extract from an mmap'd tar on a RAM-constrained box, which is the regime where it should matter. **Must be measured** (RSS and cold/warm p50/p95 A/B). | `graphbuilder.cc`, `hierarchybuilder.cc` (dep only, the fork does not touch them) | **Yes.** GraphIds are renumbered, and A\* tie-breaking can differ. Nothing in the orchestrator stores GraphIds (`exclude_turnarounds` is `LatLng`). |
| Drop unused encoded elevation on shortcuts and bridge/tunnel/ferry edges | [#6340](https://github.com/valhalla/valhalla/pull/6340), 2026-09-27 (3.9.1) | tile size, memory | Planet: 510.5 MiB (shortcuts) + ~39 MiB (ferry/bridge/tunnel) of an 80 GiB graph, **≈0.7 %**. Small but free. It does not touch `weighted_grade` (our `prefer_elevation` input) or `/height`. | dep (`test_elevation.cc`) | No for loops (only route `elevation` output, which we don't request) |
| Shortcut EdgeInfo deduplication by shape; no loop shortcuts | [#6008](https://github.com/valhalla/valhalla/pull/6008), 2026-09-16 (3.9.0) | **quality** (geometry), shortcut recovery | Two shortcuts sharing end nodes could share one EdgeInfo even with different shapes (≈70 "Duplicate shortcut" log lines on a Netherlands build, zero after the fix). A wrong shortcut shape feeds `RecoverShortcut` and served geometry, and the v4 geometry gate and road-identity index read served geometry. | `shortcutbuilder.cc` (dep) | **Yes** (rare) |
| `DirectedEdge::set_length` rounding/non-zero; graph filter must not create zero-length edges; shortcut elevation sentinel −500 | [#6319](https://github.com/valhalla/valhalla/pull/6319), [#6318](https://github.com/valhalla/valhalla/pull/6318), [#6306](https://github.com/valhalla/valhalla/pull/6306) (3.9.0) | correctness | Shortcut speed no longer mis-rounded (#6319: "the speed was not correct"). | `directededgebuilder.cc` (dep) | **Yes** (small) |
| More `surface=*` values parsed | [#6337](https://github.com/valhalla/valhalla/pull/6337), 2026-10-06 (master) | quality | >140 k extra tags world-wide get a real surface. The motorcycle `surface_factor_` then applies to them. | dep (`pbfgraphparser.cc`) | **Yes** |
| `Use::kCycleStreet` for `cyclestreet`/`bicycle_road` | [#6407](https://github.com/valhalla/valhalla/pull/6407), 2026-10-16 (master) | — | Data only. For motorcycle such edges fall to the default-use branch. Negligible in Serbia. | clean | Marginal |
| `GraphReader::GetGraphTileHeader` (header-only read) | [#6200](https://github.com/valhalla/valhalla/pull/6200), 2026-07-12 (3.8.3) | memory (tooling) | Fixes a Python helper loading the whole tileset into RAM. It does not affect `valhalla_service`. | none | No |

No upstream change touches the tile cache (`GraphReader` cache classes, `max_cache_size`, the LRU) or per-request label reservation (`max_reserved_labels_count_*`, `clear_reserved_memory`). The 3.8.2-era levers in `2026-07-15-config-tile-locality-levers.md` are unchanged.

### 3.3 Tile build time

These matter less for a 364-tile extract than for the planet runs the PRs measured, but they come with any rebuild on the new mjolnir:

| Change | PR | Published benefit (planet, 16-CPU EC2) |
|---|---|---|
| PSRS parallel external sort (+ cap to 8 threads) | [#6226](https://github.com/valhalla/valhalla/pull/6226) 2026-07-22, hotfix [#6316](https://github.com/valhalla/valhalla/pull/6316) | `way_nodes.bin` sort ~747 s → ~252 s ("up to 3×") |
| 8-byte `edge_shapes.bin` instead of reading 40-byte `OSMWayNode` | [#6234](https://github.com/valhalla/valhalla/pull/6234) 2026-09-02 | `graphbuilder::Build` 5 506 s → 3 452 s; `build_tile_set` 16 846 s → 14 432 s (−14 %) |
| No `std::regex` in `GetTagTokens` / conditional time-range parsing | [#6227](https://github.com/valhalla/valhalla/pull/6227) 2026-08-06, [#6230](https://github.com/valhalla/valhalla/pull/6230) 2026-09-16 | regex was "almost 10 % of CPU time of `ParseWays`" |
| Faster `minimum_bounding_circle` | [#6218](https://github.com/valhalla/valhalla/pull/6218) 2026-07-16 | 53 % of "validate" CPU on Germany, **only when building bounding circles** (we don't) |
| *(open)* Multithreaded `GraphFilter` | [#6378](https://github.com/valhalla/valhalla/pull/6378) | `FilterTiles` 1 420 s → 91 s; applies only when `include_pedestrian`/`include_bicycle` are off |
| *(open)* Offset read/write external sort | [#6320](https://github.com/valhalla/valhalla/pull/6320) | −34…−38 % sort time on HDD and laptop; neutral on NVMe |

Conflict note: the fork's mjolnir patch (`src/mjolnir/util.cc`, the sampling-invariant `compute_curvature`) conflicts with upstream only on an `#include` line. #6230 conflicts on `valhalla/mjolnir/util.h` (dep).

### 3.4 APIs we could use for quality and variety

- **`linear_cost_factors` is a ready-made "penalise these roads" lever for Shuffle variety, and it is already in our base.** In 3.8.2, `thor_worker_t::route()` resolves `cost_factor_lines` with `add_cost_factor_edges` **before** `parse_costing`, then branches into `roundtrip_impl`. So a factor attached to the shapes of loops a rider already saw raises their cost on **every** search the round-trip runs: harvest Dijkstra, return-leg A\* and pair pass. That complements fork-side `exclude_turnarounds` (curvagen-valhalla#28). It would let the orchestrator soft-penalise served roads, where `exclude_turnarounds` drops served turnarounds. Factors ≥ 1 keep A\* admissible. `service_limits.min_linear_cost_factor` defaults to 1.0, and every resolved factor is clamped up to it (`std::max(line.cost_factor(), min_allowed_factor)`), so by default a factor below 1 cannot be sent. Upstream's window makes this lever **safe to depend on**:
  - [#6443](https://github.com/valhalla/valhalla/pull/6443): a shape that fails to edge-walk now **warns instead of failing the request**. On 3.8.2 one stale served-loop shape (e.g. after a tile rebuild) would 4xx the whole Shuffle.
  - [#6327](https://github.com/valhalla/valhalla/pull/6327): edge walking uses trace-style location defaults (radius 10 m, no reachability test).
  - [#6329](https://github.com/valhalla/valhalla/pull/6329): before this fix, a zero factor discarded the factors of every later feature.
  - [#6328](https://github.com/valhalla/valhalla/pull/6328): empty coordinates are rejected with error 173 instead of misbehaving.
  - Shortcut handling (`add_shortcut`) already exists in 3.8.2, so penalised base edges also tax their shortcuts.
  - Cost: one edge walk per feature per request, capped by `service_limits.max_linear_cost_edges` (default 50 000). A 300 km loop is a few thousand edges. This is unmeasured; measure it before adopting.
- **`use_curvature` (open [#6449](https://github.com/valhalla/valhalla/pull/6449))**, see §5 "Watch". It matters for the quality model and for heuristic admissibility.
- **`hierarchy_limits`, `use_living_streets`, `use_lit`, `shortest`, speed types, closures**: no changes in the window apart from #6445 (matrix only). The costing-options surface grew by `use_distance` for motorcycle (#6214), `steps_factor` / `exclude_steps` (bicycle, #6394) and `allow_types` (#6350).

## 4. Conflict surface for a full merge

`git merge-tree --write-tree curvature-costing upstream/master` reports **6 conflicted files**, all small:

| File | Hunks | What collides | Resolution |
|---|---|---|---|
| `proto/descriptors/options.proto` | 1 | upstream `steps_factor = 99` vs fork `curviness_continuity = 99` (+ fork 98/100/101 in the same block) | **Renumber the fork fields first** (§6 step 0). A protobuf message cannot hold two fields with number 99. |
| `src/sif/motorcyclecost.cc` | 2 | `TransitionCost` signature (#6364 adds `edge_destonly`); `c.cost *= inv_distance_factor_` (#6214) next to the fork's continuity block | Keep both; apply `inv_distance_factor_` **after** the continuity penalty, as upstream does for its own transition terms. |
| `src/thor/route_action.cc` | 1 | `#include` block (#6371 drops `route_matcher.h` together with `add_cost_factor_edges`, its only user; fork adds `road_twin_index.h`, `roundtrip_*.h`) | Keep the fork includes and drop `route_matcher.h`. |
| `src/thor/worker.cc`, `valhalla/thor/worker.h` | 1 + 1 | #6371 deletes `min_linear_cost_factor`/`max_linear_cost_edges`; the fork's ~50 `roundtrip_*` config members sit right there | Keep the fork block, drop the two members, and remove the `add_cost_factor_edges` call in `route()` (loki does it now). |
| `src/mjolnir/util.cc` | 1 | include line | Keep both. |

**Semantic breaks that `merge-tree` does not show:**

- `route_action.cc:2677`: `cost->Allowed(de, false, *pred, tile, e, 0, 0, ridx, mask)` no longer compiles against #6364's signature `Allowed(edge, pred, tile, edgeid, time, tz, ridx, mask, bool* = nullptr)`. Drop the `false`. It fails loudly at compile time, not silently.
- `RoundTripExpansion : public Dijkstras` inherits #6364's new destination-only propagation (`edge_destonly` on labels, `pred_destonly` in `EvaluateRestrictions`). This compiles unchanged, but harvest and pair output can shift near destination-only zones, so Gate v2 must cover it.
- `test_roundtrip_audit.cc` (decoy `linear_cost_factors` admissibility probe) depends on thor-side resolution. Re-check it after #6371.
- If #6449 lands: upstream adds `valhalla::sif::kCurvatureFactor` in `dynamiccost.h`, and the fork has `kCurvatureFactor` in `motorcyclecost.cc`'s anonymous namespace inside `valhalla::sif`, so unqualified uses become **ambiguous**. Rename the fork table (e.g. `kForkCurvatureDiscount`). #6449 also adds `use_curvature = 100`.

Unchanged and therefore not hazards: `GetBestPath` signatures, the `Dijkstras` virtual interface (`ExpandingNode`, `ShouldExpand`, `GetExpansionHints` that the fork overrides), `BDEdgeLabel`, tile layout, and the C++ standard (still 20).

Against tag **3.9.1** instead of `master`, the textual set is 5 files: `options.proto` does not conflict yet, because #6394 is post-3.9.1. The semantic breaks are the same (#6364 is in 3.9.1). #6157 and the linear-cost #6350/#6443 are master-only.

## 5. Ranked shortlist

### Take now (fork-only or clean cherry-pick, no route-output change)

| # | Item | Why now | Risk | Output change |
|---|---|---|---|---|
| 1 | **Relocate fork proto fields** off upstream's next numbers: `Costing.Options` 98 `reuse_penalty`, 99 `curviness_continuity`, 100 `prefer_elevation`, 101 `prefer_curvature`, and `Options.roundtrip = 67`. Move them to a private block (e.g. 900–903 and 900; anything < 19000 and outside upstream's dense range). Add `reserved` comments so they don't drift. | Upstream already took 99 ([#6394](https://github.com/valhalla/valhalla/pull/6394)), and [#6449](https://github.com/valhalla/valhalla/pull/6449) takes 100. Every rebase until then is a forced renumber, as already happened once (`a2fd86343`, 97→101). JSON keys are unchanged and the orchestrator speaks JSON. | Low. Re-run the gurka suites; `tools/loopqual` decodes JSON. | **No** (no FINGERPRINT) |
| 2 | [#6241](https://github.com/valhalla/valhalla/pull/6241) bounding-circle iterator (`6f905d274`) | Removes UB from every loki candidate scan on our non-bounding-circle tiles. Clean pick, one file. | Very low | No |
| 3 | [#6328](https://github.com/valhalla/valhalla/pull/6328) empty-shape check for `linear_cost_factors` (`92e89e1b5`) | Clean pick. Hardens the API our audit test and a possible Shuffle penalty use. | Very low | No |
| 4 | *(optional)* [#6231](https://github.com/valhalla/valhalla/pull/6231) loki `search_.clear()` (`04f060dcb`) | Clean. Matters only if we start sending `exclude_locations`. | Very low | No |

This list is thin on purpose. Nothing that changes serving speed or loop quality can be cherry-picked in isolation. Every such item either has upstream dependencies (#6364, #6371, #6214) or needs a tile rebuild (#6342, #6008, #6340).

### Take with the next rebase (→ FINGERPRINT bump + Gate v2)

| # | Item | Expected benefit | Risk |
|---|---|---|---|
| 1 | **The rebase itself** onto 3.9.1, now unblocked by [#6303](https://github.com/valhalla/valhalla/pull/6303) | Gets us back on a supported line. Every later upstream fix becomes a cheap pick instead of a dep chain. | Moderate. Six small conflicts + one semantic compile break (§4). |
| 2 | [#6364](https://github.com/valhalla/valhalla/pull/6364) + [#6157](https://github.com/valhalla/valhalla/pull/6157) restriction correctness | Fewer failed or illegal routes near conditional destination-only zones and multiple restrictions. This affects every search the round-trip runs. | Output change (rare). It is the only upstream change that reaches `RoundTripExpansion`. |
| 3 | `linear_cost_factors` hardening ([#6371](https://github.com/valhalla/valhalla/pull/6371), [#6443](https://github.com/valhalla/valhalla/pull/6443), [#6327](https://github.com/valhalla/valhalla/pull/6327), [#6329](https://github.com/valhalla/valhalla/pull/6329), [#6350](https://github.com/valhalla/valhalla/pull/6350)) | Makes "penalise the roads this rider was already served" a safe orchestrator-side variety lever (§3.4). Prototype it against `exclude_turnarounds` with Gate v2 T2 (Served-Surface Distinctness). | Low for the engine. The edge-walk cost per request is unmeasured. |
| 4 | `inproc://` zmq default ([#6105](https://github.com/valhalla/valhalla/pull/6105)) | A small per-request hand-off saving. Unmeasured. | Low. Watch prime_server 0.12.0 for RSS changes. |
| 5 | **Tile rebuild on the new mjolnir, as a separate cutover**: [#6342](https://github.com/valhalla/valhalla/pull/6342) Hilbert, [#6340](https://github.com/valhalla/valhalla/pull/6340) elevation drop, [#6008](https://github.com/valhalla/valhalla/pull/6008) shortcut dedup, [#6319](https://github.com/valhalla/valhalla/pull/6319)/[#6318](https://github.com/valhalla/valhalla/pull/6318)/[#6306](https://github.com/valhalla/valhalla/pull/6306), [#6337](https://github.com/valhalla/valhalla/pull/6337) surfaces, build speedups | Page locality is the only upstream lever on our memory and cold-latency pressure. Expect ≈0.7 % smaller tiles and correct shortcut geometry. Locality is published; latency and RSS deltas are not. | Output change (GraphIds renumbered, surfaces, shortcuts). Measure with **rig tiles** (the 2026-09-07 discovery: prod's 364-tile Balkans set vs the rig's 211-tile Serbia set, so a mixed comparison measures tiles, not the engine). |
| 6 | [#6214](https://github.com/valhalla/valhalla/pull/6214) `use_distance` | Comes with the rebase (forced conflict). A no-op at default. A possible "direct" one-way mode knob later. | None at default |

### Watch (open upstream work)

| # | Item | Why it matters to us | What to do |
|---|---|---|---|
| 1 | [#6449](https://github.com/valhalla/valhalla/pull/6449) `use_curvature` for `MotorcycleCost` (open; opened 2026-10-30, maintainer-authored, one "lgtm") | This is upstream's own curvy-motorcycle option. It **penalises straight roads** (factor `1 + 6·s·straightness²` ∈ [1, 7], 21 quantised steps, `use_curvature` 0.5 = neutral) instead of discounting curvy ones. That **keeps the A\* heuristic admissible**, which our negative `kCurvatureFactor` breaks: factor as low as 0.523 at c0.5 and 0.343 at c0.8, per `test_roundtrip_audit.cc:1114-1116`. It is the 2026-09-05 survey's items 1 and 5, done upstream. It also weights by edge density (rural full, suburban ×0.1, urban flat ×2 penalty), skips links, roundabouts and internal edges, and reads `DirectedEdge::curvature()`, which **our mjolnir redefines** (turning density, 350°/km calibration). Reviewer concern: much larger cost ranges stress the bucket queue, and the heuristic gets looser, so expect more expansion. | Do not take blindly: field 100 and `kCurvatureFactor` collide. Prototype its **inverted-avoidance shape** in our `kCurvatureFactor` (factor ≥ 1), and Gate v2 + latency it. If upstream merges, consider mapping `prefer_curvature` onto `use_curvature` to shrink the fork's sif patch. |
| 2 | [#6425](https://github.com/valhalla/valhalla/pull/6425) better edge/node density (open, 2026-10-15) | Motorcycle `EdgeCost` uses `kDensityFactor[edge->density()]`, and transitions use `kTransDensityFactor[node->density()]`. A better rural/suburban/urban split goes to the "urban-grid wandering" the fork fights with a residential demotion (ADR-0032). #6449 depends on it. Kinkard (2026-11-04): "heuristics doesn't guide expansion on 2nd level at all", a data point for our return-leg A\* cost. | Tile content and output change. Fold it into the tile-rebuild cutover if it merges. |
| 3 | [#6436](https://github.com/valhalla/valhalla/pull/6436) interpolate node elevation inside tunnels/bridges/ferries (open, changes requested) | Fixes bogus `weighted_grade`/`max_up_slope` on multi-way structures, and `prefer_elevation` rewards `weighted_grade`. | Tile rebuild. Watch. |
| 4 | Branch `cb-exclude-edges-in-search` (successor to closed [#6229](https://github.com/valhalla/valhalla/pull/6229), see the [maintainer comment](https://github.com/valhalla/valhalla/pull/6229) of 2026-09-15) | Makes user-excluded edges visible to loki's per-edge reachability. That pattern matters when the Second Via re-correlates after excluding a corridor. #6229 itself is **closed unmerged**, so the 2026-09-05 survey's "cherry-pickable now" note no longer holds. | Watch for a PR. |
| 5 | [#6456](https://github.com/valhalla/valhalla/pull/6456) `allow_all` in loki search; [#6457](https://github.com/valhalla/valhalla/pull/6457) `exclude_restricted` search filter | Correlation-side companions to #6350. Low relevance. | — |
| 6 | [#6309](https://github.com/valhalla/valhalla/pull/6309) iterative route-matcher edge walk | `/navigate` sends whole loops to `trace_route` with `shape_match: walk_or_snap`. The recursive `expand_from_node` uses stack proportional to matched edges, which is safe on 8 MiB service threads but not unbounded. | Watch. |
| 7 | [#5983](https://github.com/valhalla/valhalla/pull/5983) reverse speed limit in `EdgeInfo` | Tile content, so FINGERPRINT territory. | Watch (open since 2026-03). |
| 8 | [#6378](https://github.com/valhalla/valhalla/pull/6378), [#6320](https://github.com/valhalla/valhalla/pull/6320), [#6338](https://github.com/valhalla/valhalla/pull/6338) build-time I/O | Build time only. | — |

## 6. Rebase recommendation

**When:** now. The #6210 hazard is gone (#6303, 3.9.0), and the fork's distance from upstream (100 commits) only grows. **Target:** tag **3.9.1** (the current "Latest" release), not `master`. A release tag is a reproducible base. It already contains #6364, #6303, #6342 and #6340, and it avoids the `options.proto` collision until we have renumbered. Pick master-only fixes (#6157, #6443, #6350, #6337) individually afterwards; they pick cleanly once the base is 3.9.1.

**How**, keeping prod's engine and tiles separable:

0. **Fork-only commit on `curvature-costing`:** relocate the proto fields (§5 take-now #1). Take-now #2 and #3 can ride along. No output change, no FINGERPRINT.
1. **Rebase in a worktree** (other agents use the main checkout): `git worktree add … curvature-costing`, `git config rerere.enabled true`, `git rebase --onto 3.9.1 3.8.2`. The fork is 70 commits; `route_action.cc` and `motorcyclecost.cc` hunks will recur, so rerere pays off. Resolve per §4, including the `Allowed()` call at `route_action.cc:2677` and moving `add_cost_factor_edges` out of `route()`. `git submodule update --init --recursive` (flatbush).
2. **Build** `Dockerfile.local` with `ENABLE_TESTS=On` and run the gurka round-trip suites (`test_motorcycle_*`, `test_roundtrip_*`, including the `test_roundtrip_audit` admissibility probe).
3. **Engine-only A/B on the rig, on the current tiles** (layout-stable): Gate v2 15/15, latency p50/p95 at the prod regime (`server_threads=2`, 2 CPUs), and RSS. Expect small route diffs from #6364. **Bump `FINGERPRINT_VERSION`** in the orchestrator at cutover. Keep the previous `:amd64-<sha>` as the warm-rollback target.
4. **Then, separately, a tile rebuild** on the rebased mjolnir (Hilbert, elevation drop, shortcut dedup, surfaces). A/B **with rig tiles on both sides** for RSS and cold/warm latency, Gate v2 again, and a second FINGERPRINT bump. This is the step that tests the only upstream lever on memory.
5. Before the *next* rebase, re-check #6449 (field 100, `kCurvatureFactor`) and #6425.

Risk assessment: **moderate, and lower than the 3.7→3.8.2 rebase**. Every conflict is mechanical, and the one semantic break fails at compile time. The fork's large new files (`roundtrip_pairs.cc`, `road_twin_index.cc`, `roundtrip_expansion.cc`) have no upstream counterpart and do not conflict.

## Sources

- Upstream repository: `github.com/valhalla/valhalla`. Local `git fetch upstream` on 2026-11-04; `git log 17af0d031..upstream/master`; `git diff` per path; `git merge-tree --write-tree` for conflict checks.
- `CHANGELOG.md` at upstream `master` (`UNRELEASED`, 3.9.1, 3.9.0, 3.8.3 sections).
- Releases: `gh release list -R valhalla/valhalla` (3.8.3 2026-07-25, 3.9.0 2026-09-19, 3.9.1 2026-10-06).
- PR bodies, comments and diffs (via `gh pr view` / `gh pr diff -R valhalla/valhalla`): #6008, #6105, #6156, #6157, #6200, #6203, #6210, #6214, #6218, #6226, #6227, #6229, #6230, #6231, #6232, #6234, #6241, #6251, #6257, #6260, #6303, #6306, #6309, #6316, #6318, #6319, #6320, #6328, #6329, #6337, #6338, #6340, #6342, #6350, #6363, #6364, #6371, #6378, #6394, #6407, #6416, #6425, #6436, #6443, #6445, #6449, #6456, #6457, #5491, #5983.
- Fork context: `docs/curvagen/research/2026-07-15-valhalla-3.7-to-3.8.2-delta.md`, `2026-07-15-config-tile-locality-levers.md`, `2026-07-16-r3-valhalla-config-surface-audit.md`, `2026-09-05-loop-algorithms-delta-survey.md` (§6 rebase hazards, §7 shortlist), `docs/curvagen/extension-api.md` (FINGERPRINT rule); fork source at `5b29936f0` (`src/thor/route_action.cc`, `src/sif/motorcyclecost.cc`, `valhalla/thor/roundtrip_expansion.h`, `test/gurka/test_roundtrip_audit.cc`).
- Orchestrator request surface: `curvagen-orchestrator/crates/domain/src/costing.rs`, `crates/engine-client/src/lib.rs`.

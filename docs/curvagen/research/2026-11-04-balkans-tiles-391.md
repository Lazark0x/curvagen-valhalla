# Balkans tiles on the 3.9.1 fork: a fresh build measured against the tiles prod serves

- **Purpose:** produce the production tileset for the combined cutover (engine rebase to upstream 3.9.1, draft PR #31; the curvature-costing change "(b2)"; new tiles; one orchestrator `FINGERPRINT_VERSION` bump), and measure the new tiles against the ones production serves today with everything else held fixed.
- **Scope:** build and measure. No deploy, no change on the box, no change to the engine or the orchestrator.
- **Engine held fixed for every A/B row below:** `rebase/3.9.1` @ `9c379e75c`, image `curvagen-valhalla:rebase-391-local`, production `valhalla.json` verbatim (xcand 0.2 / cap 4 and the v4 defaults in `worker.cc`), `--cpus 2`, `server_threads` 2, tar mmap from a read-only bind. Only the tar changes.

## Summary

- **Built:** a fresh Balkans tar (Geofabrik serbia + bosnia-herzegovina + montenegro, OSM data as of **2026-11-03T21:24Z**) with the `rebase/3.9.1` fork's mjolnir. It has the same 364 tile ids as production's tar, elevation in the same 361 tiles, 370 186 240 bytes (+0.9 %), sha256 `ba2b273b70abe8b762dc0bb19f6d37d2ec46b75281c5b977bec220e2af93d7f9`. The build took about 6 min on a shared 4-vCPU Docker VM.
- **Quality, with the engine held at 3.9.1 and only the tar changed:** every Gate v2 reject bar passes on both tars, and the other meters move a little, in both directions. Served Distinctness improves (mean 0.343 → 0.319, near-duplicates 14.8 % → 10.5 % on the demand cells), and curviness retention runs 1.009–1.018×. Distance error moves within ±0.02. Two meters get slightly worse: served retraces 2.0 → 2.7 % (bar 5 %) and near-mirror unseen metres 36 → 41 m. There are 0 failed requests on either tar. Fallbacks are unchanged: none on the demand cells or the Balkans corpus, and 174 → 174 served loops on all of corpus-v2.
- **Shuffles:** 10.0 → 9.6 % repeats, 100 % fill, and distance-error p90 28.5 → 28.4 %, all under the owner's 30 % budget. Zlatibor sits at the edge on both tars (0.30 → 0.32). Mostar (0.58) and Sarajevo (0.40) are over budget on both. That is a property of the area, not of these tiles.
- **Latency and memory:** neutral. Paired K = 3 p50 0.142 → 0.140 s, p90 0.565 → 0.572 s, paired median ratio 0.995, total engine CPU 0.968×. Cold page cache costs the same on both tars. File-backed memory grows +3 MiB, and peak anonymous memory does not grow.
- **Loop churn:** about a third of served loops are different loops on the new tar (67.8 % of served loops on corpus-v2 and 78.7 % on the Balkans corpus still have a ≥ 0.9 twin in the old served set). **All of it comes from the data, none from the 3.9.1 build.** A control tar built from the same new OSM with the current 3.8.2-fork mjolnir serves byte-identical loops to the 3.9.1 tar for 3 945 of 3 948 served loops.
- **Bounding circles:** ship **without** them. They do not change output (byte-identical on replay) and save 0.4 ms per `/locate`, which is about 0.3 % of a round trip. They cost +7.9 MB of tar, and production has never had them.
- **Recommendation:** ship these tiles in the combined cutover. The planned single `FINGERPRINT_VERSION` bump covers them, and they need one anyway, because a third of the served loops change.

## 1. What production serves today

Read from a byte copy of the box's `/opt/curvagen/data/valhalla_tiles.tar` (sha256 `c0fe81e0aeaef1408943d8276a776f47567c190abbd388c66ee98759f8427fce`, 366 909 440 bytes, tar entries dated 2026-07-14):

| | prod tar |
|---|---|
| tiles | 364 (level 0: 6, level 1: 31, level 2: 327) |
| level-2 extent | lat 41.75–46.25, lon 15.50–23.25 |
| tile `version` stamp | `3.7.0` on all 364 |
| elevation | 361 / 364 tiles |
| bounding circles | none |
| directed edges | 3 692 634 |

The tile map is Serbia, Bosnia and Herzegovina and Montenegro: Kosovo's interior tiles are absent (Geofabrik ships Kosovo as its own extract) and Croatia only appears as border spill north of Bosnia. That matches the recipe recorded at the 2026-07-14 tileset change: `osmium merge` of the Geofabrik `serbia`, `bosnia-herzegovina` and `montenegro` extracts, built with the then-current fork image. The fork's curvature metric (`1ef986efd`, `5645ad8cb`) was authored 2026-06-13, so the July tiles already carry it.

Loading this copy on the 3.9.1 engine reproduces the box's round-trip sidecar line exactly (364 tiles, 1 583 271 segments, twins 134 171, parallels 306 977, retained 13 MiB), so the baseline arm below is the tileset production serves.

## 2. The new build

**Input.** Geofabrik `europe/serbia`, `europe/bosnia-herzegovina`, `europe/montenegro` `-latest.osm.pbf`, downloaded 2026-11-04 with their `.md5` files verified. All three carry `osmosis_replication_timestamp=2026-11-03T21:24:23Z`. `osmium merge` (1.19.1) gives one 437 841 542-byte PBF (56.9 M nodes, 6.07 M ways, 68 k relations; sha256 `43cd9849…6b32`). Same three regions as July, so the coverage question answers itself; it was checked anyway (below).

**Recipe** (the image entrypoint's stages, run explicitly with a stage timer; config = production `valhalla.json` with only paths and `mjolnir.concurrency` 4 changed):

```
valhalla_build_admins      --config cfg balkans.pbf
valhalla_build_timezones   > timezones.sqlite            # timezone-boundary-builder 2026d
valhalla_build_tiles       -c cfg -e build balkans.pbf
valhalla_build_elevation   --from-tiles --decompress -c cfg
valhalla_build_tiles       -c cfg -s enhance balkans.pbf  # enhance .. cleanup
valhalla_build_extract     -c cfg -v --overwrite
```

**Build time** on this laptop's Docker VM (4 vCPU, shared the whole time with another agent's engine benchmarks, so an upper bound): admins 19 s, timezones 8 s, graph build 176 s, elevation download 47 s (13 new SRTM tiles on top of the 19 the box already holds; 32 total), enhance through cleanup 102 s, tar 10 s: **≈ 6 min end to end**. The no-bounding-circle rebuild of the same input took 247 s without the admin/timezone/elevation steps.

**Coverage check.** The new tar has exactly the same 364 tile ids as production's (no tile only in one of them), the same elevation coverage (361 / 364), and every tile is stamped `3.9.1`. Directed edges grow 1.4 % (3 692 634 → 3 743 380); the round-trip sidecar grows 1.2 % (1 583 271 → 1 602 869 segments, twins 134 171 → 136 759, parallels 306 977 → 312 323). Build warnings are the usual (8 163 unparsable OSM time ranges, 17 edges with an elevation jump over the cap, one ferry that could not be reclassified); "possible duplicates" is 0 at every level.

| tar | bytes | sha256 |
|---|---|---|
| prod (July, 3.7.0 build) | 366 909 440 | `c0fe81e0…7fce` |
| **new, 3.9.1, no bounding circles — the candidate** | **370 186 240** (+0.9 %) | `ba2b273b70abe8b762dc0bb19f6d37d2ec46b75281c5b977bec220e2af93d7f9` |
| new, 3.9.1, bounding circles on | 378 101 760 (+3.1 %) | `3966a899…ca20` |
| control: same OSM, 3.8.2-fork mjolnir (`curvagen-valhalla:latest`) | 379 432 960 | `3259f4b7…817c` |

The candidate lives at `/Users/xenix/Projects/curvagen-tiles-391/out/valhalla_tiles.balkans-osm20261103-v391.tar` (a hard link of `build/nobc/valhalla_tiles.tar`) on the build laptop. It is not committed anywhere.

## 3. Bounding circles: off

The production config has `build_bounding_circles: true`, but the July tar has none: it was built by 3.7.0, which predates them. On this build the key works as documented, and the BC tar carries circles in 247 of 327 level-2 tiles.

- **Where they are read.** Unlike the 2026-07-16 config audit's reading ("only `/locate` + `/tile`"), `loki::Search` itself reads them (`src/loki/search.cc`, `handle_bin`), so `/route` correlation uses them too. They act as a conservative prefilter: an edge whose circle cannot beat the current best candidate is skipped before projection. The result cannot change, only the work spent finding it.
- **Identity, measured.** I replayed 18 saved K = 12 requests (every 6th of corpus-balkans-v1, 216 loops) against the BC tar. All were byte-identical to the no-BC responses.
- **Cost and gain, measured.** In a paired `/locate` (600 random points, alternating engines), the no-BC tar took 6.66 ms p50 and the BC tar 6.23 ms (median paired ratio 0.905). The 0.4 ms is about 0.3 % of a K = 3 round trip (p50 140 ms). The tar grows 7.9 MB (+2.1 %), all of it page cache on a 3.8 GB box.

**Decision: build without them.** The gain is below every noise floor this project measures at. Every A/B row in this doc was measured on the no-BC tar, and it keeps the shape production has served since July. The BC tar is a safe drop-in if that ever changes, because its output is identical.

## 4. A/B: production's tar against the new tar, engine fixed at 3.9.1

### 4.1 Gate v2 meters (loopqual metrics v2)

Read with `tools/loopqual/gate_v2.py`'s own `Run` and `rows_for`, with the **old tar as both baseline and bracket**. A tile A/B has to compare against the tiles it replaces. Baseline v2 is the census run on rig tiles, and comparing against it would measure the tileset, not this change.

Corpora:

- **corpus-v2** (552 requests at K = 12, Serbia; Gate's Served Surface = blocks A+B, slots 0–5).
- **corpus-balkans-v1**, new in this branch (`tools/loopqual/corpus-balkans-v1.yaml`): Sarajevo, Banja Luka, Mostar and Podgorica at city level, c0.5 / 0.7 / 1.0, 50 / 100 / 200 km, 3 seeds, motorways allowed; 108 requests at K = 12. Every origin is counted as served.

| meter | corpus-v2 A+B: prod tar → new tar | balkans-v1: prod tar → new tar |
|---|---|---|
| R1 retrace family, served (bar ≤ 5 %) | 2.0 % → 2.7 % | 0.0 % → 0.0 % |
| R2 seam stubs ≥ 500 m | 0 → 0 | 0 → 0 |
| R3 distance error mean / p90, c0.5 (bar ≤ 0.22 / 0.42) | 0.178 / 0.382 → 0.180 / 0.397 | 0.160 / 0.293 → 0.161 / 0.351 |
| R3 c0.7 (bar ≤ 0.32 / 0.65) | 0.144 / 0.239 → 0.149 / 0.220 | 0.179 / 0.432 → 0.179 / 0.432 |
| R3 c1.0 (bar ≤ 0.32 / 0.65) | 0.134 / 0.231 → 0.139 / 0.254 | 0.231 / 0.557 → 0.229 / 0.555 |
| R4 fills at K = 12 | 552/552 → 552/552 | 105/108 → 105/108 (the same three Mostar 100 km c1.0 asks return 7 loops on both tars) |
| R5 failed requests | 0 → 0 | 0 → 0 |
| T1 curviness (`curviness_geom_clean`) c0.5 | 297.2 → 299.8 (1.009×) | 389.5 → 395.0 (1.014×) |
| T1 c0.7 | 237.0 → 241.3 (1.018×) | 405.2 → 409.9 (1.012×) |
| T1 c1.0 | 255.4 → 259.0 (1.014×) | 417.2 → 416.5 (0.998×) |
| T2 Served Distinctness mean / near-dup > 0.6 | 0.3429 / 14.8 % → **0.3185 / 10.5 %** | 0.3568 / 14.4 % → 0.3417 / 13.1 % |
| T5 near-mirror unseen metres, served (mean) | 36 → 41 m | 38 → 43 m |
| T6 deep-bank retrace family | 2.6 % → 2.3 % | 7.4 % → 7.6 % (47 → 48 of 633 loops) |
| C1 spikes ≥ 30 m / C2 edge reuse (served) | 0 / 0 → 0 / 0 | 0 / 0 → 0 / 0 |
| A1 Bank Distinctness | 0.4506 / 25.4 % → 0.4187 / 20.1 % | 0.4845 / 30.1 % → 0.4748 / 30.2 % |
| A10 builder mix, served | pair 96 % / rescue 4 % → pair 97 % / rescue 3 % | pair 89 % / rescue 11 % → pair 90 % / rescue 10 % |
| A11 fallbacks (rung > 0) / gated / tier > 0, served | 0 / 0 / 0 → 0 / 0 / 0 (all of corpus-v2 incl. block C: 174 / 103 / 178 → 174 / 97 / 174 of 3 312) | 0 / 0 / 0 → 0 / 0 / 0 |

How to read the two rows gate_v2.py prints as FAIL:

- **T5** is pinned at 0.25× of *Baseline v2* (the v3-era census). Against a v4 baseline it cannot pass even against itself (36 m × 0.25 = 9 m), so it is not a verdict on the tiles. The +5 m move is real but small: a mean of unseen near-mirror metres per served loop.
- **T6** on the Balkans corpus is one deep-bank loop.

corpus-v2 block C (corpus-v1 verbatim, motorways avoided, advisory) moves 18.8 % → 19.0 % retrace family, served. c0.8 curviness runs 1.020×.

**Gate's T3/T4 latency rows are not used.** The corpus runs were not quiet, and the two arms' blocks A/B ran in different sessions (see §6). Latency comes from the paired run in §4.3.

### 4.2 Shuffle harness (K = 3, six consecutive Shuffles, `exclude_turnarounds` radius 0.08 × distance)

This is the `k12probe/exclude_bench.py` harness unchanged, with four non-Serbian cities added to its six Serbian origins: 10 origins × 30 / 60 / 120 km × c0.5 / 1.0, which gives 60 sessions and 360 serves per tar.

| | prod tar | new tar |
|---|---|---|
| repeats in Shuffles 2–6 | 90 / 900 = 10.0 % | 86 / 900 = 9.6 % |
| fill (3 loops per serve) | 360 / 360 | 360 / 360 |
| distance error median / **p90** | 12.7 % / **28.5 %** | 12.7 % / **28.4 %** |
| rescue / rung-2 fallback share | 8.7 % / 1.7 % | 7.4 % / 1.4 % |

Per city, distance-error p90 (budget < 30 %) and repeats:

| city | p90 prod → new | repeats prod → new |
|---|---|---|
| Belgrade (Vračar) | 0.23 → 0.22 | 2.2 → 5.6 % |
| Novi Sad | 0.22 → 0.20 | 11.1 → 5.6 % |
| Niš | 0.19 → 0.19 | 2.2 → 1.1 % |
| Kragujevac | 0.19 → 0.20 | 4.4 → 4.4 % |
| **Zlatibor** | **0.30 → 0.32** | 10.0 → 8.9 % |
| **Subotica** | 0.28 → 0.28 | 13.3 → 8.9 % |
| Sarajevo | 0.40 → 0.40 | 8.9 → 7.8 % |
| Banja Luka | 0.20 → 0.19 | 5.6 → 5.6 % |
| Mostar | 0.58 → 0.58 | 27.8 → 32.2 % |
| Podgorica | 0.28 → 0.26 | 14.4 → 15.6 % |

Subotica is inside the budget. Zlatibor is at the budget on both tars: border-clipped, with 18 % of its loops built by rescue or fallback. Mostar and Sarajevo are well over it on both. Mostar has 37 % rescue or fallback and sits at the extract's coastal edge, where Croatia is not in the tileset. These are properties of the region and the extract, not of the rebuild. They belong on the distance-error ledger, not on this cutover.

### 4.3 Latency: paired, K = 3 (the production serve)

Both engines ran side by side and were fired one request at a time, alternating which arm went first (ABBA). Each request was 180 Shuffle-harness cells (10 cities × 3 distances × 2 curviness × 3 seeds) at K = 3 with production costing. The other agent's engine benchmark was running on the same VM for part of this run, so **the CPU was shared**. Each engine's cgroup `cpu.stat` was therefore also read around every request: CPU-seconds per request is the direct effort measure.

| | prod tar | new tar | ratio |
|---|---|---|---|
| wall p50 / p90 (warm) | 0.142 / 0.565 s | 0.140 / 0.572 s | 0.99× / 1.01×, paired median 0.995 (p10 0.876, p90 1.069) |
| engine CPU-s per request p50 / p90 | 0.145 / 0.565 | 0.144 / 0.576 | paired median 0.994; total 46.7 → 45.2 CPU-s (**0.968×**) |
| candidates served | 540 | 540 | |
| **cold**: page cache evicted, 30-cell strided pass, p50 / p90 | 0.147 / 0.633 s | 0.143 / 0.615 s | paired median 0.974 |
| same 30 cells re-run warm | 0.144 / 0.560 s | 0.145 / 0.562 s | paired median 0.982 |
| major faults in the cold pass | +2 082 | +2 066 | |

Cold means each engine's tar pages were reclaimed through its own cgroup's `memory.reclaim` (349 MiB → 0 and 352 MiB → 0), so only those two containers' cache was touched. The tar pages fault back in at roughly the same rate for both tars, and the cold pass costs about +10 % at p90 on both. **Engine start from a cold cache:** the round-trip sidecar build reads every tile (6.3 s on the prod tar, 6.7 s on the new one), and the engine answers a round trip after 6 s and 8 s. So a restarted engine is page-cache-warm before it serves. Production's real cold case is eviction under memory pressure, which is what the reclaim run models.

### 4.4 Memory (cgroup v2, physical; `--memory 2g`, no swap, fresh engines)

| | prod tar | new tar |
|---|---|---|
| after start (sidecar built): anon / file | 211 / 350 MiB | 228 / 353 MiB |
| peak during start (sidecar build) | 629 MiB | 626 MiB |
| after 180 warm K = 3 asks: anon / file | 499 / 350 MiB | 469 / 353 MiB |
| cgroup peak over the run | 862 MiB | 836 MiB |
| process VmHWM (counts the tar mapping per reader) | 1 218 MiB | 1 183 MiB |
| single 300 km K = 12 asks (Belgrade s42 / s101), peak anon | 551 / 792 MiB | 507 / 775 MiB |

For comparison, the box's engine today shows 297 MiB anon and 507 MiB file (RssFile) under production traffic. The new tar adds 3 MiB of file-backed memory and no anon.

### 4.5 How many served loops change, and why

Per request, each served loop (slots 0–5) of run B is compared with run A's served loops for the same request:

- *identical*: same shape bytes.
- *same road*: ≥ 0.9 mutual grid cover at about 30 m in the same slot.
- *set-matched*: a ≥ 0.9 twin anywhere in A's served set.

| A → B | corpus-v2 (3 312 served loops) | balkans-v1 (648) |
|---|---|---|
| prod tar → new tar | identical 9.4 %, same road 44.3 %, changed 46.3 %; **set-matched 67.8 %** | identical 2.0 %, same road 60.8 %, changed 37.2 %; **set-matched 78.7 %** |
| prod tar → control (new OSM, 3.8.2-fork mjolnir) | identical 9.4 %, same road 44.5 %, changed 46.2 %; set-matched 67.9 % | set-matched 79.2 % |
| **control → new tar (3.9.1-fork mjolnir, same OSM)** | **identical 100.0 % (3 300 / 3 300)** | **identical 99.5 % (645 / 648; 3 loops at Sarajevo)** |

**About a third of the served loops are different loops after the rebuild, and the cause is the data.** With the OSM held fixed, moving the mjolnir from the fork's 3.8.2 base to its 3.9.1 base changes 3 of 3 948 served loops. That is despite the build differing in 42 mjolnir files upstream, small level-0/1 edge-count differences, and a different byte layout. Everything else comes from four months of OSM edits. 7.2 % of public-road `highway` ways in the merged extract were last touched on or after 2026-07-14, a lower bound, because node moves do not bump the way. The seeded search turns any change on a loop's corridor into a different loop.

The control cannot separate the July tar's own 3.7.0 mjolnir from the edits. Geofabrik no longer publishes a mid-July extract (the nearest is 2026-08-01), and the July image is gone. The 3.7 → 3.8.2 mjolnir delta was audited as small (2026-07-15 delta research). It is the only build-side term left, and it is not separable here.

This much churn is what a data refresh looks like, not a defect. Every quality meter above holds or improves. It does mean cached and banked loops from the July tiles must not be served against the new graph, so a `FINGERPRINT_VERSION` bump is required. The combined cutover's single bump covers it.

## 5. Cutover and rollback (tiles part of the combined cutover)

**Ship:** the candidate tar only. `admins.sqlite` and `timezones.sqlite` are build-time inputs (their content is baked into the tiles). The box's copies stay as they are, and the entrypoint will not rebuild them, because they exist.

```bash
# laptop
T=/Users/xenix/Projects/curvagen-tiles-391/out/valhalla_tiles.balkans-osm20261103-v391.tar
shasum -a 256 "$T"     # ba2b273b70abe8b762dc0bb19f6d37d2ec46b75281c5b977bec220e2af93d7f9
scp "$T" root@<box>:/opt/curvagen/data/valhalla_tiles.tar.new     # 370 MB; the box has ~15 GB free

# box — in the cutover window, together with the 3.9.1 + (b2) engine image and the FINGERPRINT bump
cd /opt/curvagen/data
sha256sum valhalla_tiles.tar.new                       # must match the line above
mv valhalla_tiles.tar     valhalla_tiles.tar.bak-20261104   # the rollback target (the running engine keeps its mmap until restart)
mv valhalla_tiles.tar.new valhalla_tiles.tar
# then recreate valhalla with the new image per curvagen-meta/deploy (one restart covers image + tiles)
docker logs curvagen-valhalla 2>&1 | grep 'road identity sidecar'
#   expect "364 tiles, 1602869 segments" (the July tar reads "364 tiles, 1583271 segments")
curl -s localhost:8002/status                          # version 3.9.1, new tileset_last_modified
# smoke: a 50 km round trip (distance in METERS on the orchestrator DTO)
```

**Rollback** follows curvagen-meta `deploy/ROLLBACK.md`'s Tiles row. It takes about a minute rather than a re-scp, because the previous tar stays on the box:

```bash
cd /opt/curvagen/data
mv valhalla_tiles.tar valhalla_tiles.tar.bad-20261104
mv valhalla_tiles.tar.bak-20261104 valhalla_tiles.tar
docker restart curvagen-valhalla      # sidecar line back to "1583271 segments"
```

- **The July tar on the 3.9.1 engine is a measured combination.** It is the baseline arm of every row in §4. So a tiles-only rollback that keeps the new engine is safe for serving.
- **A tiles-only rollback still changes route output.** By the FINGERPRINT rule it needs its own bump, or loops cached against the new graph keep serving until the TTL expires. A full rollback (engine image, tiles and orchestrator together) restores the previous keys as ROLLBACK.md describes.
- The box also still holds `valhalla_tiles.tar.bak-20260714`, the 216 MB Serbia-only tar from June. It is no longer anyone's rollback target.

**Optional, independent of the tar:** the box's `elevation_data/` holds 19 SRTM tiles that cover Serbia only. `/height` returns `null` at Banja Luka, Sarajevo and Podgorica, and the orchestrator's elevation profile (`engine-client` `elevations()`) zero-fills there. This build downloaded the 13 missing tiles. The full set of 32 is in `/Users/xenix/Projects/curvagen-tiles-391/build/elevation_data/`. Shipping it would give Riders outside Serbia an elevation profile. It does not change route output (the tiles' baked elevation is identical in both tars), so it needs no FINGERPRINT bump.

## 6. Method notes and what went wrong

- **Shared machine.** Another agent's engine benchmarks ran on the same Docker VM (4 vCPU, 7.9 GB) the whole time.
  - All tile builds ran first, so their times are upper bounds.
  - Loop outputs are deterministic for a given binary, config, seed and tar: the only clock in the round-trip is the stage-timing ledger. So the quality numbers do not depend on load. Latency comes only from the paired run, which also reports CPU-seconds.
- **A VM-wide OOM during the corpus runs.** Running three or four of my engines (3 GB cap each) beside the other agent's engines exhausted the VM.
  - The kernel's global OOM killer took two of my engines mid-corpus and **one process in the other agent's container**.
  - From then on I ran one engine at a time and re-fired only the failed requests. loopqual resumes from saved responses, and outputs are deterministic, so the final runs are 552/552 and 108/108 on every arm. Two control-arm requests that timed out client-side are excluded from its diff (550 of 552).
  - This was the sum of the containers, not the tiles: each engine sat at about 0.9–1.0 GB anon when it was killed, and the same 300 km asks measure the same per-request peak on both tars (§4.4).
  - Kept as an operational note. K = 12 asks with two 300 km requests in flight hold about 1 GB anon per engine. That fits the box's 2.8 GB available today (Bank Fill is off; production serves K = 3), with little room to spare if Bank Fill at K = 12 comes back.
- **Files.** Under `/Users/xenix/Projects/curvagen-tiles-391/` on the build laptop:
  - build logs and stage timings in `build/{bc,nobc,x382}/`
  - the merged PBF in `pbf/`
  - the copied production tar in `baseline/`
  - loopqual runs in this worktree's `tools/loopqual/results/t391-{old,new,x382}-*` (gitignored)
  - the paired, Shuffle, cold and identity scripts and logs in the session scratchpad (`tiles391/`)

  Nothing large is committed. The only repo additions are this doc and `corpus-balkans-v1.yaml`.


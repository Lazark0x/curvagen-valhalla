# What production actually serves: the demand ledger, corpus-v2, and the first reading of served loops

- **Date:** 2026-09-05 (curvagen-valhalla [#9](https://github.com/Lazark0x/curvagen-valhalla/issues/9), child of the field-defect census map [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); blocks [#5](https://github.com/Lazark0x/curvagen-valhalla/issues/5))
- **Scope:** the two inputs only production has — **which cells riders actually request** (`origin_stats`) and **what loops the engine actually produced for them** (Memo + Candidate Bank bodies) — plus the corpus the census will run (`tools/loopqual/corpus-v2.yaml`) and the ROPA line the copy needs.
- **Method:** one read-only copy of the production orchestrator's SQLite cache (rsync from the Hetzner box's `curvagen_cache-data` volume, 2026-09-05T21:38:07Z–21:38:10Z; no writes, no restarts, no `docker exec`, nothing left on the box). Everything after that is offline: the copy is read via `file:…?mode=ro`, loops are metered with the pinned harness `tools/loopqual/metrics.py` (v1.3) and the D1–D5 blind-spot prototypes in `~/.curvagen-scratch/` — **nothing under `tools/loopqual/` was modified**; the one adaptation (contract 3.x `candidates[].geometry` is polyline6 2D, so `Loop.from_serving_route` cannot ingest it) lives in a scratch script. One container (`rt-corpusv2-locate`, port 8003, prod-equivalent image) was started for `/locate` snap checks of the corpus origins and removed; `valhalla-local` on :8002 was never touched. All published aggregates are **cell-level only** (0.001° ≈ 111 m) — see §7.
- **Artifacts:** `tools/loopqual/corpus-v2.yaml` (untracked), `~/.curvagen-scratch/prod-cache/` (copy + scripts + `ropa.diff`).

## TL;DR verdict

**Production demand is one neighbourhood, one Road Preference and one distance band — and the loops served into it read 0.0 % on every gated meter while carrying more invisible retrace than any corpus cell measured so far.**

1. **Demand is tiny and concentrated.** 163 Round Trip requests across 23 cells; after removing two iOS-simulator-default cells (Cupertino) and two lat/lon-transposed pre-3.0 client cells, **8 Serbia cells carry 144 requests (88 %)** — all inside a **5.1 × 3.1 km box in central Belgrade**. The top cell alone is 47.9 %. Andrey's Vračar field cell sits **0.97–2.75 km** from every one of them, so it is a fair proxy for essentially all production demand.
2. **Corpus-v1 models the wrong Road Preference.** **151 of 163 requests (93 %) ride with motorways allowed** (`a0`), while corpus-v1 pins `avoid_motorways: true` on all 232 jobs. Riders also use curviness **0.5 (87), 1.0 (38), 0.7 (19), 0.65/0.75 (9 each)** — not v1's {0.5, 0.8}. `corpus-v2` fixes both and keeps all 232 v1 jobs verbatim for comparability: **552 requests, 2.38× v1, 6 624 loops.**
3. **Every gated meter reads zero on real served loops.** Across 111 stored candidates: `spike_ge_30m` **0.0 %**, `is_lollipop` **0.0 %**, `edge_reuse_geom` mean **0.021** (max 0.173, nothing above the 0.30 tail gate), `distance_error` mean 0.131 / p90 0.264. Every Gate v1.2 input that can be evaluated on a cache dump (gates 1–7) reads clean, most of them by an order of magnitude.
4. **The blind-spot detectors read the opposite.** On the same 111 loops: **64.0 %** carry ≥ 500 m of near-mirror riding that no meter sees (b1-x02 corpus-wide: 32.5 %), **51.4 %** carry ≥ 2 km of it (corpus 8.8 %), and the mean unseen near-mirror is **6 614 m per loop — 13.1 % of the ride** (p90 **42 %**). **82.9 %** contain a near-rejoin ring, **71.2 %** a bulb-class ring ≥ 2 km (corpus 24.8 %), **84.7 %** a transversal self-crossing (corpus 62.1 %).
5. **The urban terrain, not the engine generation, explains the gap.** The corpus's own `belgrade` origin reads 70.1 % / 39.9 % / 5 774 m on the same three near-mirror measures against production's 64.0 % / 51.4 % / 6 614 m — the same regime, and nothing like the 32.5 % / 8.8 % / 1 217 m corpus average. Dense one-way pairs and dual carriageways are where riders ride, and they are exactly the class `edge_reuse_geom` cannot see. **A census run only on corpus-v1's terrain mix would under-report the defect riders actually meet by ~2×.**
6. **The next-served bank candidates are the worst ones.** A Bank tap serves the three lowest remaining ranks (`lookup.rs:53-56`), i.e. ranks 4–6. Those read **60.7 %** on same-pavement retrace ≥ 500 m against **31.6 %** at ranks 7–9 and **26.2 %** at ranks 10–12 (n = 28/38/42 — indicative, not significant). Engine rank and retrace move together, reconfirming the atlas §9 / blind-spots §8 finding from the serving side.
7. **The harness cannot read the current contract.** `Loop.from_serving_route` (`metrics.py:225-244`) expects the retired app DTO (`paths[0].points`, 3D polyline at 1e5). Contract 3.x hands `candidates[].geometry` as **2D polyline6** (`crates/domain/src/dto_v3.rs:222-249`). Serving-mode ingestion is dead code against production today — same class of staleness as `runner.py::build_request` (audit-rig-confirmation §8.5).

---

## 1. Copy provenance

The production stack (`curvagen-api`, `curvagen-valhalla`, `curvagen-postgres`, `curvagen-nginx`) was healthy at copy time — all four containers `Up`, root filesystem 61 % used, load 0.19. Nothing was restarted, written, deployed or left behind; the only remote commands were `uptime`, `df`, `docker ps`, `docker volume ls`, `docker volume inspect`, `docker inspect`, `ls`, `stat`, and one `command -v sqlite3` (absent on both host and container — so no `sqlite3 .backup`; nothing was installed).

`CACHE_DB=/data/cache.db` is the named volume `curvagen_cache-data` mounted at `/data` in `curvagen-api` (`curvagen-meta/deploy/docker-compose.yml:116,137-139`), host path `/var/lib/docker/volumes/curvagen_cache-data/_data`.

| | |
|---|---|
| Copied | `cache.db`, `cache.db-wal`, `cache.db-shm` — **one `rsync` invocation**, so the WAL and its database were taken together |
| Started / finished | 2026-09-05T21:38:07Z / 2026-09-05T21:38:10Z (3 s, 6 678 342 B transferred) |
| Sizes | `cache.db` 3 698 688 B (mtime 2026-09-02T00:24:56Z) · `cache.db-wal` 2 945 832 B (2026-09-05T15:02:41Z) · `cache.db-shm` 32 768 B (2026-09-05T15:25:03Z) |
| sha256 | `7ed86aaa…d48d4a8` (db) · `2d970acc…187a0584` (wal) · `dbb11467…c170f78c` (shm) |
| WAL handling | The database file had not been checkpointed since 2026-09-02; **most recent writes lived in the WAL**. The pristine copy is never opened for writing — a *working copy* under `work/` was opened once read-write so SQLite replayed the WAL, and `PRAGMA integrity_check` returned **`ok`** |
| Location | `~/.curvagen-scratch/prod-cache/` (mode 0700), copy + `work/` + scripts |

Row counts (after WAL replay):

| Table | Rows | Notes |
|---|---|---|
| `entries` kind 0 (Memo) | 3 | 1 Round Trip (seeded) + 2 one-way |
| `entries` kind 1 (Bank) | 108 | 14 distinct seedless prefixes, `slot_rank` 4–12 |
| `entries` kind 2 (Negative) | 14 | definitive `NO_ROUTE_FOUND` verdicts, no TTL |
| `origin_stats` | 49 | 23 distinct cells, 86 hits + 77 misses = **163 requests** |
| `entry_origins` (R*Tree) | — | not read (the shipped `sqlite3` lacks the rtree module; not needed) |

Stored body bytes: 43 526 (Memo) + 584 469 (Bank), zstd level 2 with a 1-byte codec prefix (`store.rs:19-21,95-106`). Entry lifetimes: Memo/Bank TTL 30 days (`store.rs:12-14`); **all 111 loop-bearing rows were still live at copy time**, and the earliest bank expiry was 2026-09-05T23:23:53Z — about two hours after the copy. Created-at span: 2026-07-16 (oldest negative) → 2026-09-05T15:02 (newest Memo). Single tileset throughout: `1784040258`.

## 2. Schema and decoding

| Claim | Source |
|---|---|
| `entries(key, kind, tileset_version, fingerprint, curviness_2dp, target_bucket_km, actual_distance_m, slot_rank, origin_lat, origin_lon, created_at, expires_at, last_hit, body)`; `origin_stats(prefix, cell_lat, cell_lon, hits, misses)` | `crates/cache/src/store.rs:58-87` |
| `Kind`: Memo = 0, Bank = 1, Negative = 2 | `crates/cache/src/lib.rs:23-27` |
| Seedless prefix `rt\|{tileset}\|{fingerprint}\|{lon:.5f}\|{lat:.5f}\|d{bucket_km}\|c{curviness:.2f}\|a{0\|1}`; Memo key = prefix + `\|s{seed}` | `crates/cache/src/key.rs:59-75`, `:94-97` |
| Distance bucket = `round_ties_even(m / 1000)` km | `crates/cache/src/key.rs:82-84` |
| Fingerprint = `v{FINGERPRINT_VERSION}:{fnv1a64}` of the costing JSON; **currently v4** (contract 3.1.0 rotation) | `crates/cache/src/key.rs:27-41` |
| Origin Stats cell = `floor(deg × 1000)` — 0.001°, ~111 m; "a ledger, not a geoindex" | `crates/cache/src/store.rs:365-380` (cells at `:368-369`) |
| Memo body = the whole stamped `CandidatesResponse` | `crates/api/src/serving/lookup.rs:410-417` |
| Bank body = one `Candidate`, banked at 1-based engine rank `i + 1` for `i ≥ SERVE_K` ⇒ ranks 4–12 at `BANK_FILL_K=12` | `crates/api/src/serving/fill.rs:173-181`, `mod.rs:37-38` |
| Bank rows are consumed on serve (`DELETE … RETURNING`) | `crates/cache/src/store.rs:298-328` |
| A Bank tap serves the three **lowest remaining** ranks; with ε = 0.15 the last one is swapped for a uniform pick from the remainder | `crates/api/src/serving/lookup.rs:53-90`, `mod.rs:47` |
| `Candidate.geometry` is **polyline6, 2D**; `distance` meters; `curvinessUsed` unitless | `crates/domain/src/dto_v3.rs:222-249` |

**What the cache is and is not.** It is not a log of serves. Shuffle serves are never memoized (only seeded Round Trips are, `lookup.rs:95-97`), bank rows vanish when tapped, the TTL is 30 days, and the v3→v4 fingerprint rotation retired every pre-3.1.0 key. So 111 loops is what survives, not what was served in the window. Every one of them is still a loop the engine produced for a real request at a real origin, and the 108 banked candidates are the pool the next Shuffle taps draw from. One qualification, found by matching keys: the **only Round Trip Memo in the cache is this rig's own equivalence probe** from earlier today (`d50 c0.50 a1 s445044` at the corpus `belgrade` origin — audit-rig-confirmation §5), together with its 9-slot Bank Fill. **The cache holds no rider-served Memo at all**: rider serves in this window were all Shuffle, which is never memoized.

## 3. Demand — what riders actually ask for

163 Round Trip requests, 86 hits / 77 misses (a 52.8 % hit rate). No one-way (`ow|`) rows appear in `origin_stats` at all.

### 3.1 Cells, ranked

| Rank | Cell (0.001°) | Requests | Share | Hits | Misses | Class |
|---|---|---|---|---|---|---|
| 1 | (44.792, 20.493) | 78 | 47.9 % | 45 | 33 | Belgrade |
| 2 | (44.796, 20.437) | 19 | 11.7 % | 12 | 7 | Belgrade |
| 3 | (44.786, 20.448) | 13 | 8.0 % | 9 | 4 | Belgrade |
| 4 | (44.794, 20.491) | 12 | 7.4 % | 7 | 5 | Belgrade |
| 5 | (44.797, 20.437) | 8 | 4.9 % | 5 | 3 | Belgrade |
| 6 | (44.812, 20.461) | 5 | 3.1 % | 2 | 3 | Belgrade |
| 7 | (44.784, 20.501) | 5 | 3.1 % | 3 | 2 | Belgrade |
| 8 | (44.800, 20.460) | 4 | 2.5 % | 2 | 2 | Belgrade |
| 9–23 | 15 cells with 1–2 requests | 19 | 11.7 % | 1 | 18 | incl. 3 artifact cells (below) |

**Artifacts, excluded from the corpus.** Two cells are iOS-Simulator defaults — (37.421, −122.084) is the Googleplex pin, (37.235, −122.040) its Cupertino neighbour, 3 requests, all misses, outside the Serbia tileset. Two more are lat/lon **transposed**: (20.461, 44.812) is Belgrade centre with the pair swapped and (19.700, 43.729) is Zlatibor swapped — both `v2` fingerprints, i.e. pre-3.0 clients sending the old positional `[lon,lat]` array (the shape `dto_v3::LonLat`'s hand-written map-only deserializer now rejects, `dto_v3.rs:37-40,51-96`). One further cell, (43.476, 18.748), is in Bosnia — one request, one miss, outside the tileset. One more below-cut cell, (44.790, 20.450), is **this rig's own equivalence probe** from 2026-09-05T15:02Z (audit-rig-confirmation §5), not rider demand; it is below the cut and enters neither the corpus nor the demand marginals' interpretation.

### 3.2 The cut

**Cut: cells with ≥ 3 requests, artifact cells removed ⇒ 8 cells, 144 requests, 88 % of all demand.** Justification: the distribution is a cliff, not a slope — rank 8 has 4 requests and rank 9 has 2, so ≥ 3 and ≥ 4 select exactly the same 8 cells; going to ≥ 2 adds 3 cells and 6 requests (+4 pp) at the cost of three cells whose entire history is a single missed request, and ≥ 5 drops a real cell for 2 pp. Everything below the cut is one-shot traffic that carries no band information (each has a single distance and a single curviness).

Coverage: the 8 cells span **44.785–44.812 N, 20.438–20.502 E — a 5.1 × 3.1 km box** over central Belgrade. There is **no demand outside Belgrade at all** in this window.

### 3.3 Bands the cut cells requested

| Cell | Req | Distance buckets (km) | Curviness | Road Preference |
|---|---|---|---|---|
| (44.792, 20.493) | 78 | 30:4, 50:29, 60:9, 70:2, 75:2, 100:1, 110:5, 150:15, 170:2, 220:2, 290:1, 300:6 | 0.5:44, 0.6:1, 0.65:6, 0.75:9, 1.0:18 | a0 ×78 |
| (44.796, 20.437) | 19 | 100:19 | 0.7:19 | a0 ×19 |
| (44.786, 20.448) | 13 | 50:11, 100:2 | 0.5:13 | a0 ×13 |
| (44.794, 20.491) | 12 | 60:6, 70:6 | 1.0:12 | a0 ×12 |
| (44.797, 20.437) | 8 | 70:8 | 0.65:2, 1.0:6 | a0 ×8 |
| (44.812, 20.461) | 5 | 50:3, 110:2 | 0.5:5 | a0 ×4, a1 ×1 |
| (44.784, 20.501) | 5 | 50:5 | 0.5:5 | a0 ×5 |
| (44.800, 20.460) | 4 | 50:4 | 0.5:4 | a1 ×4 |

Whole-ledger marginals:

- **Distance:** 50 km **62** · 100 km 23 · 70 km 16 · 60 km 15 · 150 km 15 · 110 km 10 · 300 km 6 · 30 km 4 · rest ≤ 2 each. Half of all demand is 50–100 km; 20 km (a corpus-v1 rung) was **never requested**, and 200 km (another rung) was never requested either.
- **Curviness:** 0.5 **87** · 1.0 **38** · 0.7 19 · 0.65 9 · 0.75 9 · 0.6 1. Riders use the extremes and a mid band; **0.8 — corpus-v1's high rung — was never requested.**
- **Road Preference:** `a0` (motorways allowed) **151** vs `a1` (avoided) **12**. 93 % of demand is the mode corpus-v1 never runs.
- **Generation:** v2 7 · v3 114 · v4 42 requests, i.e. the ledger spans three fingerprint generations (it is never rotated — only `entries` are).

**What this data cannot tell us: how many riders.** `origin_stats` has no rider id, no device id, no timestamps and no request sequence — it is four integers per (prefix, cell). 163 requests could be one rider's fortnight or forty riders' afternoon; nothing in the cache distinguishes them, and nothing should. Every "demand" statement here is about **requests**, never about people. (The rider-data ledger that *does* carry a pseudonymous id is a separate Postgres store, not this cache, and is not touched by this work.)

## 4. corpus-v2

`tools/loopqual/corpus-v2.yaml` — **untracked**, sha256 `bf40ee110d001a0f69f5b74b32db0112523d00312a8e8c7ab69bc8bf5433c83a`. Validated through the harness's own loader (`runner.load_corpus` + `enumerate_jobs`): **552 jobs, K = 12, 6 624 loops, 0 result-filename collisions**, and all **232 corpus-v1 jobs reproduced verbatim** (exact set equality on origin/distance/curviness/seed/avoid_motorways).

| Block | Origins | Distances (km) | Curviness | Seeds | `avoid_motorways` | Requests |
|---|---|---|---|---|---|---|
| **C** — corpus-v1 verbatim | the 8 v1 origins | 20/50/100/200/300 | 0.5 | 7,11,23,42,101 | true | 200 |
| **C** — corpus-v1 verbatim | the 8 v1 origins | 50/200 | 0.8 | 7,11 | true | 32 |
| **A** — Vračar field cell | `vracar` | 25/30/40/50/60 | 0.5 | 7,11,23,42,101 | **false** | 25 |
| **A** | `vracar` | 25/30/40/50/60 | 0.7 | 7,11,23,42,101 | **false** | 25 |
| **A** | `vracar` | 25/30/40/50/60 | 1.0 | 7,11,23,42,101 | **false** | 25 |
| **A** — Road Preference control | `vracar_amw` (same point) | 25/30/40/50/60 | 0.5 | 7,11,23,42,101 | true | 25 |
| **B** — demand cells | the 8 cut cells | 50/100 | 0.5 | 7,11,23,42,101 | **false** | 80 |
| **B** | the 8 cut cells | 60/70 | 1.0 | 7,11,23,42,101 | **false** | 80 |
| **B** | the 8 cut cells | 100 | 0.7 | 7,11,23,42,101 | **false** | 40 |
| **B** — long tail | ranks 1–2 | 150/300 | 0.5 | 7,11,23,42,101 | **false** | 20 |
| | | | | | **total** | **552** |

Design notes:

- **Continuity is exact, not approximate.** Block C is corpus-v1's job set character-for-character (same origins, rungs, seeds, flag), so every Baseline v1 number stays a like-for-like comparison; the corpus id is new because the *file* is new, per the harness's own rule.
- **Demand cells enter as cell centres** (e.g. cell (44 792, 20 493) → 44.7925, 20.4935), named `d<cell_lat><cell_lon>`. That is the same 0.001° coarsening the ledger itself applies — no rider start point is written into the corpus.
- **Curviness levels are the observed ones** — 0.5 everywhere, plus 0.7 (the entire c0.7 mass is one cell at 100 km, reproduced exactly) and 1.0 (observed at 60/70 km, reproduced exactly). Block C keeps 0.8 so the v1 rung is not lost.
- **Two names for one point.** `runner.job_filename` (`runner.py:78-79`) keys result files by origin/distance/curviness/seed and does **not** encode `avoid_motorways`, so the a1 control at Vračar would silently overwrite the a0 block's responses. `vracar_amw` is the same coordinate under a second name — the alternative was a harness edit, which this ticket does not license.
- **Snap check.** Every one of the 17 origins returns at least one motorcycle-costed edge from `/locate` on the prod-equivalent engine: 16 within 50 m (11 within 30 m, two at 0.0–0.1 m), and `vlasina` at **750 m** — the same sparse-origin behaviour corpus-v1 already has, kept deliberately.
- **Vračar is inside the demand box.** 0.97 km from cell rank 8, 1.57 km from rank 4, 1.78 km from rank 1, ≤ 2.75 km from all eight; and 1.96 km from corpus-v1's `belgrade`. The field cell, the demand mass and the v1 urban origin are the same neighbourhood at three resolutions.

## 5. Served-loop readings

**111 loops:** 3 from the single Round Trip Memo (ranks 1–3, created 2026-09-05T15:02Z — this rig's equivalence probe, §2) and 108 banked candidates (ranks 4–12 over 14 prefixes), of which 9 belong to that same probe's fill. **99 loops are rider-driven.** Dropping the probe's 12 moves nothing material: D1 ≥ 500 m 37.8 → 36.4 %, D1b unseen ≥ 500 m 64.0 → 59.6 %, unseen mean 6 614 → 6 094 m, rings 82.9 → 80.8 %, crossings 84.7 → 82.8 %, and every gated meter stays at 0.0 %. Aggregates below keep all 111 and label the probe's cell. Requested distances 40–100 km, curviness 0.5/0.65/0.7/1.0, mean ride **61.4 km**. Seven cells are represented; two of them (44.806/20.407 and 44.787/20.457) sit below the demand cut, and the two heaviest demand cells are present.

Serving-mode caveat, applied throughout: the stored geometry is one combined polyline with no legs, so the seam (turnaround) is **derived** as the grid point farthest from the start (`metrics.py:225-244`'s rule, re-implemented for polyline6). Everything seam-anchored — spike classes, `lollipop_stem_fraction`, `shadow_frac`, `rejoin_return_frac`, `seam_frac`, D1c, D6 — is **approximate**; geometry-only meters (`edge_reuse_geom`, `compactness`, D1 runs, D3 rings and crossings, D5 lobes) are exact. The blind-spots doc's "seam is not the ride's farthest point" detector (D6c) is structurally **N/A** here: in serving mode the seam *is* the farthest point by construction.

### 5.1 Gate v1.3 meters vs the D1–D5 detectors

| Detector fires on | prod ALL (n=111) | memo ranks 1–3 (n=3) | bank ranks 4–12 (n=108) | b1-x02 ALL (n=2779) | b1-x02 `belgrade` (n=348) |
|---|---|---|---|---|---|
| D1 retrace: ≥ 1 km ridden both ways (same pavement) | 25.2 % | 66.7 % | 24.1 % | 31.1 % | 23.9 % |
| D1 longest one-way same-pavement run ≥ 500 m | **37.8 %** | 66.7 % | 37.0 % | 32.9 % | 23.9 % |
| D1 longest one-way same-pavement run ≥ 2 km | 8.1 % | 33.3 % | 7.4 % | 25.9 % | 17.2 % |
| D1b near-mirror (r = 25 m) unseen by any meter ≥ 500 m | **64.0 %** | 100 % | 63.0 % | 32.5 % | 70.1 % |
| D1b near-mirror unseen ≥ 2 km | **51.4 %** | 100 % | 50.0 % | 8.8 % | 39.9 % |
| D1c near-mirror U-turn at the seam ≥ 200 m *(approx)* | 3.6 % | 0.0 % | 3.7 % | 2.7 % | 1.7 % |
| D2 exempt-zone shared corridor ≥ 5 % of the ride | 1.8 % | 0.0 % | 1.9 % | 7.2 % | 2.9 % |
| D2 reuse hidden by the exemption ≥ 1 km | 10.8 % | 0.0 % | 11.1 % | 14.8 % | 0.0 % |
| D3 ≥ 1 near-rejoin ring (IQ ≥ 0.15, ≥ 800 m) | **82.9 %** | 100 % | 82.4 % | 49.6 % | 68.7 % |
| D3 ring inside a single leg *(approx)* | 15.3 % | 33.3 % | 14.8 % | 13.7 % | 11.2 % |
| D3 bulb-class ring (≤ 25 % of loop) ≥ 2 km | **71.2 %** | 66.7 % | 71.3 % | 24.8 % | 46.6 % |
| D3b ≥ 1 transversal self-crossing | **84.7 %** | 100 % | 84.3 % | 62.1 % | 72.7 % |
| D3b figure-8 (crossing lobe ≥ 25 % of loop) | 36.9 % | 33.3 % | 37.0 % | 18.0 % | 25.0 % |
| D4 reuse outside the exemption ≥ 100 m (Fallback proxy) | 37.8 % | 33.3 % | 38.0 % | 35.2 % | 22.1 % |
| D5 two prominent distance lobes | 0.9 % | 0.0 % | 0.9 % | 3.0 % | 0.6 % |
| D6 cross-leg shared corridor ≥ 25 % of the ride *(approx)* | 39.6 % | 100 % | 38.0 % | 11.6 % | 22.4 % |
| D6 stem zeroed by the 120 m gap but ≥ 1.5 km at gap 1 km *(approx)* | 30.6 % | 100 % | 28.7 % | 11.7 % | 29.9 % |
| — reference: Gate `spike_ge_30m` | **0.0 %** | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| — reference: Gate `is_lollipop` | **0.0 %** | 0.0 % | 0.0 % | 0.3 % | 0.0 % |
| — reference: Gate `edge_reuse_geom > 0.30` | **0.0 %** | 0.0 % | 0.0 % | 7.1 % | 1.4 % |

Magnitudes behind the near-mirror line:

| | prod served/banked | b1-x02 ALL | b1-x02 `belgrade` |
|---|---|---|---|
| unseen near-mirror metres per loop (mean / p50 / p90) | **6 614 / 2 419 / 20 626** | 1 217 / 237 / 1 525 | 5 774 / 1 167 / 19 262 |
| … as a share of the ride (mean / p90) | **0.131 / 0.420** | 0.017 / 0.029 | 0.080 / 0.265 |
| same-pavement both-ways metres per loop (mean) | 1 363 | 5 180 | 1 652 |

The pattern is consistent: production loops carry **less** exact-pavement retrace than the corpus average (urban grids offer alternatives; mountain corpora do not) and **far more** near-mirror retrace — the class that is invisible to `find_spikes` *and* to `edge_reuse_geom`, and the one the v3 engine moved least (blind-spots §2).

### 5.2 By demand cell (cell-level only)

| Cell | n | mean km | `dist_err` | reuse mean | D1 ≥ 500 m | D1b unseen ≥ 500 m | D1b unseen m (mean) | D3 ring | D3 bulb ≥ 2 km | D3b xing | D6 shadow ≥ 25 % *(approx)* |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (44.792, 20.493) *(demand rank 1)* | 30 | 59.5 | 0.122 | 0.016 | 20.0 % | 50.0 % | 5 457 | 83.3 % | 83.3 % | 83.3 % | 30.0 % |
| (44.806, 20.407) *(below cut)* | 18 | 80.4 | 0.155 | 0.026 | 27.8 % | 44.4 % | 3 449 | 55.6 % | 33.3 % | 66.7 % | 16.7 % |
| (44.787, 20.457) *(below cut)* | 18 | 38.0 | 0.103 | 0.031 | **88.9 %** | **88.9 %** | 7 866 | 88.9 % | 66.7 % | **100 %** | 66.7 % |
| (44.794, 20.491) *(demand rank 4)* | 15 | 60.5 | 0.171 | 0.014 | 33.3 % | 53.3 % | 5 360 | 93.3 % | 73.3 % | 93.3 % | 26.7 % |
| (44.790, 20.450) *(this rig's probe, not rider demand)* | 12 | 48.5 | 0.124 | 0.033 | 50.0 % | **100 %** | 10 901 | **100 %** | 83.3 % | **100 %** | **75.0 %** |
| (44.796, 20.437) *(demand rank 2)* | 9 | 109.3 | 0.141 | 0.001 | 11.1 % | 55.6 % | 7 155 | 77.8 % | 77.8 % | 55.6 % | 22.2 % |
| (44.784, 20.501) *(demand rank 7)* | 9 | 48.0 | 0.098 | 0.030 | 33.3 % | 77.8 % | 10 127 | 88.9 % | 88.9 % | 88.9 % | 55.6 % |

Per-cell n is 9–30; treat these as indicative. The spread is nonetheless large and terrain-shaped: the 40 km cell (44.787, 20.457) fires on nearly everything, while the 100 km cell (44.796, 20.437) — long loops that escape the city — is the cleanest on every meter except the near-mirror one, which stays at 55.6 % because the escape corridors themselves are dual carriageways.

**The Vračar field cell (44.797, 20.472) has no rows in the cache** — neither demand nor loops. Its nearest neighbours in the ledger are 0.97 km (4 requests) and 1.57 km (12 requests) away. Corpus-v2 block A is therefore the *first* measurement of that cell, not a re-measurement.

### 5.3 By served slot

| Slice | n | mean km | reuse mean | D1 ≥ 500 m | D1b unseen ≥ 500 m | D6 shadow ≥ 25 % | IQ mean |
|---|---|---|---|---|---|---|---|
| Memo ranks 1–3 (the rig's probe) | 3 | 47.6 | 0.030 | 66.7 % | 100 % | 100 % | 0.067 |
| Bank ranks 4–6 (**next to be served**) | 28 | 62.9 | 0.033 | **60.7 %** | 57.1 % | 35.7 % | 0.219 |
| Bank ranks 7–9 | 38 | 65.0 | 0.019 | 31.6 % | 68.4 % | 10.5 % | 0.191 |
| Bank ranks 10–12 | 42 | 58.2 | 0.015 | **26.2 %** | 61.9 % | 40.5 % | 0.216 |

A Bank tap serves the three lowest remaining ranks, so "ranks 4–6" is not a bystander slice — it is literally the next serve for those prefixes (with a 15 % chance the third slot is swapped for a random higher rank, `mod.rs:47`). Same-pavement retrace **falls monotonically with rank** (60.7 → 31.6 → 26.2 %) and so does mean `edge_reuse_geom` (0.033 → 0.019 → 0.015): the engine's preferred candidates are its most retraced ones. The Memo row is 3 loops from this rig's own probe — quoted for completeness, not as evidence.

### 5.4 Extremes (cell-labelled)

| Reading | Value | Where |
|---|---|---|
| Longest same-pavement one-way retrace run | 3 860 m | cell (44.792, 20.493), d60 km c1.0, bank rank 7, 58.2 km ride, `edge_reuse_geom` 0.126, `spike_count` 0 |
| Unseen near-mirror metres in one loop | 26 356 m (**51 % of the ride**) | cell (44.784, 20.501), d50 km c0.5, bank rank 8, 51.8 km ride, reuse 0.109, spikes 0 |
| Highest `edge_reuse_geom` in the cache | 0.173 | cell (44.806, 20.407), d50 km c0.65, bank rank 6 — still **below** every gate threshold |
| Largest mid-route ring | 47 308 m | cell (44.806, 20.407), d100 km c1.0, bank rank 8, 118.5 km ride, reuse 0.000 |
| Highest undiscounted stem fraction | 0.104 | cell (44.787, 20.457), d40 km c0.5, bank rank 11 — the gated meter reads 0.0 |

Other v1.3 readings across the 111 loops: `distance_error` mean 0.131 / p90 0.264 / max 0.378 (the fork's ±18 % harvest band shows as a right tail); `compactness` mean 0.204, p10 0.021; `shadow_frac` mean 0.236, p90 0.695; `curviness_retention` **exactly 1.0 for every loop** — as expected, the orchestrator echoes the requested value straight through (`crates/api/src/serving/transform.rs:10-23` → `:83`), so the meter remains a future-drift alarm, not a measurement; declared vs geometric length differ by 51 m on average (polyline rounding).

## 6. Caveats

1. **n = 111 loops from 14 prefixes + 1 Memo.** Cell rows with n < 20 are indicative. Nothing here is a significance claim; the census run on corpus-v2 (6 624 loops) is the measurement, this is its aim.
2. **Derived seam.** Every metric marked *(approx)* above depends on the turnaround, which serving geometry does not carry. D6's two rows and D1c are the most affected; the D6c "seam is not the farthest point" test is meaningless in this mode.
3. **No rider-served Memo exists in the cache.** The one Memo is this rig's own equivalence probe (§2); rider traffic in the window was Shuffle, which is never memoized. Rider-visible evidence is therefore the bank pool — 99 loops after the probe's 12 are removed — read through what a Bank tap would hand out.
4. **The bank is not the serve.** 108 of 111 loops are ranks 4–12. They reach riders through Bank taps (ranks 4–6 first) and ε-exploration, but the ranks 1–3 that most riders saw were consumed at generation time and are gone unless the request was seeded.
5. **Survivorship.** 30-day TTL, consume-once bank reads, unmemoized Shuffle serves and the v3→v4 fingerprint rotation all delete history. The cache under-represents traffic and skews toward *unserved* candidates and *recent* prefixes.
6. **Mixed engine generations.** Entry `created_at` spans 2026-08-06 → 2026-09-05 across fingerprints v3 (51 loops) and v4 (60). The engine binary is **not** part of the cache key, and prod's `:amd64` tag floats; the v3-era rows may come from a different build than today's `b4f514d7f`. Split readings by generation differ little (D1 ≥ 500 m: v3 39.2 % vs v4 36.7 %; unseen near-mirror ≥ 500 m: 64.7 % vs 63.3 %), which is reassuring but not conclusive.
7. **Demand ≠ riders.** No identifiers, no timestamps, no sequence in `origin_stats` — request counts only (§3.3).
8. **Two lat/lon-transposed and three out-of-tileset cells** are client artifacts, not demand; excluded from the corpus and flagged here so nobody re-derives them as "international demand".
9. **The 111 loops were metered with the detectors as they stand**, including their known limits: planimetric geometry cannot distinguish a mountain switchback pair from a dual carriageway (blind-spots §2.1) — though in central Belgrade, at 15–25 m lateral offsets, the switchback reading is not available as an explanation.
10. **`Loop.from_serving_route` could not be used.** It expects `paths[0].points` (3D polyline, 1e5) from the retired app DTO; contract 3.x serves `candidates[].geometry` (2D polyline6). The scratch script re-implements the same rule (decode → 1e-5 grid → dedup → farthest-point seam) and hands the harness's own `Loop` constructor the result; the harness was not edited. **This is a harness bug worth a ticket** — serving-mode ingestion is currently dead against production.

## 7. Hygiene and the ROPA line

Published resolution is the ledger's own: **0.001° cells (~111 m)**, never a request coordinate, and **no timestamp is attached to any cell** (created-at ranges are quoted for the cache as a whole, not per cell). Loop geometries are metered but never plotted or quoted; the extremes table names cells, not streets. The corpus stores cell centres, which are cell-resolution by construction.

The copy stays under `~/.curvagen-scratch/prod-cache/` (mode 0700) until the census closes — the served-loop reading and corpus-v2 are derived from it and the census may need to re-read it. **Deletion happens at census close** (curvagen-valhalla#9 records the date; the ticket asks for it to be said on the ticket too).

A ROPA entry is drafted — **not applied** — at `~/.curvagen-scratch/prod-cache/ropa.diff` (unified diff against `curvagen-orchestrator/docs/compliance/ropa.md`, appending a second processing activity: purpose *route-quality analysis of the serving cache*, data = cached candidate geometries + demand cells, recipients none, transfers none, retention *deleted at census close*, Art 32 = mode-0700 read-only copy, cell-level publication). One honest gap is marked `TBD(Andrey)` in the draft: **the ROPA has no standalone entry for the serving cache itself**, so "legal basis mirrors the serving-cache entry" has nothing to point at yet — the draft states the mirror in words (technical component of delivering the requested route; the Art 6(1)(a) consent basis for rider data does not extend to it and is not relied on) and flags that it should become a pointer once the serving cache is recorded.

## 8. What the census inherits

- **Corpus:** `tools/loopqual/corpus-v2.yaml`, id `corpus-v2`, sha256 `bf40ee110d001a0f69f5b74b32db0112523d00312a8e8c7ab69bc8bf5433c83a`, **552 requests × K=12 = 6 624 loops**. Untracked; commit it (or not) on Andrey's word.
- **Demand cut:** cells with ≥ 3 requests minus simulator/transposed artifacts = **8 Belgrade cells, 144/163 requests (88 %)**.
- **Engine run line** (prod-equivalent, arm64, `b4f514d7f`; never touch `valhalla-local` on :8002):

  ```bash
  docker run -d --name rt-census-engine -p 8003:8003 \
    -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
    -e use_tiles_ignore_pbf=True -e serve_tiles=True -e server_threads=2 \
    valhalla-curvature:prod-equivalent-b4f514d7f \
    bash -lc "sed 's|tcp://\*:8002|tcp://*:8003|' /custom_files/valhalla.json > /tmp/valhalla-8003.json && exec valhalla_service /tmp/valhalla-8003.json 2"
  curl -s localhost:8003/status   # {"version":"3.8.2","tileset_last_modified":1785932567,…}
  ```

- **Detector commands:** the D1–D5 prototypes in `~/.curvagen-scratch/` (`lqbs_lib.py`, `lqbs_measure.py`, `lqbs_rings.py`, `lqbs_xing.py`, `lqbs_stemgap.py`, `lqbs_lobe.py`) run unchanged against a corpus-v2 result directory — `python3 lqbs_measure.py <run_dir> out.jsonl`, etc. (blind-spots Appendix A). For serving-mode input, use `~/.curvagen-scratch/prod-cache/meter_served.py` as the pattern for contract-3.x ingestion.
- **Baselines to compare against:** this reading (production, 111 loops, serving mode) and `results/b1-x02` (corpus-v1, 2 779 loops, engine mode). The `belgrade` slice of the latter is the closest terrain match to production demand.
- **Two harness bugs to fix before or during the run:** `runner.py::build_request` is stale against contract 3.7.1 (`startPoint` array vs object — audit-rig §8.5) and `Loop.from_serving_route` is stale against contract 3.x geometry (§6.10). Neither blocks an engine-mode census.

---

## Appendix A — method, exact commands

Production access (one session, read-only; box and key from the private deploy repo):

```bash
# inspection
ssh -i ~/.ssh/lazark0x_ssh_key root@<box> 'uptime; df -h /; docker ps; docker volume ls; command -v sqlite3'
ssh -i ~/.ssh/lazark0x_ssh_key root@<box> 'docker volume inspect curvagen_cache-data --format "{{.Mountpoint}}";
  ls -la /var/lib/docker/volumes/curvagen_cache-data/_data/; docker inspect curvagen-api --format "{{json .Mounts}}"'

# the one copy — db + wal + shm in a single rsync so the WAL matches its database
rsync -av --stats -e "ssh -i ~/.ssh/lazark0x_ssh_key" \
  'root@<box>:/var/lib/docker/volumes/curvagen_cache-data/_data/cache.db*' \
  ~/.curvagen-scratch/prod-cache/
```

Offline (all under `~/.curvagen-scratch/prod-cache/`, `python3` = system CPython 3.14):

```bash
mkdir -p work && cp cache.db cache.db-wal cache.db-shm work/   # pristine copy stays untouched
sqlite3 work/cache.db "PRAGMA integrity_check; PRAGMA journal_mode;"   # ok / wal — replays the WAL
sqlite3 work/cache.db ".schema"

python3 demand.py                 # origin_stats decode: cells x bands x hits/misses
python3 demand2.py                # marginals, cut analysis, per-cell band table
python3 meter_served.py served.jsonl   # 111 loops: metrics v1.3 analyze_loop + D1-D6
python3 agg_served.py             # tables: ALL / source-rank / cell / distance / curviness / generation
python3 compare_det.py            # the D1-D6 detector table vs b1-x02 and its belgrade slice
python3 locate.py                 # /locate snap check for all 17 corpus-v2 origins (engine on :8003)

# corpus validation through the harness's own loader (read-only import)
tools/loopqual/venv/bin/python3 -c "…runner.load_corpus / enumerate_jobs / job_filename…"
```

`meter_served.py` imports `tools/loopqual/metrics.py` and `~/.curvagen-scratch/lqbs_lib.py` read-only and calls `metrics.analyze_loop`, `L.antimirror_runs`, `L.exempt_stats`, `L.raw_stem`, `L.near_rejoin_rings`, `L.self_intersections`, `L.distance_peaks`, plus a verbatim port of `lqbs_stemgap.py::prefix_len`. Its only original code is the contract-3.x ingestion (polyline6 2D → 1e-5 grid → farthest-point seam → `metrics.Loop`) and zstd decompression through the `zstd` CLI (no Python zstd binding on this rig). `PARAMS` is mutated only inside `L.raw_stem`, which restores it in a `finally` — no file under `tools/loopqual/` was written.

Engine container: started as `rt-corpusv2-locate` on :8003 with the run line in §8, used for `/locate` only, then `docker rm -f rt-corpusv2-locate`. `valhalla-local` (:8002) was never addressed.

## Appendix B — artifacts

| Path | What |
|---|---|
| `~/.curvagen-scratch/prod-cache/cache.db{,-wal,-shm}` | the pristine read-only copy (delete at census close) |
| `~/.curvagen-scratch/prod-cache/work/cache.db` | WAL-replayed working copy, read via `mode=ro` |
| `~/.curvagen-scratch/prod-cache/COPY-PROVENANCE.txt` | rsync timestamps + transfer stats |
| `~/.curvagen-scratch/prod-cache/{demand,demand2,meter_served,agg_served,compare_det,locate}.py` | the scripts above |
| `~/.curvagen-scratch/prod-cache/served.jsonl` | 111 per-loop metric records |
| `~/.curvagen-scratch/prod-cache/{demand,agg,det,locate}.txt` | raw outputs backing §3 and §5 |
| `~/.curvagen-scratch/prod-cache/ropa.diff` | the ROPA draft (**not applied**) |
| `tools/loopqual/corpus-v2.yaml` | the census corpus (untracked) |

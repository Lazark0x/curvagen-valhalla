# Round Trip on AU/NZ: engine time, memory, served lengths and the curvature bucket histogram

- **Purpose:** measure Round Trip on the first AU/NZ tileset before it reaches a box (curvagen-valhalla#22), so the Sydney box's size (curvagen-ios#437) and the engine's compose memory limit read measured numbers, and see what the sparse outback cells do.
- **Scope:** measurement only. No engine change, no deploy.
- **Engine, held fixed:** the fork at `7b21ef2ca` (`server-waves-2026-11-06`, the build that made the tiles), image `s-meta-12-valhalla:7b21ef2ca` (arm64, local). Production `valhalla.json` byte for byte (a copy of the box's file, sha256 `735d7f44…7450`), `valhalla_service … 2` (prod's `VALHALLA_THREADS`), `--cpus 2`, `--memory 5g` without swap, tar mmapped from a read-only bind.
- **Tilesets:** AU/NZ `aunz-osm20261104` (meta#12: 1.69 GiB tar, 11,048 tiles, curvature metric 1). The Balkans arm is a byte copy of the tar production serves today (July 2026, 364 tiles, `/Users/xenix/Projects/curvagen-tiles-391/baseline/`).
- **Requests:** the orchestrator's own engine request (`crates/engine-client` `trip_request`, costing from `crates/domain` `costing()`, ported verbatim in `tools/loopqual/runner.py`): K = 3 (`SERVE_K`; Bank Fill is off), motorways allowed, the app's distances (slider 10–300 km: the 50 km default, 100, 200, 300).

## Summary

- **Memory, the number for #437 and the compose limit.** At prod's two engine threads, AU/NZ needs about **2.8× the Balkans' anonymous memory and 3.5× its cgroup total**:

  | | Balkans (prod tar) | AU/NZ | ratio |
  |---|---|---|---|
  | start to ready (road-identity sidecar reads every tile) | 6.4 s | 27.6 s | 4.3× |
  | anon / file after start | 178 / 349 MiB | 250 / 1,697 MiB | 1.4× / 4.9× |
  | anon peak during start | 239 MiB | 735 MiB | 3.1× |
  | anon high-water, one request at a time | 610 MiB | 1,549 MiB | 2.5× |
  | **anon peak, 3 requests in flight (`ENGINE_CONCURRENCY=3`)** | **677 MiB** | **1,881 MiB** | **2.8×** |
  | **cgroup peak (`memory.peak`, anon + page cache)** | **1,036 MiB** | **3,599 MiB** | **3.5×** |

  The whole tar sits in page cache from start on: the sidecar build reads every tile. Anonymous memory does not come back after a request (the thor workers keep their reservations, `clear_reserved_memory: false`), so a busy engine sits at its high-water mark, and that mark grows **per worker thread**: two identical asks in a row raise it twice, once per thread. That makes threads the multiplier for a bigger box (section 5).
- **Time.** AU/NZ is not slower per request overall: one-at-a-time p50 0.23 s against 0.36 s, because the outback answers in under 0.2 s. The tail is the cities: **Melbourne** (1.6 M harvest labels) takes 5.2–6.2 s at 100–300 km, Sydney 3.4 s at 300 km, against Belgrade's 2.5 s. **Broome 300 km takes 5–7 s on 7 k labels**, all of it rescue builds, and is the request that set the anon high-water mark (+486 MiB). With 3 in flight, AU p90 is 6.0 s and the worst request 14.3 s (Broome 300 km); Balkans p90 is 2.2 s and its worst 4.8 s. Times are this laptop's (M1 Pro cores); the ratios are what carries to a box.
- **Served lengths: the sparse cells are not usable as served today.** On AU/NZ **20.4 % of served loops (99 of 486) miss the target by more than half, 60 are at least twice it, and slot 0 misses by more than half on 10 of 54 cells** (Katherine 100 km serves **1,127 km** first). On the Balkans control: 0 of 252. **96 of those 99 loops are tier 0 and not `gated`**, so nothing in the engine answer tells the orchestrator. Queenstown at c0.5 answers **442 at 200 and 300 km** (no turnaround candidates at all, 60 ms). Details and repro bodies: `notes/valhalla-22-findings.md` for valhalla#23.
- **The bucket histogram.** The fork's 4-bit curvature bucket (`compute_curvature`, `kFullyCurvyDegPerKm = 350`, calibrated on the Serbia histogram) puts **3.8 % of AU/NZ main-road length in the curvy buckets (≥ 9, the costing's `kCurvyBucketThreshold`) against 22.5 % on the Balkans**, mean bucket 1.47 against 4.34. NZ alone is 11.4 %, Wellington, Cairns and Hobart 12–13 % (Belgrade's 15 %). The Stuart Highway cells are 0.2 %. Part of the gap is the metric, not the roads: AU main-road edges are 5× longer (length-weighted median 2.1 km against 0.39 km), and the bucket is a per-edge turning density, so a 2 km twisty stretch inside a 20 km edge reads straight.

## 1. Corpus

`tools/loopqual/corpus-aunz-v1.yaml`, 168 requests: 11 origins at city level (0.01°) × 50/100/200/300 km × seeds 7/11/23 at c0.5, plus c1.0 at Sydney, Queenstown and Alice Springs.

- Launch cities: Sydney (the smoke anchor), Melbourne, Perth, Queenstown.
- The Stuart Highway corridor: Alice Springs, Tennant Creek, Katherine.
- Shapes with no Balkans analogue: Broome (one highway along an empty coast), Hobart (an island), Cairns (a coastal strip under a range), Wellington (sea on three sides).

Control: `tools/loopqual/corpus-balkans-k3-v1.yaml`, 84 requests in the same shape on Vračar, Zlatibor, Djerdap, Sarajevo and Mostar.

**Seeds barely vary the answer.** Seeds 7, 11 and 23 (carried over from the older corpora) serve the identical bank in 48 of 54 AU cells and 21 of 28 Balkans cells: the seed rotation is `surv[(seed + b) % surv.size()]`, and odd seeds collide on short survivor lists. So the corpus measures about one ask per cell, three times. Memory and time are unaffected (the same ask does the same work), but per-cell variety is not measured here. A Shuffle in production draws a random seed.

## 2. Method

`tools/loopqual/engine_memory.py` runs inside the Docker VM in its own container, sharing the engine's network namespace and reading the engine's cgroup (`--cgroupns host`), so the sampler and the request clock are the same clock.

- It samples `memory.current` and `memory.stat` (anon, file) every 20 ms and fires the jobs, either one at a time with 1 s between requests or 3 in parallel (prod's `ENGINE_CONCURRENCY`; with 2 engine threads the third waits in the engine's queue, as on the box).
- Per request it records wall time, status, served lengths, the engine's `provenance` per loop, and the peak anon during the request. It writes loopqual's own response records, so `loopqual run` on the same `--out` adds every loop metric.
- Each phase gets a fresh engine. `startup` samples from container start until `/status` answers.
- The kernel (6.8) cannot reset `memory.peak`, so per-phase peaks are the sampled maxima plus the container's lifetime `memory.peak`.
- The engine ledger (`roundtrip pair-select`, `gate`, `rungs`, `pair-leg`) is joined to the requests in order for the one-at-a-time phase.

Everything ran under the colima lock on a quiet VM (4 vCPU, 8 GiB, shared with idle containers only). Raw output: `tools/loopqual/results/s22/{aunz,balkans}-{single,parallel}/` on the build laptop (gitignored).

## 3. Per cell (c0.5 unless marked; seed 7; wall time is the median of the 3 seeds)

Builders: p = pair, r = rescue. Tier and `g` (gated) per slot. "Overlap" is the bank's largest pair overlap (loopqual `max_pair_overlap`). Rescue is attempts/loops from the ledger; "stash" is the gate's dirty-loop stash.

| cell | wall s | served km | × target | builders | tier | overlap | rescue | stash | harvest labels |
|---|---|---|---|---|---|---|---|---|---|
| sydney 50 | 1.27 | 54/49/56 | 1.1/1.0/1.1 | ppp | 0 0 0 | 0.21 | 0/0 | 0 | 389 k |
| sydney 100 | 2.43 | 111/96/113 | 1.1/1.0/1.1 | ppp | 0 0 0 | 0.17 | 0/0 | 0 | 732 k |
| sydney 200 | 3.10 | 259/186/180 | 1.3/0.9/0.9 | ppp | 0 0 0 | 0.38 | 0/0 | 0 | 896 k |
| sydney 300 | 3.43 | 267/281/342 | 0.9/0.9/1.1 | ppp | 0 0 0 | 0.45 | 0/0 | 0 | 946 k |
| melbourne 50 | 2.40 | 48/57/42 | 1.0/1.1/0.8 | ppp | 0 0 0 | 0.01 | 0/0 | 0 | 767 k |
| melbourne 100 | 5.18 | 120/94/100 | 1.2/0.9/1.0 | ppp | 0 0 0 | 0.41 | 0/0 | 0 | 1,317 k |
| melbourne 200 | 5.40 | 186/161/192 | 0.9/0.8/1.0 | ppp | 0 0 0 | 0.29 | 0/0 | 0 | 1,543 k |
| melbourne 300 | 5.67 | 333/280/278 | 1.1/0.9/0.9 | ppp | 0 0 0 | 0.30 | 0/0 | 0 | 1,599 k |
| perth 50 | 1.58 | 40/43/54 | 0.8/0.9/1.1 | ppp | 0 0 0 | 0.30 | 0/0 | 0 | 444 k |
| perth 100 | 2.16 | 87/108/85 | 0.9/1.1/0.9 | ppp | 0 0 0 | 0.17 | 0/0 | 0 | 617 k |
| perth 200 | 2.45 | 201/171/169 | 1.0/0.9/0.8 | prr | 0 0 0 | 0.36 | 2/2 | 0 | 681 k |
| perth 300 | 2.20 | 309/293/339 | 1.0/1.0/1.1 | ppr | 0 0 2 | 0.25 | 1/1 | 0 | 695 k |
| queenstown 50 | 0.12 | 43/62/72 | 0.9/1.2/1.4 | prr | 0 3g 3g | 0.86 | 4/2 | 2 | 16 k |
| queenstown 100 | 0.22 | 94/77/73 | 0.9/0.8/0.7 | rrr | 3g 3g 3g | 0.94 | 12/2 | 3 | 20 k |
| queenstown 200 | 0.06 | **442** | | | | | | | 41 k |
| queenstown 300 | 0.06 | **442** | | | | | | | |
| alice 50 | 0.06 | 58/59/40 | 1.2/1.2/0.8 | ppr | 0 0 3g | 0.50 | 2/1 | 2 | 11 k |
| alice 100 | 0.04 | 107/109/165 | 1.1/1.1/1.6 | ppp | 0 0 0 | 0.54 | 0/0 | 0 | 12 k |
| alice 200 | 0.05 | 255/165/222 | 1.3/0.8/1.1 | ppp | 0 0 0 | 0.09 | 0/0 | 0 | 14 k |
| alice 300 | 0.07 | 291/437/**907** | 1.0/1.5/**3.0** | prr | 0 0 0 | 0.40 | 2/2 | 0 | 14 k |
| tennant 50 | 0.02 | 41/53/20 | 0.8/1.1/0.4 | ppr | 0 0 0 | 0.07 | 1/1 | 0 | 2 k |
| tennant 100 | 0.04 | 112/110/96 | 1.1/1.1/1.0 | ppp | 0 0 0 | 0.38 | 0/0 | 0 | 3 k |
| tennant 200 | 0.08 | 271/681/1,311 | 1.4/3.4/**6.6** | rrr | 0 0 0 | 0.64 | 3/3 | 0 | 3 k |
| tennant 300 | 0.16 | 541/1,258/1,420 | **1.8/4.2/4.7** | rrr | 0 0 0 | 0.37 | 4/3 | 1 | 3 k |
| katherine 50 | 0.09 | 131/378/38 | **2.6/7.6**/0.8 | rrr | 0 0 3g | 0.74 | 14/2 | 3 | 5 k |
| katherine 100 | 0.13 | **1,127**/200/145 | **11.3**/2.0/1.5 | rrr | 0 3g 3g | 0.79 | 12/3 | 3 | 6 k |
| katherine 200 | 0.11 | **1,137**/174/157 | **5.7**/0.9/0.8 | prr | 0 3g 3g | 0.93 | 4/2 | 2 | 7 k |
| katherine 300 | 0.19 | 1,115/1,104/1,106 | **3.7/3.7/3.7** | rrr | 0 0 0 | **1.00** | 8/3 | 1 | 7 k |
| broome 50 | 0.04 | 49/41/47 | 1.0/0.8/0.9 | prr | 0 3g 3g | 0.82 | 3/2 | 2 | 6 k |
| broome 100 | 0.04 | 377/86/101 | **3.8**/0.9/1.0 | rrr | 0 3g 3g | 0.93 | 5/3 | 2 | 7 k |
| broome 200 | 0.13 | 362/360/180 | 1.8/1.8/0.9 | rrr | 0 0 3g | 0.97 | 10/3 | 3 | 7 k |
| broome 300 | **5.85** | 406/263/336 | 1.4/0.9/1.1 | rrr | 0 3g 3g | 0.76 | 6/3 | 3 | 7 k |
| hobart 50 | 0.22 | 51/58/60 | 1.0/1.2/1.2 | ppp | 0 0 0 | 0.19 | 0/0 | 0 | 97 k |
| hobart 100 | 0.30 | 93/94/108 | 0.9/0.9/1.1 | ppp | 0 0 0 | 0.17 | 0/0 | 0 | 148 k |
| hobart 200 | 0.35 | 204/179/165 | 1.0/0.9/0.8 | ppp | 0 0 0 | 0.11 | 0/0 | 0 | 174 k |
| hobart 300 | 0.43 | 261/243/323 | 0.9/0.8/1.1 | ppp | 0 0 0 | 0.62 | 0/0 | 0 | 177 k |
| cairns 50 | 0.27 | 174/52/57 | **3.5**/1.0/1.1 | rrr | 0 3g 3g | 0.65 | 7/3 | 3 | 43 k |
| cairns 100 | 0.17 | 165/176/214 | 1.7/1.8/2.1 | prr | 0 0 0 | 0.88 | 2/2 | 0 | 47 k |
| cairns 200 | 0.31 | 174/195/205 | 0.9/1.0/1.0 | ppp | 0 0 0 | 0.43 | 0/0 | 0 | 67 k |
| cairns 300 | 0.24 | 261/306/281 | 0.9/1.0/0.9 | ppr | 0 0 0 | 0.37 | 1/1 | 0 | 70 k |
| wellington 50 | 0.24 | 54/56/58 | 1.1/1.1/1.2 | ppp | 0 0 0 | 0.51 | 0/0 | 0 | 96 k |
| wellington 100 | 0.36 | 112/113/81 | 1.1/1.1/0.8 | ppp | 0 0 0 | 0.51 | 0/0 | 0 | 123 k |
| wellington 200 | 0.37 | 190/186/349 | 0.9/0.9/1.7 | ppr | 0 0 0 | 0.96 | 1/1 | 1 | 141 k |
| wellington 300 | 0.88 | 344/458/450 | 1.1/1.5/1.5 | prr | 0 0 0 | 0.87 | 3/2 | 1 | 156 k |
| sydney c1.0 300 | 2.65 | 245/332/257 | 0.8/1.1/0.9 | ppr | 0 0 0 | 0.70 | 1/1 | 0 | 890 k |
| queenstown c1.0 200 | 0.23 | 218/393/444 | 1.1/2.0/2.2 | rrr | 0 0 0 | 0.70 | 3/3 | 0 | 37 k |
| queenstown c1.0 300 | 0.47 | 1,120/1,122/1,367 | **3.7/3.7/4.6** | rrr | 0 0 0 | 0.94 | 3/3 | 0 | 43 k |
| alice c1.0 300 | 0.09 | 291/890/456 | 1.0/3.0/1.5 | prr | 0 0 0 | 0.21 | 2/2 | 0 | 14 k |

The remaining c1.0 rows (Sydney, Queenstown and Alice at 50–200 km) read like their c0.5 rows.

**Balkans control** (same shape): every request served, all 252 loops within 0.5× of the target, the worst 1.4×; 18 rescue loops and 3 gated. Wall times per distance, p50 / max: 50 km 0.12 / 0.55 s, 100 km 0.22 / 0.86 s, 200 km 0.35 / 1.65 s, 300 km 0.81 / 2.53 s (Vračar 300 km: 717 k labels, 2.5 s). AU/NZ per distance: 0.19 / 2.61, 0.19 / 5.24, 0.31 / 5.73, 0.47 / 7.02 s.

loopqual meters (metrics v2, `results/s22/*-single/report.md`): no spike of 30 m or more and no lollipop on either tileset. Distance error mean at c0.5 is **59.7 % on AU/NZ** against 12.0 % on the Balkans. By origin it is 8.7–11.0 % in Melbourne, Perth, Sydney and Hobart, and 277 % in Katherine, 143 % in Tennant Creek, 47 % in Broome and Cairns. Edge reuse (geometric) is 0.47 in Broome and 0.51 in Queenstown, where every Balkans origin reads 0–0.001.

## 4. The curvature bucket histogram

`tools/loopqual/tile_buckets.py` reads every DirectedEdge of a tar (headers and bitfields as `graphtileheader.h` and `directededge.h` lay them out; the edge count reproduces the 3,692,634 the 2026-11-04 research read from the same Balkans tar). It counts one direction per road, no shortcuts, motorcycle-accessible road uses, length-weighted. "Main roads" is motorway through tertiary, the network a loop rides; "near X" is every edge whose start node lies within 150 km of the origin (a 300 km loop's turnaround band).

| main roads (motorway–tertiary) | km | mean bucket | b0 | b1–4 | b5–8 | **b9–15 (curvy)** | curvy km | all roads: mean / curvy share |
|---|---|---|---|---|---|---|---|---|
| **AU/NZ whole** | 367,226 | **1.47** | 50.9 % | 40.2 % | 5.1 % | **3.8 %** | 14,051 | 3.84 / 18.4 % |
| AU (lon < 165) | 335,200 | 1.32 | 51.9 % | 40.6 % | 4.4 % | 3.1 % | 10,393 | 3.46 / 15.8 % |
| NZ | 32,026 | 2.98 | 40.4 % | 36.1 % | 12.1 % | 11.4 % | 3,658 | 6.84 / 40.0 % |
| near Sydney | 15,284 | 2.03 | 55.3 % | 29.0 % | 8.7 % | 7.1 % | 1,078 | 5.16 / 29.7 % |
| near Melbourne | 24,596 | 1.66 | 61.9 % | 26.5 % | 5.2 % | 6.3 % | 1,561 | 4.45 / 25.4 % |
| near Perth | 12,458 | 1.06 | 60.7 % | 33.4 % | 3.7 % | 2.2 % | 274 | 3.51 / 16.7 % |
| near Queenstown | 3,971 | 1.94 | 34.4 % | 54.3 % | 8.7 % | 2.7 % | 106 | 6.37 / 35.3 % |
| near Alice Springs | 1,643 | 0.60 | 63.0 % | 35.9 % | 0.9 % | 0.2 % | 3 | 3.24 / 12.4 % |
| near Tennant Creek | 603 | 0.20 | 85.7 % | 13.8 % | 0.2 % | 0.2 % | 1 | 1.45 / 2.9 % |
| near Katherine | 1,004 | 0.79 | 51.6 % | 47.3 % | 0.6 % | 0.5 % | 6 | 2.80 / 7.4 % |
| near Broome | 552 | 0.16 | 92.3 % | 6.9 % | 0.5 % | 0.3 % | 2 | 1.66 / 5.6 % |
| near Hobart | 4,547 | 3.71 | 24.5 % | 43.6 % | 19.6 % | 12.3 % | 557 | 7.17 / 40.5 % |
| near Cairns | 3,139 | 3.42 | 28.8 % | 44.8 % | 13.2 % | 13.3 % | 416 | 6.12 / 31.7 % |
| near Wellington | 3,513 | 3.00 | 47.6 % | 30.1 % | 8.8 % | 13.4 % | 471 | 8.24 / 51.3 % |
| **Balkans whole (prod tar)** | 40,000 | **4.34** | 40.4 % | 23.4 % | 13.7 % | **22.5 %** | 8,994 | 7.86 / 48.8 % |
| near Vračar | 18,340 | 3.08 | 53.1 % | 21.5 % | 10.3 % | 15.1 % | 2,761 | 5.87 / 34.5 % |
| near Zlatibor | 19,291 | 4.77 | 37.8 % | 22.7 % | 13.9 % | 25.6 % | 4,942 | 9.04 / 57.2 % |
| near Djerdap | 10,588 | 3.24 | 48.0 % | 24.9 % | 12.1 % | 15.0 % | 1,586 | 6.88 / 41.1 % |
| near Sarajevo | 16,031 | 5.57 | 27.6 % | 25.5 % | 17.3 % | 29.5 % | 4,731 | 9.91 / 63.4 % |
| near Mostar | 9,666 | 6.47 | 22.8 % | 22.9 % | 18.5 % | 35.7 % | 3,452 | 10.55 / 68.3 % |

The Balkans tar built on 3.9.1 from November OSM (`out/valhalla_tiles.balkans-osm20261103-v391.tar`) reads within 0.06 of the prod tar's mean bucket in every row. JSON for all three tars: `/Users/xenix/Projects/curvagen-tiles-aunz-22/run/buckets-*.json`.

**Edge length, the metric's blind spot.** On main roads, the length-weighted median edge is **2,132 m on AU/NZ against 393 m on the Balkans**, and 28.9 % of AU/NZ main-road length is on edges longer than 5 km (1.4 % on the Balkans). The bucket is the edge's mean turning density, so a long edge averages its bends away. Restricted to edges of 1 km or less, AU/NZ main roads are still only 5.0 % curvy against 16.9 %: the roads really are straighter, and long edges dilute what bends there are.

What this means for Round Trip:

- **Outback cells have nothing for `prefer_curvature` to choose.** Around Tennant Creek, Broome and Katherine, 1–6 km of main road is curvy. The harvest's curviness-per-km ranking (`roundtrip_expansion.cc`, the same bucket) has no signal there, and the serves above come from what the network allows, not from curvature.
- **Sydney, Melbourne and Perth sit at a third to a half of Belgrade's curvy share.** The coast and range cells (Hobart, Cairns, Wellington) match Belgrade. None reaches Zlatibor or Mostar.
- **Recalibrating `kFullyCurvyDegPerKm` is not this ticket's call.** It changes every tile and every served loop (a new tileset, a FINGERPRINT bump), and the edge-length dilution would survive any rescale. Splitting long edges for the metric, or bucketing the curviest kilometre of an edge instead of its mean, is a mjolnir change for a follow-up ticket, if Andrey wants AU curvature to rank like Serbian curvature.

## 5. Box size and the engine's memory limit

Measured at prod's 2 engine threads (`VALHALLA_THREADS=2`):

- **Anon high-water: 1.55 GiB one at a time, 1.88 GiB with 3 in flight.**
- **Page cache: the whole 1.66 GiB tar**, touched at start.
- **cgroup peak: 3.51 GiB.**

Anonymous memory stays at its high-water mark and grows per worker thread. About 650 MiB per thread above the idle 250 MiB one at a time, and about 815 MiB per thread with 3 in flight.

| engine on | threads | anon (measured / est.) | tar cache | engine total | suggested `VALHALLA_MEM_LIMIT` |
|---|---|---|---|---|---|
| Balkans box today (3.7 GB, 0 swap) | 2 | 0.66 GiB measured (0.79 GiB for a K = 12 300 km ask, 2026-11-04 research) | 0.34 GiB | 1.01 GiB measured | **2g** (default) |
| AU/NZ, 8 GB Sydney box | 2 | 1.84 GiB measured | 1.66 GiB | 3.51 GiB measured | **4g** |
| AU/NZ, 8 GB Sydney box | 4 | ≈ 3.4 GiB, estimated (250 MiB + 4 × 815 MiB) | 1.66 GiB | ≈ 5.1 GiB | **5632m**, re-measured on the box |

- **The 3.7 GB Hetzner box cannot serve AU/NZ.** 3.5 GiB of engine leaves nothing for Postgres (640 MiB cap), the api and the OS. The 8 GB Sydney plan from the 2026-10-15 sizing research is the smallest box this measures as fitting, and at 4 threads it is tight: 5.5 GiB of engine, 640 MiB of Postgres, and about 1.8 GB for the api, nginx, the OS and its page cache.
- **A cap below the measured total does not fail at once.** Page cache is reclaimable, so a capped engine re-reads tiles from disk, which is slower but alive. It fails when **anon** reaches the cap: the cgroup's OOM killer stops the engine, compose restarts it, and it is back after 6 s (Balkans) or 28 s (AU/NZ). That is the same trade the Postgres cap makes: an engine stopped in its own cgroup instead of the host OOM killer picking the api or Postgres.
- **The limit is curvagen-meta's `deploy/docker-compose.yml` `mem_limit: ${VALHALLA_MEM_LIMIT:-2g}`.** The Sydney box's `.env` sets `VALHALLA_MEM_LIMIT` with its tiles. AU/NZ under the 2 GiB default would OOM-loop at its first busy minute, so the tiles and the `.env` line ship in one step.
- **Not measured:** 4 engine threads (no 4-thread box here; the 2-thread VM run is what the per-thread estimate rests on), x86 timing, and memory under real traffic. The first hour on the Sydney box should read `docker stats` and the engine cgroup's `memory.peak`.

## 6. What did not work or was left out

- Queenstown c0.5 at 200 and 300 km is a 442 in 60 ms: the pair pass sees 40 k labels, F01 rejects 25,993 chains, and **no turnaround candidate survives** (`considered=0 chosen=0`). The same origin at c1.0 serves (rescue, 2–4.6× the target). meta#12's first look served Queenstown 300 km with a bare `{prefer_curvature: 0.8}` costing on the build config, so the orchestrator's costing (or prod's config) empties the band; which one was not isolated. It is in the findings for valhalla#23, with its body.
- Seeds 7/11/23 give one bank per cell (section 1). A per-cell variety measurement needs seeds of both parities, or the orchestrator's random Shuffle seeds.
- Elevation was not mounted for the Balkans arm. It does not change routes (elevation is baked into the tiles), only `/height`.

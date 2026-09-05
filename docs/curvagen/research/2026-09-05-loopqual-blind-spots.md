# Gate v1.2 Blind Spots: the Rider-Visible Loop Shapes the loopqual Meters Cannot See

- **Date:** 2026-09-05 (orchestrator ticket [#38](https://github.com/Lazark0x/curvagen-orchestrator/issues/38), wayfinder map #36 — round-trip v4)
- **Scope:** the tension between `results/b1-x02` reading **spikes 0.00 %, lollipop 0.3 %, `edge_reuse_geom` 0.064** and riders still reporting spikes and lollipops. Which rider-visible defect shapes are structurally **invisible**, **undercounted**, or **deliberately tolerated** by metrics v1.3 (`tools/loopqual/metrics.py`) and Gate v1.2 (`tools/loopqual/gate_v1_proto.py`), and what detectors would see them. Input to the *Gate v2* grilling; **not** a proposal to change the pinned harness.
- **Method:** offline only — **zero engine runs, zero requests to any engine or to `api.curvagen.cc`, no container started or stopped.** Every number is computed from the saved raw engine JSON of `tools/loopqual/results/b1-x02/responses/` (232 requests, 2 779 loops, 2026-07-16, engine note `b1v5 xcand 0.2 only` on Valhalla 3.8.2, corpus-v1 sha256 `8c3db58a1fa9…`) and, for contrast, `results/baseline-v1.2-fix44/` (2 602 loops, pre-v3 engine `fix44`). Prototype detectors are standalone Python in `~/.curvagen-scratch/` that **import `tools/loopqual/metrics.py` read-only** (`Loop.from_engine_trip`, `_shared_mask`, `edge_reuse_geom`, `corridor_stats`) — nothing under `tools/loopqual/` was modified; the one runtime override (`PARAMS["start_exemption_m"] = 0.0`, restored in a `finally`) exists only to read the undiscounted meter. Full command lines in Appendix A.

## TL;DR verdict

**Gate v1.2's three headline meters read clean on b1-x02 because they measure three narrow shapes, not the three product defects they are named after. The rider-visible residual is large, and most of it is not in the gate at all.**

1. **`find_spikes` is a palindrome detector, not a retrace detector.** On b1-x02 it reads 0.00 % — yet **32.9 % of the same loops contain a one-way run ≥ 500 m ridden in both directions on *identical* pavement** (25.9 % ≥ 2 km), a mean **7.2 % of every ride** (`baseline` 12.1 %). The worst case in the run, **vlasina / 200 km / c0.5 / s11 / slot 5** (banked at slot 5 — servable through the ε-exploration swap, `crates/api/src/serving/lookup.rs:64-90`), rides a **23.8 km stretch out and back on the same road** (2 288 of 2 290 grid points shared, mean lateral offset 0.01 m) and reports `spike_count = 0`, `max_stub_km = 0`. Gates 1 and 2 cannot fire on it because the retrace has no reversal apex — the loop rides A→B at km 11–35 and B→A at km 127–150, 90 km apart. **The gate credits the v3 engine with a 43.4 % → 0.0 % spike win; the retrace it was built to remove actually fell 68.7 % → 32.9 % (loops with a ≥ 500 m run) — the meter overstates the win by ~2×.**
2. **Near-mirror retrace (dual carriageways, parallel streets) is invisible to *every* meter.** At a 25 m corridor radius, **32.5 % of b1-x02 loops carry ≥ 500 m** — and 8.8 % carry ≥ 2 km — of opposite-direction riding whose grid keys never repeat, so `edge_reuse_geom` reads it as **zero**. It is also the one class the v3 engine did **not** move: baseline 41.0 % → b1-x02 32.5 % at the ≥ 500 m bar, and *mean unseen metres 1 300 → 1 217 (−6 %)*, because hard exclusion works on edge identity. Named case: **belgrade / 100 km / c0.5 / s101 / slot 6** — `edge_reuse_geom 0.0000`, `spike_count 0`, `is_lollipop False`, while riding 4.41 km out (km 13.9–18.3) and 4.41 km back (km 100.3–104.7) on the two carriageways of one divided road **14.0 m apart**, plus a second 2.26 km pair. Same physical road recurs across seeds and distances.
3. **The product's Lollipop has no detector at all.** `lollipop_stem_fraction` measures a *start stem* (leg0 prefix + leg1 suffix); the product Lollipop (curvagen-meta `CONTEXT.md:93-95`) is a **mid-route ring**. Prototyping a near-rejoin detector: **13.7 % of b1-x02 loops contain a ring inside a single leg** (6.0 % ≥ 1.5 km), **24.8 % contain a bulb-class ring ≥ 2 km** (ring ≤ 25 % of the loop), and **62.1 % of loops are not simple closed curves at all** — they contain at least one transversal self-crossing. The gate reads `is_lollipop` **0.3 %**. Textbook case: **nis / 20 km / c0.5 / s23 / slot 8** — a 1 678 m ring at km 6.8 **inside leg0**, rejoin gap **0.0 m**, on a 20.7 km ride; every gated meter reads 0.
4. **The stem meter is one parameter away from a 27× different reading.** `PARAMS["lollipop_gap_m"] = 120` (`metrics.py:60`) breaks the prefix walk at the first unshared gap > 120 m. Re-running the identical formula at a 1 000 m gap moves `is_lollipop` from **0.29 % to 7.92 %**; **11.7 % of loops have their stem zeroed by the 120 m gap while carrying ≥ 1.5 km of shared corridor** that a 1 km gap sees. Named case: **belacrkva / 50 km / c0.8 / s7 / slot 5** — a 50.3 km "loop" that is **96 % ridden twice** (`edge_reuse_geom 0.9202`, `compactness 0.0002`) reports `stem_frac 0.0000` because leg0 diverges from the corridor for 429 m between km 0.148 and 0.577; the 21.5 km of shared corridor that starts at km 0.595 is never reached by the walk.
5. **The Start Exemption is a deliberate, documented tolerance — and on the worst cells it hides a quarter of the ride.** Inside the exempt 1.5 km at each end, b1-x02 carries a mean **857 m** of cross-leg shared corridor (max **2 995 m** = the cap), and the discount hides a mean **549 m** of reuse (14.8 % of loops ≥ 1 km). At **20 km targets, 26.7 % of loops** spend ≥ 5 % of the whole ride on exempt-zone out-and-back. Named cell: **vlasina / 20 km / c0.5 / all five seeds** — an 11.4 km loop with **2 995 m (26.4 % of the ride)** of exempt out-and-back; the gate reads `stem_frac 0.1253` where the undiscounted formula reads **0.390**.
6. **Fallback Loops and Second Via builds are structurally uncountable from a response.** `curvagen-orchestrator/CONTEXT.md:114-117` says a Fallback Loop is "Tagged in responses, counted by the harness" — **it is not.** The tag lives on the fork's internal `Loop` struct (`src/thor/route_action.cc:1732`, `:1735`) and is only `LOG_INFO`'d (`:2127`, `:2130`); there is no field for it in `proto/descriptors/trip.proto`, and grepping all 232 saved responses for `fallback` / `second_via` / `dirty` / `stage` returns **0 files**. A Second Via build is additionally spliced into a single return leg *by design* (`route_action.cc:1950-1966`) so the 2-leg contract holds — all 2 779 loops present exactly 2 legs and 3 locations. Best geometric proxies: reuse outside the exemption ≥ 100 m fires on **35.2 %** of loops (a lower bound — the A2 ledger measured ~8.4 fallbacks per 12-candidate request at 20–50 km), and two prominent distance lobes on **3.0 %** (9.4 % under relaxed prominence).
7. **Four of the five `corridor_stats` outputs are metered but ungated.** `shadow_frac`, `bulb_count`, `stem_out_m`/`stem_back_m`, `rejoin_return_frac`, `compactness`, `max_stub_km`, `spike_len_fraction`, `edge_reuse_way` and every `common_trunk_frac_*` appear in `loops.jsonl` and in no gate. `shadow_frac ≥ 0.30` fires on **10.9 %** of b1-x02 loops and **5.7 % of all loops carry `shadow_frac ≥ 0.30` with `lollipop_stem_fraction == 0`** — the shape is measured, printed, and ignored.
8. **Served slots are not cleaner than the bank.** Across every prototype detector, slots 0–2 (`SERVE_K = 3`, `crates/api/src/serving/mod.rs:38`) track the K=12 aggregate within a few points and are *worse* on three of them (retrace ≥ 500 m 34.2 % vs 32.5 %; exempt-zone reuse 15.2 % vs 14.6 %; seam-not-farthest 43.7 % vs 41.4 %). Banking serves the same defect density the corpus average shows — the atlas's §9 finding, reconfirmed under the new detectors.

---

## 1. What Gate v1.2 actually reads

`gate_v1_proto.py` has eleven numbered gates. Only **five per-loop metrics** enter them.

| Gate | Reads | Threshold (v1.2 locked) | Shape it can see |
|---|---|---|---|
| 1 | `spike_ge_500m` (`:62-64`) | `= 0` | exact-mirror palindrome ≥ 500 m |
| 2 | `spike_ge_30m` (`:65-67`) | ≤ 2 % | exact-mirror palindrome ≥ 30 m |
| 3/4 | `edge_reuse_geom` mean (`:72-81`) | ≤ 0.12 @20 km, ≤ 0.10 @50, ≤ 0.05 @≥100 | identical undirected grid segments, **outside** the exemption |
| 5 | `edge_reuse_geom > 0.30` (`:84-87`) | ≤ 8 % | as above, tail |
| 6a/6b | `is_lollipop` (`:90-103`) | ≤ 2 % overall, ≤ 55 % worst cell | leg0-prefix / leg1-suffix corridor **stem only** |
| 7 | `distance_error` (`:106-117`) | mean ≤ 0.22/0.32, p90 ≤ 0.42/0.65 | requested-vs-served length |
| 8 | `curviness_geom_clean` (`:120-124`) | ≥ 0.95× base | despiked heading change per km |
| 8b | ordering c0.8 > c0.5 (`:142-151`) | **advisory** | — |
| 9 | `max_pair_overlap` (`:130-140`) | ≤ 1.02× base, near-dup ≤ base+2 pp | cross-candidate edge sharing |
| 10/11 | latency, failures (`:153-163`) | ≤ 1.10×, ≤ base | — |

**Metered in `loops.jsonl` and read by no gate:** `max_stub_km`, `spike_len_fraction`, `n_spikes_{seam_uturn,seam_wrapped,mid_fwd,mid_ret}`, `seam_uturn`, `edge_reuse_way`, `lollipop_stem_fraction` (only its boolean), `stem_out_m`, `stem_back_m`, **`shadow_frac`**, `rejoin_return_frac`, **`bulb_count`**, **`compactness`**, `curviness_retention`, `max_pair_overlap_raw`, `best_twin_sep_m`, `common_trunk_frac_{25,33,50,75}`, `seam_frac`.

Three of the ungated ones are exactly the ones that see the defects below: `shadow_frac` (`metrics.py:462`), `bulb_count` (`:464-474`), `compactness` (`:536`).

---

## 2. H1 — the spike meter is a palindrome meter

**Verdict: BLIND to non-palindromic retrace; UNDERCOUNTS near-mirror retrace, which is invisible to `edge_reuse_geom` as well.**

`find_spikes` (`metrics.py:254-318`) requires a reversal apex `pts[i-1] == pts[i+1]`, extended while `pts[i-w] == pts[i+w]`. Two shapes escape it:

- **(a) exact-pavement retrace with no apex** — the loop rides a road A→B and later rides B→A, separated by kilometres of other riding. No palindrome exists. `edge_reuse_geom` sees it (same undirected keys) *unless* the reuse falls inside the exemption; nothing reports the **length of the longest single retrace run**, which is what the rider actually experiences.
- **(b) near-mirror retrace** — the two traversals are 8–25 m apart (dual carriageway, one-way pair, parallel street). Different grid keys ⇒ `edge_reuse_geom` reads **0**, and there is no apex ⇒ `find_spikes` reads **0**. The harness names this limitation in its own docstring (`metrics.py:266-268`: "dual-carriageway U-turns … surface in the corridor detector instead, so spike counts are a slight undercount") and the atlas repeats it (§3) — **nobody had measured how slight**. The product glossary already covers it: **Backtracking** is "retraces the same *or nearly same* road in the opposite direction" (curvagen-meta `CONTEXT.md:85-87`).

### 2.1 Measured prevalence

Detector D1 (§2.3), r = 10 m (class a — same pavement) and r = 25 m restricted to segments whose grid key does **not** repeat (class b — genuinely unseen):

| reading | baseline-v1.2-fix44 | **b1-x02** |
|---|---|---|
| `spike_ge_30m` (**gate 2**) | 43.9 % (all loops; `report.md`'s c0.5 slice reads 43.4 %) | **0.0 %** |
| one-way same-pavement retrace run ≥ 500 m | 68.7 % | **32.9 %** |
| … ≥ 2 km | 50.3 % | **25.9 %** |
| share of the ride ridden in both directions (mean / p90) | 0.121 / 0.307 | **0.072 / 0.192** |
| near-mirror (r = 25 m) unseen by any meter ≥ 500 m | 41.0 % | **32.5 %** |
| … ≥ 2 km | 9.8 % | **8.8 %** |
| unseen near-mirror mean metres per loop | 1 300 m | **1 217 m** |
| near-mirror U-turn **at the seam**, ≥ 200 m unseen | 2.2 % | **2.7 %** |

By cell on b1-x02 (fires-on %, r = 25 m unseen ≥ 500 m): 20 km 20.6 · 50 km 28.4 · 100 km 29.7 · 200 km 40.6 · 300 km 41.7. By origin: **belgrade 70.1 · novisad 57.2** · zlatibor 35.8 · golija 29.6 · vlasina 27.5 · djerdap 19.8 · nis 15.5 · belacrkva 4.6. Served slots 0–2: 30.6 % vs 33.2 % for slots 3–11.

**Caveat, stated plainly:** on planimetric geometry a mountain **switchback pair** is indistinguishable from a dual-carriageway out-and-back — two anti-parallel arms 20–40 m apart, converging at both ends. The terrain split is the honest disambiguator: the mass of unseen near-mirror sits in **flat terrain** (belgrade 833, novisad 424 of the 2 874 NEW-heavy runs among the four longest runs recorded per loop) where switchbacks do not exist, and the mean lateral offset of those runs peaks at **15–20 m** (1 342 of 2 874; 0–5 m: 156, 5–10 m: 302, 10–15 m: 739, 20–25 m: 335) — the central-reservation band. A production detector should consume elevation (the **serving-mode DTO polyline carries centimetre elevation**, `metrics.py:107-132`) or way-ids (`/trace_attributes`) to settle mountain cases; see §9.

### 2.2 Named examples

Class (a) — same pavement, `spike_count = 0`:

| loop | ride | longest one-way retrace | gate reads |
|---|---|---|---|
| **vlasina / 200 km / c0.5 / s11 / slot 5** | 160.8 km | **23 770 m** (leg0 km 11.1–34.9 ↔ leg1 km 126.6–150.4), 2 288/2 290 identical grid points, mean offset **0.01 m** | spikes **0**, `max_stub_km` **0**, reuse 0.6942, stem 0.0088, `is_lollipop` **False** |
| **belacrkva / 50 km / c0.8 / s7 / slot 5** | 50.3 km | **24 050 m** — **96 % of the ride ridden twice** | spikes **0**, reuse 0.9202, stem **0.0000**, IQ **0.0002** |
| **zlatibor / 300 km / c0.5 / s11 / slot 5** | 299.2 km | **84 288 m** (28 % of the ride) | spikes **0**, reuse 0.2817, stem 0.0000, shadow 0.0058 |
| **golija / 300 km / c0.5 / s101 / slot 0** *(served)* | 282.8 km | **39 530 m** (28 %) | spikes **0**, reuse 0.2804, stem 0.0000 |
| **nis / 300 km / c0.5 / s11 / slot 11** | 315.2 km | 25 025 m (16 %) | spikes **0**, reuse 0.1591, stem 0.0000 |

Class (b) — near-mirror, unseen by **both** spike and reuse meters:

| loop | ride | unseen near-mirror | gate reads |
|---|---|---|---|
| **belgrade / 100 km / c0.5 / s101 / slot 6** | 119.3 km | **24 423 m**; longest run 4 409 m at 44.86306, 20.58080, offset **14.0 m** (leg0 km 13.9–18.3 ↔ leg1 km 100.3–104.7) | `edge_reuse_geom` **0.0000**, spikes **0**, stem **0.0000**; only `shadow_frac 0.2798` sees it — **ungated** |
| **belgrade / 200 km / c0.5 / s42 / slot 6** | 174.5 km | 24 446 m; same 4 409 m carriageway pair | reuse 0.0217, spikes 0, stem 0, shadow 0.2019 |
| **novisad / 200 km / c0.5 / s11 / slot 11** | 219.5 km | **45 937 m** (the corpus max) | reuse **0.0000**, spikes 0, stem 0, shadow 0.3361 |
| **belgrade / 50 km / c0.5 / s101 / slot 1** *(served)* | 52.7 km | 19 262 m; run 4 501 m at offset 11.1 m | reuse **0.0000**, spikes 0, stem 0, shadow 0.4705, IQ 0.048 |
| **novisad / 300 km / c0.5 / s11 / slot 6** | 293.9 km | 21 423 m; longest run 6 153 m, offset 15.7 m | reuse **0.0000**, spikes 0, stem 0 |

### 2.3 Detector proposal — **D1 `retrace_runs`**

> For each directed segment *i* of the loop (midpoint `m_i`, bearing `b_i`, on the existing 1e-5 grid), find the nearest segment *j* with `|i − j| > 4`, `dist(m_i, m_j) ≤ r`, and `|Δbearing − 180°| ≤ 35°`. Merge maximal runs of flagged segments tolerating unflagged gaps ≤ 60 m; report runs ≥ 200 m.
>
> **Outputs:** `retrace_len_m` (flagged length; the rider rides it twice), `retrace_frac = retrace_len_m / total_m`, **`max_retrace_run_m`** (the single longest one-way run — the rider-facing quantity), `retrace_unseen_m` (flagged length whose undirected grid key does *not* repeat, i.e. the part `edge_reuse_geom` cannot see), and a zone split `{exempt, seam, mid}` using the same 1 500 m constant.
> **Parameters:** `r ∈ {10, 25}` (report both: 10 m = same pavement, 25 m = parallel corridor), `ang_tol 35°`, `min_run 200 m`, `gap 60 m`.
> **Cost:** hash grid, **14.5 ms per loop** measured over 250 b1-x02 loops (single core, CPython 3.14) — ~40 s for the whole 2 779-loop corpus; negligible against a 0.96 s p50 engine request.
> **Reads on b1-x02:** `max_retrace_run_m ≥ 500 m` on 32.9 % of loops (baseline 68.7 %); `retrace_unseen_m ≥ 500 m` on 32.5 % (baseline 41.0 %); `retrace_frac` mean 0.072 (baseline 0.121).
> **Suggested gate form:** two blocking rungs replacing the trivially-satisfied gates 1–2 —
> `G1' max_retrace_run_m ≥ 2 000 m` on ≤ *X* % of loops (b1-x02: **25.9 %**, baseline 50.3 % — a real 2× win to ratchet on), and
> `G2' retrace_unseen_m ≥ 2 000 m` on ≤ *Y* % (b1-x02 **8.8 %**, baseline 9.8 % — flat, i.e. the axis v3 never attacked).
> Both must be **relative-to-baseline** ratchets on the first pass; absolute bars need the field census (§9) to say which offsets riders actually mind.

---

## 3. H2 — the Start Exemption: tolerated by design, magnitude never measured

**Verdict: TOLERATED (correctly, per ADR-0037 §3) — but the tolerance is unbounded as a *fraction of the ride*, and on short targets it eats a quarter of it.**

ADR-0037 §3 is explicit and right: a ~1.5 km network-forced stem in a 20 km loop reads 0.15 reuse and ~100 % lollipop *by construction*, so the meters were "billing the algorithm for designed, Andrey-confirmed behavior". `metrics.py:355-356` skips reused segments whose midpoint is within 1 500 m of ride start or ride end; `:454-456` subtracts 1 500 m from each stem. The constant is pinned to the fork's `kStartExemptionMeters` (`route_action.cc:999`).

What the exemption does **not** do is scale with the ride. A 1 500 m + 1 500 m forgiveness is 1.5 % of a 200 km ride and **26 % of an 11 km one**.

### 3.1 Measured on b1-x02

| reading | baseline | **b1-x02** |
|---|---|---|
| cross-leg shared corridor inside the exempt zone (mean / p90 / max) | 992 m / — / 2 995 m | **857 m / 2 995 m / 2 995 m** (2 995 m = the 2 × 1 500 m cap) |
| reuse hidden by the discount (mean) | 719 m | **549 m** |
| loops with ≥ 1 km of hidden reuse | 21.6 % | **14.8 %** |
| loops where the exempt-zone corridor is ≥ 5 % of the ride | 8.3 % | **7.2 %** |
| … ≥ 10 % of the ride | — | **2.4 %** |
| `stem_frac` recomputed with the exemption **off** (mean / > 0.10) | 0.0147 / 3.8 % | **0.0133 / 3.3 %** (gate reads **0.3 %**) |

By distance the tolerance is entirely a short-ride phenomenon: exempt-zone corridor ≥ 5 % of the ride fires on **26.7 % of 20 km loops**, 10.8 % at 50 km, and **0.0 % at ≥ 100 km**. By origin it is one cell: **vlasina 35.9 %**, everything else ≤ 5.5 %. Served slots 0–2 6.6 % vs 7.4 % — no slot protection.

### 3.2 Named examples

| loop | ride | exempt-zone out-and-back | gate reads | undiscounted |
|---|---|---|---|---|
| **vlasina / 20 km / c0.5 / s7, s11, s23, s42, s101 / slots 8–9** (identical shape, all five seeds) | 11.4 km | **2 995 m = 26.4 % of the ride**; hidden reuse 2 995 m | `is_lollipop` **True** (`stem_frac 0.1253`), reuse 0.4924 | `stem_frac` **0.390** — a 3.1× understatement |
| **nis / 20 km** cell | — | 5.2 % of loops ≥ 5 % of ride | `is_lollipop` 0.0 % | — |
| **novisad / 20 km** cell | — | 5.5 % of loops ≥ 5 % of ride | `is_lollipop` 0.0 % | — |

The vlasina cell is the one Gate 6b's 55 % worst-cell allowance was written for (`gate_v1_proto.py:100-103`). It is a correct tolerance of a *network fact*; the finding is that **nobody can currently see how much of the ride it is**, because no metric reports the exempt-zone length.

### 3.3 Detector proposal — **D2 `exempt_zone_load`**

> Report, as a first-class metric, what the exemption forgives: `exempt_corridor_m` (cross-leg shared corridor at 40 m within the first/last 1 500 m of path distance), `exempt_corridor_frac = exempt_corridor_m / total_m`, `reuse_hidden_m = (edge_reuse_geom(0) − edge_reuse_geom(1500)) × total_m`. All three are one extra `_shared_mask` pass plus one extra `edge_reuse_geom` call.
> **Cost:** **12.8 ms per loop** measured (dominated by the existing `_shared_mask`; ~0 marginal if folded into `corridor_stats`).
> **Reads on b1-x02:** mean `exempt_corridor_m` 857 m; `exempt_corridor_frac ≥ 0.05` on 7.2 % of loops (26.7 % at 20 km).
> **Suggested gate form:** keep the exemption absolute for *reuse* (it is a network fact) but gate its **share of the ride**: `exempt_corridor_frac ≤ 0.15` on ≥ 95 % of loops, evaluated **per distance band** (b1-x02: 1.0 % of loops over 0.15 overall; the 20 km band is where it bites). Equivalently: make the exemption `min(1500 m, 0.05 × target)` in metrics v2 and re-ratchet gates 3–6 — that is a **pinned-constant change** and must move `route_action.cc:999` in the same commit (`metrics.py:44-48`, README "change them together").

---

## 4. H3 — mid-route rings: the product Lollipop has no detector

**Verdict: BLIND. The gated `is_lollipop` measures a different shape from the product term it is named after.**

The product **Lollipop** (curvagen-meta `CONTEXT.md:93-95`) is: "departs the main path onto a short loop — usually a small curvy ring the routing engine detoured through to gather Curvature — and rejoins the same path … at nearly the same junction. Distinct from a Spur … and from Backtracking." It is **mid-route** and **ring-shaped**.

The harness's `lollipop_stem_fraction` (`metrics.py:401-481`) measures the maximal **leg0 prefix** within 40 m of leg1 plus the **leg1 suffix** within 40 m of leg0 — i.e. a *start stem*, anchored at index 0 and index −1. A ring at km 24 of a 50 km ride contributes exactly **zero** to it. `bulb_count` counts unshared leg0 runs ≥ 500 m, which is a different quantity again (51.0 % of b1-x02 loops have ≥ 2, 19.8 % have ≥ 3 — ungated). `compactness` "only hints": it is a whole-loop isoperimetric quotient whose own docstring warns it cancels signed area on self-intersecting shapes (`metrics.py:543-547`).

**Both legs are individually cost-optimal** — leg0 is a path in the forward expansion label tree, leg1 a bidirectional-A* path with the forward corridor hard-excluded (`route_action.cc:1587-1594`). A cost-optimal path cannot revisit a node, so **every intra-leg ring found below is a *near*-rejoin between two distinct nodes within the touch radius, and it is cost-optimal under the curvature-discounted costing**: the ring is cheaper than riding straight through, because `prefer_curvature` pays for it. That is precisely the mechanism the product glossary names ("a small curvy ring the routing engine detoured through to gather Curvature") — it is not a bug in the search, it is the costing being taken at its word.

### 4.1 Measured on b1-x02

Detector D3 (§4.3): a self-touch is a pair of points within 40 m separated by ≥ 800 m of riding whose shorter arc has isoperimetric quotient ≥ 0.15 (so out-and-backs, which enclose no area, are rejected); the ride's own start/end closure is excluded by a 1 500 m guard; touches wholly inside the exempt zone are dropped.

| reading | baseline | **b1-x02** |
|---|---|---|
| loops with ≥ 1 near-rejoin ring | 57.0 % | **49.6 %** |
| loops with ≥ 2 | 44.6 % | **37.1 %** |
| bulb-class ring (ring ≤ 25 % of loop) | 38.1 % | **35.2 %** |
| … and ≥ 2 km | 30.0 % | **24.8 %** |
| **ring inside a single leg** (the purest product Lollipop) | — | **13.7 %** (6.0 % ≥ 1.5 km) |
| loops with ≥ 1 transversal self-crossing | 72.6 % | **62.1 %** |
| figure-8 (crossing lobe ≥ 25 % of loop) | 22.0 % | **18.0 %** |
| **gate `is_lollipop`** | 0.4 % | **0.3 %** |

Ring position across b1-x02: 3 896 cross-leg, **741 inside leg0**, **184 inside leg1**. Intra-leg rings by distance: 20 km 5.0 % · 50 km 12.4 % · **100 km 19.7 %** · 200 km 16.1 % · 300 km 15.2 %. Served slots 0–2: **15.2 %** vs 13.3 % for slots 3–11 — *slightly worse on the served slots.* Bulb-class ≥ 2 km by origin: belgrade 46.6 · novisad 39.9 · nis 20.7 · zlatibor 20.2 · belacrkva 20.1 · djerdap 17.8 · golija 17.0 · vlasina 15.7.

### 4.2 Named examples (intra-leg rings — cost-optimal by construction)

| loop | ride | ring | gate reads |
|---|---|---|---|
| **nis / 20 km / c0.5 / s23 / slot 8** | 20.7 km | **1 678 m, IQ 0.68, rejoin gap 0.0 m**, in **leg0** at km 6.8 (seam km 8.4), 43.31889, 21.81736 | spikes 0, reuse **0.0000**, stem **0.0000**, shadow 0.1143 |
| **belgrade / 50 km / c0.5 / s42 / slot 4** | 49.3 km | **4 953 m (10 % of the ride)**, IQ 0.58, gap 34.9 m, **leg0** at km 24.3 (seam km 29.3) | spikes 0, reuse 0.0219, stem 0.0000 |
| **novisad / 200 km / c0.8 / s11 / slot 5** | 183.6 km | **18 852 m**, IQ 0.57, gap 34.0 m, **leg0** at km 73.0 (seam km 91.9) | spikes 0, reuse **0.0000**, stem 0.0000, shadow 0.0057 |
| **novisad / 20 km / c0.5 / s42 / slot 3** | 24.1 km | 1 697 m, IQ 0.61, gap 29.5 m, **leg0** at km 9.5 | spikes 0, reuse 0.1151, stem 0.0000 |
| **djerdap / 300 km / c0.5 / s42 / slot 10** | 303.7 km | 2 489 m, IQ 0.77, gap 28.5 m, **leg1** at km 144.0 (seam km 139.6) | spikes 0, reuse 0.0462, stem 0.0000 |
| **belacrkva / 20 km / c0.5 / s23 / slot 6** | 25.8 km | 1 256 m, IQ **0.82**, gap 36.4 m, **leg0** at km 9.7 | spikes 0, reuse **0.0000**, stem 0.0000 |

Cross-leg, high-quality rings for contrast: **novisad / 100 km / c0.5 / s23 / slot 11** — a 1 702 m ring (2.0 % of an 84.6 km loop), IQ **0.82**, rejoin gap 24.6 m, at km 41.5→43.2 straddling the seam; **belgrade / 100 km / c0.5 / s42 / slot 8** — 7 463 m ring (8.4 %), IQ 0.78, gap 11.5 m, km 38.4→45.9.

### 4.3 Detector proposal — **D3 `self_touch` / `ring`**

> **(i) Near-rejoin rings.** Decimate the loop to ~25 m; hash-grid the points; for each point *i* keep the *nearest-in-arc* partner *j* with `dist ≤ r_touch` and `min(arc, total − arc) ≥ L_min`; reject the pair when the shorter arc's isoperimetric quotient `< iq_min` (that kills out-and-backs, which the retrace detector owns); reject when both endpoints fall inside the Start Exemption of the ride ends (that is the loop's own closure); take rings greedily shortest-first with an index-occupancy mask so nested touches count once.
> **Outputs:** `ring_count`, `max_ring_m`, `ring_frac = max_ring_m / total_m`, `intra_leg_ring_count` (the strict product Lollipop), plus `self_crossing_count` from a second, independent pass (proper segment intersections, non-adjacent by ≥ 500 m of arc, crossing angle ≥ 30° so a jittering retrace does not register hundreds of pseudo-crossings, clustered at 150 m so one physical junction is one event).
> **Parameters:** `r_touch 40 m` (= `lollipop_corridor_radius_m`), `L_min 800 m`, `iq_min 0.15`, closure guard 1 500 m (= Start Exemption), crossing `min_arc 500 m` / `min_angle 30°` / `cluster 150 m`.
> **Cost:** **8.5 ms/loop** (rings) + **7.9 ms/loop** (crossings) measured; both O(n) after decimation.
> **Reads on b1-x02:** `intra_leg_ring_count ≥ 1` on **13.7 %**; `max_ring_m ≥ 2 km & ring_frac ≤ 0.25` on **24.8 %**; `self_crossing_count ≥ 1` on **62.1 %**; figure-8 on **18.0 %**.
> **Suggested gate form:** `G6' intra_leg_ring_count ≥ 1 with ring ≥ 1 500 m` on ≤ *Z* % of loops (b1-x02: **6.0 %**) — this is the gate that replaces `is_lollipop`, which currently gates a shape the product does not name. Keep a **separate advisory** on `self_crossing_count` until the field census says whether riders read a self-crossing as a defect or as a normal big loop; 62 % is far too common to gate blind.
> **Parameter sensitivity to record:** `iq_min` is load-bearing. At `iq_min = 0.05` the ring rate rises 49.6 % → 59.4 % because thin "rings" that are really parallel-road out-and-backs enter (validated by hand on `vlasina / 200 km / c0.5 / s101 / slot 11`, ring IQ 0.05, and `belacrkva / 200 km / c0.5 / s101 / slot 1`, IQ 0.05 — both are retraces, not rings). The 0.15 floor was chosen from those hand checks, not from a sweep; Gate v2 should sweep it.

---

## 5. H4 — Fallback Loops: the tag never leaves the engine

**Verdict: STRUCTURALLY UNCOUNTABLE from the saved data. The vocabulary claims a tag that does not exist on the wire.**

`curvagen-orchestrator/CONTEXT.md:114-117` defines **Fallback Loop** as "a loop whose return leg ran under the soft reuse leash because hard exclusion found no route home. **Tagged in responses, counted by the harness**; the residual the gate tolerates."

Measured against the fork and the data:

- The tag exists **only inside the engine**: `struct Loop { … bool fallback; … bool second_via; }` at `src/thor/route_action.cc:1732`/`:1735`, set at `:1709`/`:1972`, consumed at `:2025-2026` for the distance-correction threshold, and emitted only as `LOG_INFO` (`:2127`, `:2130`, `:2183-2185`).
- There is **no response field**. `proto/descriptors/trip.proto` has no `fallback`/`second_via` member; `options.proto:577-578` carries the round-trip *request* sub-message only.
- Grepping all 232 saved `responses/*.json` for `"fallback`, `"second_via`, `"dirty`, `"stage`, `"kind`, `"tag`, `"debug` returns **0 files**. The full key-path enumeration of the responses contains nothing beyond stock Valhalla `trip`/`legs`/`summary`/`locations`.

So the harness has never counted fallbacks, and cannot. **This is a documentation defect to fix in the same pass as Gate v2** (either add the field to the response, or correct the glossary entry).

### 5.1 Best available proxy, and how far off it is

Hard exclusion bars the return from the forward leg's edges *beyond* the exemption (`route_action.cc:1587-1594`). Therefore **any reuse outside the exemption implies the return did not run under exclusion** — a decent one-sided proxy, with two known error modes: it misses the (very common) fallback whose reuse happens to fall entirely inside the exempt zone, and it can fire on a loop whose return passes an exempt forward edge far from its own end.

| reading | baseline | **b1-x02** |
|---|---|---|
| reuse outside the exemption ≥ 100 m | 76.0 % | **35.2 %** |
| … ≥ 1 km | — | **31.3 %** |
| mean metres outside the exemption | — | 4 738 m |

By distance (≥ 100 m): 20 km 37.1 · 50 km 38.6 · **100 km 43.5** · 200 km 30.5 · 300 km 27.1. By origin: **vlasina 100 %**, golija 30.7, nis 29.6, zlatibor 27.2, djerdap 25.6, novisad 25.3, belgrade 22.1, belacrkva 21.8. Served slots 0–2 **33.8 %** vs 35.7 % — fallbacks do **not** cluster away from the served slots.

**How big the undercount is:** the A2 audit measured the engine's own `fallback_count` on this corpus at **8.4 per 12-candidate request at 20 km and 50 km, 7.7 at 100, 3.9 at 200, 3.1 at 300** (`2026-07-15-a2-fallback-return-astar-audit.md`, "Characterization"), i.e. ~70 % of candidates at short distance. The geometric proxy reads 37–39 % there. **The proxy is a lower bound off by roughly 2× at short distance** — consistent with the exemption swallowing the common village-stem fallback.

Fallbacks are also *not* served as dirty loops: the Defect Gate rejects an exact-mirror seam stub ≥ 30 m (`route_action.cc:1056`, `kSeamStubRejectM`) and pushes it to `dirty_loops`, and **`spike_ge_30m` reads 0.0 % on all 2 779 b1-x02 loops** — so no dirty-by-seam-stub loop reached a slot in this run.

### 5.2 Named examples (served slots only)

| loop | ride | reuse outside the exemption |
|---|---|---|
| **vlasina / 50 km / c0.8 / s7 / slot 1** | 98.8 km (for a 50 km request) | **83 305 m = 84.4 % of the ride**; reuse 0.8436, IQ 0.006, `is_lollipop` **True** |
| **golija / 300 km / c0.5 / s101 / slot 0** | 282.8 km | 79 300 m = 28.0 %; reuse 0.2804, spikes 0, stem 0.0000 |
| **vlasina / 300 km / c0.5 / s101 / slot 1** | 331.4 km | 41 852 m = 12.6 % |
| **belacrkva / 200 km / c0.5 / s11 / slot 1** | 268.4 km | 39 229 m = 14.6 % |
| **djerdap / 200 km / c0.8 / s7 / slot 1** | 193.9 km | 37 034 m = 19.1 % |

### 5.3 Detector proposal — **D4 `provenance`**

> Not a geometry detector — a **wire change**. Emit per-trip build provenance in the round-trip response: `{fallback: bool, second_via: bool, dirty: bool, corrected: bool, harvest_band: float}`. The values already exist on the internal `Loop` (`route_action.cc:1729-1736`); the serializer change is one optional message on the trip.
> **Cost:** zero at runtime; a FINGERPRINT-neutral additive response field (it does not change route output, so no `FINGERPRINT_VERSION` bump — confirm against `crates/cache` before landing).
> **Reads on b1-x02:** unavailable — that is the point. The proxy reads 35.2 %; the engine ledger says ~70 % at short distance.
> **Suggested gate form:** `fallback_frac ≤ base + 2 pp` per distance band, plus a **hard** `dirty_served == 0` on a done-run. Until the field exists, Gate v2 should carry the proxy explicitly labelled a lower bound.

---

## 6. H5 — Second Via two-lobe builds

**Verdict: BLIND (no tag, spliced away by design), but measurably RARE — this is not where the rider-visible residual lives.**

The Second Via rebuild routes turnaround → V2 → start and then "splice[s] B+C into one return leg (response seam stays the turnaround)" (`route_action.cc:1950-1966`) precisely so the 2-leg serialization contract holds. Confirmed on the data: **all 2 779 b1-x02 trips have exactly 2 legs and 3 `break` locations** — a two-lobe build is wire-indistinguishable from a one-lobe build.

Two geometric signatures were prototyped:

| signature | baseline | **b1-x02** |
|---|---|---|
| ≥ 2 prominent lobes in the distance-from-start profile (prominence ≥ 25 % of max, separation ≥ 2 km) | 2.5 % | **3.0 %** |
| … relaxed (prominence ≥ 15 %, separation ≥ 1.5 km) | — | **9.4 %** (≥ 3 lobes: 13 loops) |
| the **leg boundary is not the ride's farthest point** (`d(seam) < 0.9 × d(max)`) | — | **42.0 %** (12.3 % below 0.75×) |

The last row is the more useful one, and it has a second consequence (§7.3). Two-lobe loops by origin are concentrated in **vlasina 13.0 %**, everything else ≤ 2.9 %.

**How a rider reads them:** a two-lobe circuit whose lobes are comparable is a figure-8, which the self-crossing detector catches independently (18.0 % of loops have a crossing lobe ≥ 25 % of the loop — far more than 3.0 %, so most figure-8 shapes in b1-x02 are *not* Second Via rebuilds, they are ordinary loops that cross themselves). Second Via is doing its job: it fires on `stem_fraction > 0.10` at build time (`route_action.cc:1026`, `:1879`) and the served `is_lollipop` rate is 0.3 %.

### 6.1 Named examples

| loop | ride | lobes | note |
|---|---|---|---|
| **belacrkva / 200 km / c0.8 / s7 / slot 0** *(served)* | **390.2 km for a 200 km request** (`distance_error` 0.95) | peaks at km 133.3 (71.9 km out) and km 279.0 (71.0 km out); **seam at km 96.6** — both lobes are inside leg1 | spikes 0, reuse 0.0000, IQ 0.248 |
| **novisad / 200 km / c0.5 / s42 / slot 0** *(served)* | 212.3 km | peaks at km 77.5 (54.9 km out) and km 140.0 (56.8 km out); seam km 84.3 | IQ **0.004** — the two lobes cancel signed area, exactly the `compactness` caveat at `metrics.py:543-547` |
| **zlatibor / 200 km / c0.8 / s11 / slot 2** *(served)* | 192.0 km | peaks km 66.0 / km 137.0; seam km 89.5 | reuse 0.0127, spikes 0 |
| **djerdap / 200 km / c0.8 / s11 / slot 7** | 272.8 km | peaks km 108.9 (58.4 km out) / km 212.1 (49.2 km out); seam km 66.6 | reuse 0.0000 |
| **golija / 200 km / c0.8 / s7 / slot 3** | 207.7 km | peaks km 82.7 / km 145.5; seam km 91.3 | reuse 0.3469, shadow 0.2852 |

### 6.2 Detector proposal — **D5 `lobes`**

> Decimate the ride to 100 m, take great-circle distance from the start as a 1-D profile, and count prominence-filtered maxima (prominence ≥ `p × max`, separation ≥ `s`). Report `lobe_count`, `seam_is_apex = d(seam)/d(max)`, and `apex_leg`.
> **Parameters:** `p 0.25`, `s 2 000 m`; report the relaxed `p 0.15 / s 1 500 m` alongside — the reading moves 3.0 % → 9.4 %, so a single threshold is not defensible yet.
> **Cost:** **4.1 ms/loop** measured; `seam_is_apex` alone is **0.4 ms/loop**.
> **Reads on b1-x02:** `lobe_count ≥ 2` 3.0 % (9.4 % relaxed); `seam_is_apex < 0.9` on 42.0 %.
> **Suggested gate form:** **advisory only.** Ship `lobe_count` and `seam_is_apex` as reported metrics in v2 so the census has a handle; do not gate until D4 provenance exists to separate "Second Via did its job" from "the loop is shaped oddly for another reason".

---

## 7. H6 — what else the data shows

### 7.1 The corridor meter sees it; no gate reads it

`shadow_frac` (fraction of leg0 within 40 m of leg1 *anywhere*, `metrics.py:462`) is the only existing meter that catches near-mirror and mid-route corridor sharing — and no gate touches it.

| reading on b1-x02 | value |
|---|---|
| `shadow_frac` mean / p50 / p90 / max | 0.097 / 0.019 / 0.332 / 0.941 |
| loops with `shadow_frac ≥ 0.30` | **10.9 %** |
| loops with `shadow_frac ≥ 0.30` **and `lollipop_stem_fraction == 0`** | **5.7 % of all loops** |
| loops with `shadow_frac ≥ 0.50` and stem 0 | 2.4 % |
| `bulb_count ≥ 2` / `≥ 3` | **51.0 % / 19.8 %** |
| `compactness < 0.10` | **17.1 %** (20 km: 28.8 %, 50 km: 22.9 %, 300 km: 18.3 %) |

Recomputing shared corridor as a fraction of the **whole loop** (both legs, 40 m): mean 0.090, and **20.4 % of loops spend ≥ 10 % of the ride on cross-leg shared corridor**, 11.6 % ≥ 25 %, 5.6 % ≥ 50 %. At 20 km that is **42.5 % / 27.9 %**; at 300 km, 2.1 % / 0.0 %. Served slots 0–2: 8.9 % ≥ 25 % vs 12.5 % for slots 3–11.

Named: **golija / 50 km / c0.8 / s7 / slot 5** — `shadow_frac 0.9149`, reuse 0.8208, IQ 0.001, **`lollipop_stem_fraction` 0.0000, `is_lollipop` False**. **djerdap / 20 km / c0.5 / s101 / slot 11** — shadow 0.8290, stem 0, IQ 0.176. **belgrade / 50 km / c0.5 / s42 / slot 7** — shadow 0.8141, reuse 0.1319, stem 0, IQ 0.0011.

### 7.2 The 120 m gap is the single most sensitive parameter in the gate

`prefix_len` (`metrics.py:425-440`) walks the shared prefix and **breaks** at the first unshared gap > `lollipop_gap_m = 120` (`metrics.py:60`, pinned to `kStemGapM`, `route_action.cc:1028`). Re-running the *identical* formula at other gap tolerances, on the same 2 779 loops:

| `gap_m` | mean stem (m) | mean `stem_frac` | `is_lollipop` (gate 6) |
|---|---|---|---|
| **120 (shipped)** | 866 | 0.0039 | **0.29 %** |
| 400 | 1 066 | 0.0047 | 0.54 % |
| 1 000 | 2 155 | 0.0243 | **7.92 %** |
| 3 000 | 3 938 | 0.0570 | 10.94 % |

**11.7 % of loops have `stem_frac == 0` under the shipped gap while carrying ≥ 1.5 km of stem at a 1 km gap** (5.4 % carry ≥ 5 km). By origin: belgrade 29.9 %, golija 29.0 %, zlatibor 13.3 %, djerdap 8.6 %, novisad 6.6 %, belacrkva 4.3 %, nis 2.0 %, vlasina 0.0 %.

Worked mechanism — **belacrkva / 50 km / c0.8 / s7 / slot 5**, the 96 %-retraced loop from §2.2. Its leg0 shared-mask run-length encoding against leg1 is:

```
shared 0–32 m | UNSHARED 148–577 m (429 m gap) | shared 595–22 054 m | unshared 22 070–28 246 m | shared 28 261–28 297 m
```

The walk breaks at 577 m with `last_shared_len = 32 m`; `32 − 1500 → 0`; `stem_out = 0`. The **21.5 km** of shared corridor beginning 595 m from the ride start is never reached. `shadow_frac` reads it correctly at 0.7666 — and is ungated.

### 7.3 Serving mode's derived seam is wrong on ~42 % of loops

`Loop.from_serving_route` derives the seam as the grid point farthest from the start (`metrics.py:224-244`, seam pick at `:238`) because the app DTO has no legs; the module docstring calls seam-relative fields "approximate" (`:19-23`). Measured against the true leg boundary on b1-x02: **`d(seam) / d(max)` mean 0.886, p10 0.725; the leg boundary is not the farthest point on 42.0 % of loops (12.3 % below 0.75×), and where they differ the true apex sits in leg1 67.3 % of the time.** 30.4 % of all loops have their farthest point ≥ 5 km of riding away from the true seam. By distance: 20 km 45.8 %, 50 km 53.5 %, 100 km 47.3 %, 200 km 34.8 %, 300 km 26.9 %.

Consequence: in serving mode every seam-anchored metric — spike class (`seam_uturn`/`seam_wrapped`), `stem_out`/`stem_back` (prefix/suffix of the *derived* legs), `rejoin_return_frac`, `seam_frac` — is mis-anchored on ~2 loops in 5. **Any Gate v2 that intends to gate the serving path must either carry the leg split in the DTO or use only seam-free detectors** (D1, D3 and the whole-loop form of D2/D6 are all seam-free by construction; the zone split in D1 is the only part that needs the seam).

### 7.4 Near-mirror U-turns at the seam

The seam-zone slice of D1 isolates the "U-turn across a dual carriageway at the turnaround" the atlas predicted but never counted: **2.7 % of b1-x02 loops carry ≥ 200 m of unseen near-mirror within 250 m of the seam** at r = 25 m, rising to **7.3 % at r = 40 m** (baseline 2.2 % / —). Small, but it is exactly the class `find_spikes` was designed to catch and cannot. Concentrated at 20 km (5.4 %) and 300 km (4.8 %); by origin golija 5.5 %, nis 3.2 %, vlasina 3.2 %.

---

## 8. Proposed metrics v2 / Gate v2 table

Everything below is a **proposal for the Gate v2 grilling**, not a decision. "Reads on b1-x02" is what the prototype measured; "baseline" is `baseline-v1.2-fix44` under the same prototype.

| # | metric (v2) | replaces / adds to | formula sketch | cost | baseline | **b1-x02** | proposed gate form |
|---|---|---|---|---|---|---|---|
| M1 | `max_retrace_run_m` | `max_stub_km` (gates 1–2) | D1, r = 10 m, longest anti-parallel run | 14.5 ms | 68.7 % ≥ 500 m | **32.9 %** | blocking: ≥ 2 km on ≤ ratchet(baseline) — b1-x02 **25.9 %** vs base 50.3 % |
| M2 | `retrace_frac` | `spike_len_fraction` | D1 flagged length / total | (same pass) | 0.121 mean | **0.072 mean** | blocking: mean ≤ ratchet, per distance band |
| M3 | `retrace_unseen_m` | **new** — nothing sees it | D1 at r = 25 m minus repeated grid keys | (same pass) | 1 300 m mean | **1 217 m mean** | blocking: ≥ 2 km on ≤ ratchet — b1-x02 **8.8 %** vs base 9.8 % (**the axis v3 never moved**) |
| M4 | `exempt_corridor_frac` | **new** — makes the tolerance visible | D2, shared corridor inside 1.5 km of each end ÷ ride | 12.8 ms (folds into `corridor_stats`) | 8.3 % ≥ 0.05 | **7.2 % ≥ 0.05** (20 km: 26.7 %) | blocking **per band**: ≤ 0.15 on ≥ 95 % of loops; or re-spec the exemption as `min(1500, 0.05 × target)` (moves `route_action.cc:999` in the same commit) |
| M5 | `reuse_hidden_m` | audit companion to gates 3–5 | `edge_reuse_geom(0) − edge_reuse_geom(1500)` | ~0 | 719 m mean | **549 m mean** | advisory (audit trail for M4) |
| M6 | `intra_leg_ring_count` / `max_ring_m` | **replaces `is_lollipop`** | D3 near-rejoin, `iq ≥ 0.15`, `L ≥ 800 m` | 8.5 ms | — | **13.7 %** ≥ 1; 6.0 % ≥ 1.5 km | blocking: ring ≥ 1 500 m inside one leg on ≤ *Z* % — b1-x02 **6.0 %** |
| M7 | `ring_frac` (bulb class) | `bulb_count` (currently ungated) | `max_ring_m / total_m ≤ 0.25` | (same pass) | 30.0 % ≥ 2 km | **24.8 % ≥ 2 km** | advisory pending census |
| M8 | `self_crossing_count` | **new** | D3b, ≥ 500 m arc, ≥ 30°, 150 m cluster | 7.9 ms | 72.6 % | **62.1 %** | advisory — 62 % is too common to gate blind |
| M9 | `shadow_frac_loop` | promotes `shadow_frac` to whole-loop and **to a gate** | cross-leg 40 m corridor ÷ total ride | (same pass as M4) | — | mean 0.090; **20.4 % ≥ 0.10, 11.6 % ≥ 0.25** | blocking: ≥ 0.25 on ≤ ratchet, per band (20 km 27.9 % is the hard cell) |
| M10 | `stem_frac` @ gap 1 000 m | **re-parameterizes** gate 6 | identical formula, `gap_m = 1000` | ~0 | — | `is_lollipop` **0.29 % → 7.92 %** | decide the gap in the grilling; today's 0.3 % is a parameter artefact, not a quality reading |
| M11 | `fallback` / `second_via` / `dirty` | **wire change** (D4) | engine emits the tags it already computes | 0 | — | unavailable (proxy 35.2 %; engine ledger ~70 % @ ≤ 50 km) | blocking `dirty_served == 0`; `fallback_frac ≤ base + 2 pp` |
| M12 | `lobe_count`, `seam_is_apex` | **new** (D5) | prominence peaks on distance-from-start | 4.1 ms | 2.5 % ≥ 2 lobes | **3.0 %**; `seam_is_apex < 0.9` on **42.0 %** | advisory; **`seam_is_apex` is also the serving-mode-validity flag** (§7.3) |
| M13 | `compactness` | already metered, **ungated** | unchanged (`metrics.py:536`) | ~0 | — | **17.1 % < 0.10** (20 km 28.8 %) | blocking: `< 0.05` on ≤ ratchet — a sub-0.05 IQ loop is geometrically an out-and-back |

Total added compute: **~48 ms per loop**, ~2.2 min for the full 2 779-loop corpus on one core (the existing analysis pass already costs more than that), and none of it touches the engine.

**Two structural recommendations for the grilling, independent of thresholds:**

1. **Rename or re-anchor the meters to the product terms.** Today `Spike` (orchestrator `CONTEXT.md:98-101`) is defined as "the measured form of a product Spur … an exact-mirror out-and-back stub" — the definition *builds the blind spot into the ubiquitous language*. The product's **Spur** ("returns along the same *or nearly same* road") and **Backtracking** ("same *or nearly same* road in the opposite direction") both already say "nearly". Metrics v2 should measure what the glossary says.
2. **Gate the meters that already exist before adding new ones.** `shadow_frac`, `bulb_count` and `compactness` cost zero and would have caught **belgrade / 100 km / c0.5 / s101 / slot 6**, **golija / 50 km / c0.8 / s7 / slot 5** and **belacrkva / 50 km / c0.8 / s7 / slot 5** respectively — three of the worst loops in this document — without a single new line of detector.

---

## 9. What the field census must still verify on the rig

Everything above is geometry. These questions cannot be settled from saved JSON:

1. **Is a 14 m dual-carriageway out-and-back a defect to a rider, or a non-event?** The whole M3 threshold depends on it. Fixture: **belgrade / 100 km / c0.5 / s101 / slot 6**, run 4 409 m at 44.86306, 20.58080.
2. **Switchbacks vs retrace.** The near-mirror detector cannot separate them planimetrically (§2.1). The census must either (a) re-run the corpus in **serving mode** and use the DTO's centimetre elevation to add a vertical-separation test (`|Δz| > 15 m ⇒ switchback`), or (b) run the `/trace_attributes` pass and use way-ids. Both are already-built harness capabilities (`--serving`, `--trace-engine`). Until then, the mountain-origin share of M3 (zlatibor 35.8 %, golija 29.6 %) is an upper bound.
3. **How small a ring still reads as a Lollipop?** M6's `L_min = 800 m` and `iq_min = 0.15` are hand-set from six inspected cases. Fixtures: **nis / 20 km / c0.5 / s23 / slot 8** (1 678 m, IQ 0.68, gap 0.0 m — should be a hit) and **vlasina / 200 km / c0.5 / s101 / slot 11** (2 481 m, IQ 0.05 — should be a miss, it is a retrace).
4. **Does a self-crossing bother anyone?** 62.1 % of served loops cross themselves. If riders do not care, M8 stays advisory forever and the figure-8 discussion closes.
5. **Is the 120 m stem gap defensible?** The gate's headline lollipop number moves 27× on this one parameter (§7.2). The census should look at the 11.7 % of loops the gap zeroes and say whether their shape is a lollipop.
6. **Ground-truth the fallback rate.** Re-run one corpus cell with the engine's debug ledger enabled (`route_action.cc:2183-2185` already prints `fallbacks=` and `second_vias=` per request) and compare against the 35.2 % geometric proxy — that calibrates M11 before the wire change lands.
7. **Serving-mode seam validity.** Re-run the corpus in `--serving` mode and compare `seam_is_apex` against the engine-mode leg boundary loop-for-loop; §7.3's 42 % is measured on engine-mode geometry and predicts, but does not prove, the serving-mode error.
8. **Distance outliers.** **belacrkva / 200 km / c0.8 / s7 / slot 0** is served at **390.2 km for a 200 km request**. Gate 7 absorbs it in a mean; the census should decide whether a 1.95× overshoot is ever servable.

---

## Appendix A — method, commands, reproduction

**Nothing under `tools/loopqual/` was modified.** All prototypes live in `~/.curvagen-scratch/` and import the pinned harness read-only:

- `lqbs_lib.py` — detector library. Imports `metrics` from `tools/loopqual`; uses `Loop.from_engine_trip`, `metrics._shared_mask`, `metrics.edge_reuse_geom`, `metrics.corridor_stats`, `grid_to_ll`, `seg_len_m`, `bearing_deg`. Defines `antimirror_runs` (D1), `exempt_stats` + `raw_stem` (D2), `near_rejoin_rings` + `self_intersections` (D3), `distance_peaks` (D5), `zone_of`, `xy_frame`. `prefix_len` is a verbatim port of the nested function in `metrics.corridor_stats` (used only in `lqbs_stemgap.py`, to vary `gap_m`). `raw_stem` sets `metrics.PARAMS["start_exemption_m"] = 0.0` **at call time and restores it in a `finally`** — an in-memory override of a value `corridor_stats` reads at line 454, never a file edit.
- `lqbs_measure.py` — D1 (r = 10/25/40) + D2 + D3 + D5 per loop.
- `lqbs_rings.py` — D3 rings only, tightened (`iq ≥ 0.15`, closure guard 1 500 m).
- `lqbs_xing.py` — D3b crossings only, angle-filtered and clustered.
- `lqbs_stemgap.py` — stem-meter gap sweep (§7.2).
- `lqbs_lobe.py` — `seam_is_apex` (§7.3).
- `lqbs_report.py`, `summary.py`, `examples.py` — aggregation and named-example extraction.

```bash
# all paths absolute; python3 = system CPython 3.14 (the harness venv works identically —
# the detectors are dependency-free)
cd ~/.curvagen-scratch

python3 lqbs_measure.py  /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02              b1x02.jsonl
python3 lqbs_measure.py  /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/baseline-v1.2-fix44 base.jsonl
python3 lqbs_rings.py    /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02              b1x02_rings.jsonl
python3 lqbs_rings.py    /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/baseline-v1.2-fix44 base_rings.jsonl
python3 lqbs_xing.py     /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02              b1x02_xing.jsonl
python3 lqbs_xing.py     /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/baseline-v1.2-fix44 base_xing.jsonl
python3 lqbs_stemgap.py  /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02              b1x02_stem.jsonl
python3 lqbs_lobe.py     /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02              b1x02_lobe.jsonl

python3 lqbs_report.py b1x02.jsonl \
  /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02/loops.jsonl
python3 summary.py     # the §1/§8 cross-tab (ALL / slots 0-2 / slots 3-11 / distance / origin)
python3 examples.py    # the named-example tables
```

**Provenance grep (§5):**

```bash
cd /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/b1-x02/responses
for k in fallback second_via dirty stage kind tag debug; do
  echo "$k: $(grep -l "\"$k" *.json 2>/dev/null | wc -l) files"; done      # all zero
```

**Validation performed before any number was quoted:**

- **D1 against known ground truth** — run on `baseline-v1.2-fix44`, where `find_spikes` reports 43.9 % spiked loops. Every exact spike reappears as an anti-parallel run with `same_key_m ≈ flagged_m` and mean offset 0.0–1.3 m (e.g. `belacrkva / 100 km / c0.5 / s101 / slot 0`, `seam_wrapped` stub 1 943 m ↔ run 1 876 m at offset 0.0 m).
- **D1 point-level proof on b1-x02** — for `vlasina / 200 km / c0.5 / s11 / slot 5`, dumping the raw grid points shows the return traversal is the forward sequence reversed, point for point: forward `[(4267493,2237505), (4267480,2237506), (4267474,2237506), (4267458,2237507), (4267439,2237507)]` ↔ return `… (4267439,2237507), (4267458,2237507), (4267474,2237506), (4267480,2237506), (4267493,2237505)`; **2 288 of 2 290 grid keys shared**, and `metrics.find_spikes(loop)` returns `[]`.
- **D3 by hand on six candidate rings**, which drove the `iq_min` 0.05 → 0.15 tightening and the 1 500 m closure guard (two false positives found and removed: a thin parallel-road retrace, and the ride's own start/end closure — `belgrade / 50 km / c0.8 / s7 / slot 1`, touch at km 0.445 ↔ km 47.158).
- **D3b crossing filter** — before the 30° angle filter and 150 m clustering, a jittering exact retrace produced up to **570 pseudo-crossings** on one loop; after, the corpus mean is 1.26 and the max 20.
- **Determinism** — all detectors are pure functions of the saved polylines; re-running any script reproduces its `.jsonl` byte-for-byte.

**Primary sources cited:** `tools/loopqual/metrics.py` (v1.3), `tools/loopqual/gate_v1_proto.py`, `tools/loopqual/README.md`, `tools/loopqual/corpus-v1.yaml`, `results/b1-x02/{report.md,report.json,loops.jsonl,responses/}`, `results/baseline-v1.2-fix44/`, `src/thor/route_action.cc`, `proto/descriptors/{trip,options}.proto`, `docs/curvagen/adr/0037-roundtrip-v3-defect-gated-loop-construction.md` §3, `docs/curvagen/research/2026-07-13-round-trip-defect-atlas.md` §3/§4/§6/§7/§9, `docs/curvagen/research/2026-07-13-loop-quality-harness.md`, `docs/curvagen/research/2026-07-15-a2-fallback-return-astar-audit.md`, `curvagen-meta/CONTEXT.md:85-95`, `curvagen-orchestrator/CONTEXT.md:92-152`, `curvagen-orchestrator/crates/api/src/serving/{mod.rs:38,lookup.rs:46-90}`.

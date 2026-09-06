# Why the return re-rides the same straight road instead of the neighbour that looks the same

- **Date:** 2026-09-06 (curvagen-valhalla [#13](https://github.com/Lazark0x/curvagen-valhalla/issues/13), input to the P1.1 close, Gate v2 and P2; wayfinder map round-trip v4 [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4))
- **Scope:** Andrey's verdict on the P1.1 worst-20 gallery — *"better, but I don't know why the algorithm prefers the straight same road instead of a neighbouring one that looks the same."* Answered case by case on **all 20 worst P1.1 loops** (`tools/loopqual/results/p1-1-road-identity/`, D1b unseen near-mirror metres) plus **9 more Vračar-block loops** — every block-A loop in the corpus whose return retraces a *straight* stretch ≥ 300 m (chord/arc ≥ 0.80). 29 loops, 66 retrace stretches, 18 distinct pieces of pavement. Cell-level coordinates only; roads are named by their OSM name/ref.
- **Method:** offline decode of the saved P1.1 responses with the pinned detectors (`~/.curvagen-scratch/p1/lqbs_lib.py::antimirror_runs`, r = 25 m, `min_run_m = 300`, importing `tools/loopqual/metrics.py` read-only — nothing under `tools/loopqual/` was modified). Road identity, neighbours and the counterfactual come from a **read-only prod-equivalent engine** (`valhalla-curvature:prod-equivalent-b4f514d7f`, arm64, commit `b4f514d7f`) served on **:8005** against the same read-only Serbia tiles (`curvagen-orchestrator/data`, `tileset_last_modified 1785932567`), container `rt-nbr-8005`, removed at the end. `rt-p1-build` (:8003), `valhalla-local` (:8002), :8004 and :8791 were never addressed; no production traffic. Fallback provenance is read from the P1.1 run's own engine ledger (§3).

## TL;DR verdict

**Three different things are being called "the algorithm prefers the same road", and only one of them is that.**

| verdict | what it actually is | loops (of 29) | retrace metres (of 42 551 in 66 stretches) | fix owner |
|---|---|---|---|---|
| **H0 — not a retrace** | a mountain **switchback**: the road folds back on itself inside 400 m, D1b's radius-only geometry counts the two arms as a near-mirror. Plus two loops whose whole residue is < 300 m. | **15** (13 + 2) | 11 803 m in 31 stretches | Gate v2 / D1b detector |
| **H2 — fallback rung 2** | the return failed rung 0, rung 2 dropped **every** hard exclusion at once, and the pavement came back with it | **9** | 18 881 m in 17 stretches | fallback policy |
| **H3 — twin miss (scope, not radius)** | the two carriageways of a dual carriageway, ridden **out and back inside the return leg**. They *are* a registered twin pair; the twin bar is assembled from the **forward corridor only**, so nothing applies. | **5** | 9 937 m in 14 stretches | P1.2 twin test |
| H1 — Start Exemption | 480 m + 485 m of the same street at both ends of a 70 km ride, inside the 1 500 m exemption — legal by construction | co-present on 2, never dominant | 1 930 m in 4 stretches | Gate v2 exemption policy |
| H4 — costing class gap | never sole. The retraced road is 1.4×–2.5× cheaper **per metre**, always because of *speed*, never because of road class | co-present on all H2/H3 | — | no change indicated |
| H5 — curvature economics / harvest commitment | never sole. Present as *distance padding*: the return has to burn 27 km to cover an 8 km gap, and the excursion it buys is what gets re-ridden | co-present on 5 | — | P2 |

**The direct answer to the question.** On the two worst loops in the gallery (Vračar 40 km, 2 759 m each) the "same straight road" is the **M11 / E 70 trunk**, and the return leg makes a ~6.5 km westward excursion along it and comes straight back — **on the opposite carriageway, both passes inside the return leg**. The engine's own ledger reads `self-overlap 0 m` for those loops and tiers them `r0` — a clean hard-exclude success — because the built-loop score's overlap meter and the geometry gate's `twin_ride` test both only ask *"is this return edge on the **forward** leg?"* (`src/thor/route_action.cc:1883-1914`). A return leg re-riding **itself** is invisible to every P1.1 mechanism. And the neighbour that looks the same on the map is not one: with both carriageways barred, the engine finds **no route at all** across that stretch — the roads at 27–50 m are severed frontage stubs.

**So: it is not the costing.** At c0.5 with motorways allowed, `use_highways = 0.65` puts `highway_factor` at **−0.0034**, and `kHighwayFactor` is **0.5 for trunk and 0.5 for unclassified alike** (ADR-0032's residential/unclassified demotion, `src/sif/motorcyclecost.cc:69-77`) — the class term between the M11 and its frontage road is *identically zero*. The 2.5× per-metre gap is 100 km/h against 40 km/h, i.e. time. Costing is behaving exactly as designed; the identity machinery has a hole.

---

## 1. What the corpus actually contains

Every retrace stretch ≥ 300 m at r = 25 m in the 29 loops, deduplicated by pavement — 18 distinct pieces:

| pavement (0.5° cell) | road | class / speed | len | offset mean/max | same-pavement m | chord/arc | run vs partner | verdict |
|---|---|---|---|---|---|---|---|---|
| (44.5, 20.0) | **M11 / E 70** "Аутопут за Загреб" | trunk 100 | 597 m | 16.7 / 23.3 | 0 | 1.000 | ret↔ret, 5 944 m apart | H3 |
| (44.5, 20.0) | M11 / E 70 | trunk 100 | 636 m | 20.3 / 24.6 | 0 | 1.000 | ret↔ret, 2 831 m apart | H3 |
| (44.5, 20.0) | M11 / E 70 | trunk 100 | 668 m | 19.9 / 22.9 | 0 | 1.000 | ret↔ret, 2 831 m apart | H3 |
| (44.5, 20.0) | **Аутопут за Нови Сад** | primary 60 | 576 m | 22.4 / 24.6 | 0 | 1.000 | ret↔ret, 2 004 m apart | H3 |
| (44.5, 20.0) | Аутопут за Нови Сад | tertiary 54 | 865 m | 13.2 / 19.6 | 0 | 0.958 | ret↔ret, 96 m apart (U-turn) | H3 |
| (44.5, 20.0) | Аутопут за Нови Сад | tertiary 50 | 796 m | 13.3 / 19.6 | 0 | 0.959 | ret↔ret, 92 m apart (U-turn) | H3 |
| (43.5, 19.5) | **Поблаће — Крајчиновићи** | unclassified 39 | 6 979 m ×2 | 0.0 / 0.0 | 6 979 | 0.921 | fwd↔ret, 9 976 m apart | H2 |
| (44.5, 20.5) | **Маршала Тита / Сланачки пут** | tertiary 50 | 325 m | 2.8 / 23.7 | 283 | 0.971 | fwd↔ret, 2 867 m apart | H2 |
| (44.5, 20.5) | Маршала Тита / Качарска | tertiary 48 | 358 m | 3.1 / 20.9 | 283 | 0.969 | ret↔fwd, 2 844 m apart | H2 |
| (44.5, 20.0) | **Храстов запис** | residential 35 | 335 m | 2.0 / 21.0 | 300 | 0.959 | fwd↔ret, 1 359 m apart | H2 |
| (44.5, 20.0) | Храстов запис | residential 33 | 315 m | 1.2 / 24.3 | 300 | 0.949 | ret↔fwd, 1 352 m apart | H2 |
| (44.5, 20.5) | **Двадесет деветог новембра** | tertiary 39 | 307 m | 4.6 / 18.3 | 235 | 0.994 | fwd↔ret, 7 114 m apart | H2 |
| (44.5, 20.0) | **Војислава Илића** (Vračar) | secondary 34 | 480 m | 1.0 / 17.7 | 476 | 0.226 | fwd↔ret, km 0.0 | **H1** |
| (44.5, 20.0) | Војислава Илића | secondary 35 | 485 m | 2.2 / 22.4 | 476 | 0.229 | ret↔fwd, km 43.9 | **H1** |
| (43.0, 19.0) | **192 / Бучје / Крњача** | secondary 60 | 395 m | 19.6 / 24.6 | 0 | **0.148** | **same index range** | **H0** |
| (43.5, 19.5) | **195 / Стража** | secondary 60 | 366 m | 17.2 / 24.7 | 0 | **0.212** | **same index range** | **H0** |
| (43.0, 19.0) | **Калуђеровићи / 192** | tertiary 52 | 383 m | 14.0 / 23.6 | 0 | **0.454** | **same index range** | **H0** |
| (44.0, 19.5) ×5 | Valjevo–Bajina Bašta hill roads | — | 200–290 m | 15–21 | 0 | — | same index range | H0 (sub-300) |

The decisive column is the last one. `antimirror_runs` reports each flagged run's **partner index range**; when that range *is the run's own* (`11157..11207` partnering `11157..11207`), the loop passed through once and the road doubled back on itself. Every zlatibor stretch in the worst-20 is that. Every H2/H3 stretch has a partner 92 m–10 km further along the path — a genuine second pass.

## 2. The case table

One row per retrace stretch ≥ 300 m; the loop columns are filled on its first row. `ledger tier` is the engine's own rung tier for that slot and `self-ovl` its twins-aware self-overlap metres (§3). The counterfactual, where measured, is §4's "cost of crossing this stretch without its own pavement".

| set | loop (block / cell / asked km / curviness / seed / slot) | built km | ledger tier / self-ovl | stretch at | retrace m | same road: class / speed | nearest parallel road (offset · class · speed) + counterfactual | way relation | verdict | fix owner |
|---|---|---|---|---|---|---|---|---|---|---|
| P1.1 | A/(44.798, 20.472)/40 km/c0.5/s101/slot8 | 45.22 | r0 / 0 m | km 21.52 | 597 m | M11 · trunk · 100 km/h | 20.7 m · M11 slip road (ramp); nearest independent pavement unclassified @ 27 m · no route | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 23.02 | 636 m | M11 · trunk · 100 km/h | 50.2 m · unclassified · 40 km/h | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 26.48 | 668 m | M11 · trunk · 100 km/h | 46.7 m · unclassified · 40 km/h · no route | two ways, twin pair | **H3** | P1.2 twin test |
| P1.1 | A/(44.798, 20.472)/40 km/c0.7/s101/slot7 | 45.22 | r0 / 0 m | km 21.52 | 597 m | M11 · trunk · 100 km/h | 20.7 m · M11 slip road (ramp); nearest independent pavement unclassified @ 27 m · no route | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 23.02 | 636 m | M11 · trunk · 100 km/h | 50.2 m · unclassified · 40 km/h | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 26.48 | 668 m | M11 · trunk · 100 km/h | 46.7 m · unclassified · 40 km/h · no route | two ways, twin pair | **H3** | P1.2 twin test |
| P1.1 | C/zlatibor/200 km/c0.5/s11/slot5 | 239.77 | r0 / 0 m | km 102.22 | 383 m | Калуђеровићи · tertiary · 52 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 109.76 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 206.24 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s23/slot4 | 239.77 | r0 / 0 m | km 102.22 | 383 m | Калуђеровићи · tertiary · 52 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 109.76 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 206.24 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s7/slot4 | 239.77 | r0 / 0 m | km 102.22 | 383 m | Калуђеровићи · tertiary · 52 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 109.76 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 206.24 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | B/(44.794, 20.491) demand#4/70 km/c1.0/s23/slot10 | 44.42 | r0 / 0 m | km 0.0 | 480 m | Војислава Илића · secondary · 34 km/h | 29.6 m · secondary · 49 km/h | same way, two passes | **H1** | Gate v2 exemption |
|  |  |  |  | km 20.02 | 576 m | Аутопут за Нови Сад · primary · 60 km/h | 21.2 m · the third carriageway of the same boulevard; nearest independent tertiary @ 53 m · +14641 m | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 20.68 | 865 m | Аутопут за Нови Сад · tertiary · 54 km/h | 25.1 m · tertiary · 30 km/h · +13857 m | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 21.63 | 796 m | Аутопут за Нови Сад · tertiary · 50 km/h | 31.0 m · tertiary · 30 km/h | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 43.93 | 485 m | Војислава Илића · secondary · 35 km/h | 27.6 m · secondary · 49 km/h | same way, two passes | **H1** | Gate v2 exemption |
| P1.1 | B/(44.794, 20.491) demand#4/70 km/c1.0/s7/slot10 | 44.42 | r0 / 0 m | km 0.0 | 480 m | Војислава Илића · secondary · 34 km/h | 29.6 m · secondary · 49 km/h | same way, two passes | **H1** | Gate v2 exemption |
|  |  |  |  | km 20.02 | 576 m | Аутопут за Нови Сад · primary · 60 km/h | 21.2 m · the third carriageway of the same boulevard; nearest independent tertiary @ 53 m · +14641 m | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 20.68 | 865 m | Аутопут за Нови Сад · tertiary · 54 km/h | 25.1 m · tertiary · 30 km/h · +13857 m | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 21.63 | 796 m | Аутопут за Нови Сад · tertiary · 50 km/h | 31.0 m · tertiary · 30 km/h | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 43.93 | 485 m | Војислава Илића · secondary · 35 km/h | 27.6 m · secondary · 49 km/h | same way, two passes | **H1** | Gate v2 exemption |
| P1.1 | C/zlatibor/200 km/c0.5/s101/slot4 | 230.93 | r0 / 0 m | km 90.81 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 197.4 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s101/slot7 | 272.62 | r0 / 0 m | km 142.61 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 239.09 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s11/slot4 | 230.93 | r0 / 0 m | km 90.81 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 197.4 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s11/slot7 | 275.44 | r0 / 0 m | km 145.42 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 241.91 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s11/slot11 | 249.91 | r3 / 7374 m | km 78.73 | 6979 m | Поблаће — Крајчиновићи · unclassified · 39 km/h | 28.2 m · unclassified · 20 km/h · no route | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 95.68 | 6979 m | Поблаће — Крајчиновићи · unclassified · 39 km/h | 28.2 m · unclassified · 20 km/h · no route | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 119.79 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 216.28 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s23/slot3 | 230.93 | r0 / 0 m | km 90.81 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 197.4 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s23/slot6 | 275.44 | r0 / 0 m | km 145.42 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 241.91 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s42/slot2 | 223.85 | r0 / 0 m | km 93.83 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 190.32 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s42/slot4 | 230.93 | r0 / 0 m | km 90.81 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 197.4 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s7/slot3 | 230.93 | r0 / 0 m | km 90.81 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 197.4 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| P1.1 | C/zlatibor/200 km/c0.5/s7/slot6 | 275.44 | r0 / 0 m | km 145.42 | 395 m | 192 · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
|  |  |  |  | km 241.91 | 366 m | Стража · secondary · 60 km/h | none within 150 m | switchback: one way, one pass | **H0** | Gate v2 / D1b |
| A-extra | A/vracar/25 km/c0.5/s42/slot11 | 20.83 | r2 / 469 m | km 8.57 | 325 m | Маршала Тита · tertiary · 50 km/h | 32.5 m · residential · 35 km/h · +2603 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 11.74 | 358 m | Маршала Тита · tertiary · 48 km/h | 22.3 m · residential · 35 km/h · +2603 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/25 km/c0.7/s42/slot11 | 20.83 | r2 / 469 m | km 8.57 | 325 m | Маршала Тита · tertiary · 50 km/h | 32.5 m · residential · 35 km/h · +2603 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 11.74 | 358 m | Маршала Тита · tertiary · 48 km/h | 22.3 m · residential · 35 km/h · +2603 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/40 km/c1.0/s101/slot11 | 38.66 | r2 / 299 m | km 17.68 | 335 m | Храстов запис · residential · 35 km/h | none within 150 m · +5149 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 19.36 | 315 m | Храстов запис · residential · 33 km/h | 77.4 m · residential · 35 km/h · +5149 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/40 km/c1.0/s11/slot10 | 38.66 | r2 / 299 m | km 17.68 | 335 m | Храстов запис · residential · 35 km/h | none within 150 m · +5149 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 19.36 | 315 m | Храстов запис · residential · 33 km/h | 77.4 m · residential · 35 km/h · +5149 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/40 km/c1.0/s23/slot11 | 38.66 | r2 / 299 m | km 17.68 | 335 m | Храстов запис · residential · 35 km/h | none within 150 m · +5149 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 19.36 | 315 m | Храстов запис · residential · 33 km/h | 77.4 m · residential · 35 km/h · +5149 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/40 km/c1.0/s42/slot11 | 38.66 | r2 / 299 m | km 17.68 | 335 m | Храстов запис · residential · 35 km/h | none within 150 m · +5149 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 19.36 | 315 m | Храстов запис · residential · 33 km/h | 77.4 m · residential · 35 km/h · +5149 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/40 km/c1.0/s7/slot11 | 38.66 | r2 / 299 m | km 17.68 | 335 m | Храстов запис · residential · 35 km/h | none within 150 m · +5149 m | same way, two passes | **H2** | fallback policy |
|  |  |  |  | km 19.36 | 315 m | Храстов запис · residential · 33 km/h | 77.4 m · residential · 35 km/h · +5149 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/60 km/c0.7/s42/slot9 | 71.77 | r2 / 323 m | km 30.77 | 307 m | Двадесет деветог новембра · service_other · 39 km/h | 36.3 m · residential · 35 km/h · +527 m | same way, two passes | **H2** | fallback policy |
| A-extra | A/vracar/60 km/c1.0/s101/slot11 | 40.81 | r0 / 0 m | km 18.2 | 865 m | Аутопут за Нови Сад · tertiary · 54 km/h | 25.1 m · tertiary · 30 km/h · +13857 m | two ways, twin pair | **H3** | P1.2 twin test |
|  |  |  |  | km 19.15 | 796 m | Аутопут за Нови Сад · tertiary · 50 km/h | 31.0 m · tertiary · 30 km/h | two ways, twin pair | **H3** | P1.2 twin test |
| P1.1 | B/(44.796, 20.437) demand#2/300 km/c0.5/s101/slot0 | 319.21 | r0 / 0 m | — (all runs < 300 m) | — | — | — | — | **H0** | Gate v2 / D1b |
| P1.1 | B/(44.796, 20.437) demand#2/300 km/c0.5/s11/slot0 | 319.21 | r0 / 0 m | — (all runs < 300 m) | — | — | — | — | **H0** | Gate v2 / D1b |

## 3. Fallback provenance — recoverable, and it settles three of the five hypotheses

**Yes, per-loop fallback provenance was recoverable.** The P1.1 run's engine ledger is captured at `~/.curvagen-scratch/p1.1/engine-p11-final.log` (929 blocks) and `engine-p11-final-last.log` (553 blocks). Each request emits

```
roundtrip rungs: r0=14 r1=0 r2=3 none=0
roundtrip ranking: rung tier then built-loop score — 0:r0/139/184/0/32 1:r0/114/135/0/18 …
                                                     slot:rTIER/score/loop-curviness/self-overlap-m/dist-err
```

The log does not name the request, so blocks were keyed to response files by their **12-slot distance-error fingerprint** against `loops.jsonl`. All 18 files carrying a stretch resolved, and every candidate block agreed on the slots in question (several requests were replayed in a second identical run, which is why some fingerprints are not unique — the ledger content is).

Tier `3` in the *ranking* line is the **gated** tier (§3/§4 of the P1.1 iteration doc), not `rung_hits[3]`.

| loop | tier | engine self-overlap | built score | request rungs r0/r1/r2/none |
|---|---|---|---|---|
| Vračar 40 km c0.5 s101 **slot 8** (worst-20 #1) | **r0** | **0 m** | 49 | 14/0/3/0 |
| Vračar 40 km c0.7 s101 **slot 7** (worst-20 #2) | **r0** | **0 m** | 49 | 15/0/3/0 |
| demand#4 70 km c1.0 s7 & s23 **slot 10** | **r0** | **0 m** | 16 | 15/1/4/0 |
| Vračar 60 km c1.0 s101 slot 11 | **r0** | **0 m** | 16 | 17/0/3/0 |
| zlatibor 200 km c0.5, 13 loops slots 2–7 | **r0** | **0 m** | 447–517 | — |
| zlatibor 200 km c0.5 s11 **slot 11** | **r3 = gated** | **7 374 m** | 404 | 14/0/21/0 |
| Vračar 25 km c0.5 & c0.7 s42 slot 11 | **r2** | 469 m | 115 | 16/0/4/0 |
| Vračar 40 km c1.0 ×5 seeds slot 10/11 | **r2** | 299 m | 163 | 15–17/0/3–12/0 |
| Vračar 60 km c0.7 s42 slot 9 | **r2** | 323 m | 233 | 12/0/5/0 |
| demand#2 300 km c0.5 s101 & s11 slot 0 | **r0** | **0 m** | — | 15/0/0/0 |

Two readings jump out.

1. **`self-overlap 0 m` on every H3 loop.** The engine believes those loops are perfectly clean while D1b reads 2 759 m of near-mirror. That is not a threshold problem — `roundtrip_gate_twin_ride_m` is 500 m and the meter reads *zero* — it is a **scope** problem, confirmed in code at `route_action.cc:1883-1914`: `fwd_ids` is filled from `L.fwd` only, and the overlap loop tests `L.ret` edges against it. Return-vs-return is not measured, so it is neither ranked down nor gated.
2. **Every H2 loop sits at r2 or gated, and every H2 overlap is under the 500 m gate** (299 / 323 / 469 m) except the zlatibor 6 979 m one, which the gate *did* catch (7 374 m) and which was then promoted anyway by the per-slot last resort (`rungs` shows only 14 clean legs for K = 12 slots plus refills).

## 4. The counterfactual: what the neighbour would actually have cost

For each instructive stretch, the read-only engine was asked to route between two points **900 m outside each end of the retrace**, at that loop's own curviness and motorway setting, twice: unconstrained, and with `exclude_locations` sampling **both traversals** (22 + 22 points) so the retraced pavement and its opposite carriageway are barred — i.e. exactly what a scope-corrected twin bar would do.

| stretch | base | without its own pavement | delta |
|---|---|---|---|
| M11 / E 70, 668 m (Vračar 40 km) | 2.317 km / 86 s | **no route** | — |
| M11 / E 70, 597 m | 2.570 km / 1 423 s | **no route** | — |
| Аутопут за Нови Сад, 865 m | 2.581 km / 182 s | 16.438 km / 984 s | **+13 857 m, ×5.4 time** |
| Аутопут за Нови Сад, 576 m | 2.024 km / 127 s | 16.665 km / 992 s | **+14 641 m, ×7.8 time** |
| Поблаће — Крајчиновићи, 6 979 m | 8.868 km / 835 s | **no route** | — |
| Маршала Тита, 325 m | 2.152 km / 318 s | 4.755 km / 1 430 s | +2 603 m, ×4.5 time |
| Храстов запис, 335 m | 1.650 km / 877 s | 6.799 km / 4 538 s | +5 149 m, ×5.2 time |
| **Двадесет деветог новембра, 307 m** | 5.120 km / 1 964 s | 5.647 km / 2 461 s | **+527 m, ×1.3 time** |

**Seven of the eight have no cheap alternative; one does.** The neighbour "that looks the same" is, on the two motorway-grade corridors, a severed frontage stub or a 14 km detour, and in the Zlatibor valley it does not exist at all. That is the honest half of the answer to Andrey: on those cases the map's parallel line is not a road you can ride from A to B.

## 5. The per-metre cost, from the fork's own tables

`MotorcycleCost::EdgeCost` (`src/sif/motorcyclecost.cc:474-504`), transitions excluded, at each loop's own costing (`curvagen-orchestrator/crates/domain/src/costing.rs`):

| case | road | factor | s/m | cost/m | ratio |
|---|---|---|---|---|---|
| Vračar 40 km, c0.5, motorways allowed (`use_highways` 0.65 → `highway_factor` **−0.0034**) | M11 trunk, 100 km/h, density 3 | 0.9233 | 0.0360 | **0.0332** | 1.00 |
| | frontage unclassified, 40 km/h, 47 m off | 0.9233 | 0.0900 | 0.0831 | **2.50×** |
| demand#4 70 km, c1.0, allowed (`use_highways` 0.30 → `highway_factor` 1.28) | Аутопут за Нови Сад tertiary, 54 km/h | 0.9750 | 0.0667 | **0.0650** | 1.00 |
| | Батајнички булевар tertiary, 30 km/h, 31 m off | 0.9750 | 0.1200 | 0.1170 | 1.80× |
| | Батајнички булевар tertiary, 50 km/h, **curvature 3**, 59 m off | 0.8550 | 0.0720 | 0.0616 | **0.95× (cheaper!)** |
| zlatibor 200 km, c0.5, motorways avoided | Поблаће unclassified, 39 km/h, curvature 8 | 0.8750 | 0.0923 | **0.0808** | 1.00 |
| | gravel track, 20 km/h, curvature 11, 28 m off | 0.8150 | 0.1800 | 0.1467 | 1.82× |
| Vračar 25 km, c0.5, allowed | Маршала Тита tertiary, 50 km/h | 0.9250 | 0.0720 | **0.0666** | 1.00 |
| | Прохорска / Стари гај residential, 35 km/h | 0.9233 | 0.1029 | 0.0950 | 1.43× |
| Vračar 60 km, c0.7, allowed | Двадесет деветог новембра tertiary, 50 km/h | 0.9000 | 0.0720 | **0.0648** | 1.00 |
| | Двадесет другог децембра residential, 35 km/h, 36 m off | 0.9000 | 0.1029 | 0.0926 | 1.43× |

Three things the table settles.

- **The class term never separates these pairs.** `kHighwayFactor` is `{motorway 1.0, trunk 0.5, primary 0, secondary 0, tertiary 0, unclassified 0.5, residential 0.5, service 0}`. Trunk and unclassified carry the *same* 0.5; tertiary and tertiary carry the same 0; and at c0.5 with motorways allowed `highway_factor` is −0.0034 anyway. **H4 as the ticket framed it does not exist in this corpus** — there is no road-class gap, only a speed gap.
- **The neighbour is not always dearer.** The curvier 50 km/h segment of Батајнички булевар is 5 % *cheaper* per metre than the carriageway that got re-ridden. Per-metre cost alone therefore cannot explain that case; connectivity (§4, +13.9 km) can.
- **Surface and curvature move the needle less than speed.** The only surface-driven case is the Zlatibor gravel track (`kSurfaceFactor[gravel] = 0.5`), and even there 39 vs 20 km/h dominates the 1.82×.

## 6. Per-case notes — the ten most instructive

**1–2. Vračar 40 km, c0.5 s101 slot 8 and c0.7 s101 slot 7 — the two worst loops in the gallery (2 759 m each).** Asked 40 km, built 45.2 km, forward leg 18.4 km, return leg **26.8 km for a 14.4 km straight-line gap (1.86×)**. The return spends the surplus on a ~6.5 km westward excursion along the **M11 / E 70** (turning around ~2.7 km west of the seam) and comes back along the same corridor on the opposite carriageway — 1 901 m of it at 13–25 m offset, at loop km 21.5, 23.0 and 26.5 (597 / 636 / 668 m). Chord/arc **1.000** — dead straight, which is exactly what Andrey saw. The pairs are **different OSM ways** (`672719302/672719304` ↔ `477498648/671802857`; `475315724` ↔ `135336021`) at a constant offset, so they pass the P1.1 switchback test and *are* registered twins; the twin bar simply never looks at them because both passes are in the return leg. Ledger: **r0, self-overlap 0 m**. Nearest non-M11 pavement is a slip road at 21 m and unclassified frontage at 27–50 m; barring the corridor leaves **no route**. **Verdict H3, with H5 (distance padding) as the reason the excursion exists at all and H4 (2.5×/m) as the reason the frontage road would never win on cost even if it were connected. H3 dominates: the machinery designed to override cost never fired.**

**3. Vračar 60 km c1.0 s101 slot 11 — the same shape, a different road.** 865 m out and 796 m back on **Аутопут за Нови Сад** with only **~95 m between the two passes** — a U-turn at a junction on a dual carriageway, the most rider-legible version of the defect. Ledger **r0 / 0 m**. Verdict **H3**.

**4–5. demand#4 (44.794, 20.491) 70 km c1.0, s7 and s23, slot 10 — H3 and H1 in one loop.** 2 237 m of Аутопут за Нови Сад return-vs-return (H3, as above) *plus* 480 m at km 0.0 and 485 m at km 43.9 of **Војислава Илића** ridden both ways at 1–2 m offset — 476 m of it literally the same pavement. Both sit at path-distance 0 from the ends, inside `kStartExemptionMeters = 1500`, so they are **H1: designed behaviour**, and the engine's own meter correctly ignores them (that is why it reads 0). The rider still sees the same street leaving and returning; whether that is a defect is a *policy* question for Gate v2, not an engine bug.

**6. zlatibor 200 km c0.5 s11 slot 11 — the only true F01 in the worst-20, and the gate caught it.** 6 979 m of **Поблаће — Крајчиновићи** (unclassified, 39 km/h) ridden out at km 78.7 and back at km 95.7, exact same pavement, 0.0 m offset, 78.7 km from either end. The request drove **21 of its legs to rung 2**; the loop was **gated** (tier 3, self-overlap 7 374 m ≫ the 500 m threshold) and then **promoted by the per-slot last resort** because the bank could not be filled otherwise. The counterfactual says why: with that pavement barred there is **no route** — a one-road valley. **Verdict H2** (the all-or-nothing rung 2), with the gate working exactly as designed and the last-resort promotion, not the gate, being what a rider sees.

**7. Vračar 40 km c1.0, all five seeds, slot 10/11 — the cul-de-sac turnaround.** 335 m out at km 17.7 and 315 m back at km 19.4 on **Храстов запис** (residential, 35 km/h), 300 m of it the same pavement, straddling the turnaround at km 19.3. Ledger **r2, self-overlap 299 m** — under the 500 m gate by 201 m, which is the only reason it was served. The alternative costs **+5 149 m and ×5.2 time**: the street is the only way in and out. **H2 dominant**; the gate threshold is the co-cause. That this reproduces identically across all five seeds says it is a network fact, not a search accident.

**8. Vračar 25 km c0.5 / c0.7 s42 slot 11 — where the twin tier bars its own escape.** 325 m + 358 m on **Маршала Тита / Сланачки пут** (tertiary, 48–50 km/h) at Slanci, 283 m same pavement. Ledger **r2, self-overlap 469 m** — 31 m under the gate. The neighbours here are real: Стари гај at 22 m, Прохорска at 32 m, Власуљарска at 40–50 m. But 22–32 m is *inside* the 30 m twin radius, so those streets are exactly what the twin tier hard-excludes alongside the corridor; the escape route and the thing being escaped are the same tier. The measured detour is +2 603 m, ×4.5 time. **H2 dominant, with a P1.2 note: the twin radius and the alternative supply overlap in dense grids.**

**9. Vračar 60 km c0.7 s42 slot 9 — the one case the fallback genuinely should not have taken.** 307 m on **Двадесет деветог новембра** (tertiary), 235 m same pavement, at km 30.8 of a 71.8 km loop. Ledger **r2, self-overlap 323 m**. The alternative — Двадесет другог децембра, residential, 35 km/h, **36 m away** — costs **+527 m and ×1.3 time**. This is the only stretch in 29 loops where the neighbour that looks the same *is* the neighbour, is connected, and is nearly free. It was lost because rung 2 drops **every** hard exclusion at once rather than the one that failed. **H2, and the single clearest argument for a graded rung.**

**10. zlatibor 200 km c0.5 — 13 of the 20 worst loops, and none of them is a retrace.** The three recurring stretches (395 m on road **192** near Бучје, 366 m on **195** near Стража, 383 m on **Калуђеровићи**) all report their **own index range as their partner**: the run is antiparallel to *itself*. Chord/arc 0.148, 0.212 and 0.454 — the road turns through ~180° inside 400 m. All three are a **single OSM way**. The ledger agrees: **r0, self-overlap 0 m** on every one. These are Zlatibor hairpins, ridden once, and the P1.1 switchback test is *correct* to leave them alone. **They are in the worst-20 because D1b measures radius and bearing without asking whether the two arms are the same pass.** No engine change removes them; a detector change does.

**Also: demand#2 (44.796, 20.437) 300 km c0.5, slots 0 (×2), worst-20 #19 and #20.** No stretch reaches 300 m at all. Nine runs of 200–290 m: five are self-folding hill switchbacks in the (44.0, 19.5) cell, four are 203–290 m on the Belgrade approach within 1.5 km of home. 1 615 m of D1b metres, 0 m of retrace. **H0 / sub-threshold.**

## 7. What to change, ranked by how many of these 29 cases it removes

| # | change | owner | removes | cost / caveat |
|---|---|---|---|---|
| **1** | **Make D1b switchback-aware** — reject a run whose partner index range overlaps its own, or require the partner to be ≥ N segments / ≥ 500 m away along the path. (The engine already knows how; port `road_twin_index.cc:290-313`'s min/max-offset idea, or just the index test, which is exact and free.) | Gate v2 metric / detector | **15 of 29 loops (13 + 2), 11 803 m of 42 551** — and the *entire top of the worst-20 ranking below rank 5* | Zero engine cost. This is a measurement fix: it changes what the gallery shows, not what riders get. It must land before any threshold is set on D1b, or Gate v2 will ratchet against Zlatibor's terrain. |
| **2** | **Give the twin/parallel identity a return-vs-return term** — build the overlap set from the return leg's own settled edges (plus their twins) as it is walked, not only from `L.fwd`; feed the same number to `L.self_overlap_m` so the built-loop rank and the `twin_ride` gate both see it. | P1.2 twin test | **5 loops, 9 937 m — including both worst-20 loops (2 759 m each) and the whole demand#4 residue** | The measured truth (§4) is that on the M11 and Novi Sad corridors there is **no cheap alternative**, so the practical effect is that the gate rejects the loop and the slot refills — it does *not* put the rider on the frontage road. Budget the refill (`thor.roundtrip_gate_refill_budget`) before enabling. |
| **3** | **Grade rung 2** — release the hard exclusion that actually blocked the route (or release by path-distance bands) instead of dropping all of them. | fallback policy | **up to 9 loops, 18 881 m**, but only **1 of 8** measured stretches had a cheap alternative (+527 m, ×1.3) | The other seven have no cheap way home; for those the right lever is #4, not the rung. Expect ~1 in 8 to convert. |
| **4** | **Lower `roundtrip_gate_twin_ride_m` 500 → 250** | Gate v2 | gates the three H2 loops at 299 / 323 / 469 m | Does **nothing** for the top of the worst-20 until #2 lands — the meter reads 0 there. And gating without a replacement just spends refill budget or promotes via the last resort, which is what already happened to the 7 374 m zlatibor loop. |
| **5** | **Start Exemption policy** — decide whether 480 m of the same street at both ends of a 70 km ride is a defect | Gate v2 | 2 loops' 1 930 m, never dominant | Pure policy. The engine is doing what ADR-0037 §3 asked. |
| **6** | **Costing** | — | **0** | No class gap exists: `kHighwayFactor[trunk] == kHighwayFactor[unclassified] == 0.5`, and at c0.5/allowed `highway_factor` is −0.0034. The 1.4×–2.5× gaps are speed. Changing the class table to separate trunk from unclassified would be a real lever for *future* cases, but it would not have changed any loop here — connectivity, not cost, decided seven of eight. |

**For P2 (H5, not a fix here but the shape behind cases 1–5).** Vračar 40 km turns around 14.4 km from home (straight line) and then rides 26.8 km back — a **1.86× detour ratio**; demand#4 70 km reads 25.2 km against 16.7 km, **1.51×**. Every H3 case in this set is a *distance-padding excursion* whose cheapest execution is out-and-back on the nearest fast road. Ranking already penalises `dist_err` (Wd = 1); it does not penalise the return leg's own detour ratio. A detour-ratio term would attack the cause rather than the symptom.

## 8. What this does not say

- The neighbour roads were sampled with `/locate` at five points per stretch, radius 150 m, filtered to edges 20–150 m away whose heading is within 30° of the stretch's chord (or its reverse), excluding the retraced ways themselves. Service roads, driveways and paths were dropped. That finds *pavement*, not *routes* — which is why §4's counterfactual is the load-bearing test.
- The per-metre costs in §5 exclude `TransitionCost`, the rejoin grade, the cross-candidate penalty and the 4.2× reuse leash. They compare free pavement to free pavement, which is the right comparison for "why is the neighbour not chosen on its own merits" and the wrong one for reproducing the engine's actual A\*.
- The corpus ran with `roundtrip_xcand_penalty` **off** (the rig's `valhalla.json` carries no `roundtrip_*` key; prod runs it on at 0.2 — P1.1 iteration doc §0). The cross-candidate penalty would push later slots off already-ridden pavement, so the H2 cases at slots 9–11 may be milder in production than they are here. The H3 cases sit at slots 7, 8, 10, 11 too — but their pavement is re-ridden *within one loop*, which the cross-candidate memory does not see either.
- D1b's `mech_m` (`am25_new_m`, `min_run_m = 200`) and this document's 300 m inventory differ by design; the worst-20 ranking uses the former.

---

## Appendix A — commands

All paths absolute. `python3` = system CPython. Nothing under `tools/loopqual/` was modified; the two artefacts written are this file and `results/p1-1-road-identity/gallery-same-road.html`.

```bash
# read-only prod-equivalent engine on :8005 (NOT :8002/:8003/:8004/:8791)
docker run -d --name rt-nbr-8005 -p 8005:8005 \
  -v /Users/xenix/Projects/curvagen-orchestrator/data:/custom_files:ro \
  -e use_tiles_ignore_pbf=True -e serve_tiles=True -e server_threads=2 \
  valhalla-curvature:prod-equivalent-b4f514d7f \
  bash -lc "sed 's|tcp://\*:8002|tcp://*:8005|' /custom_files/valhalla.json > /tmp/valhalla-8005.json && exec valhalla_service /tmp/valhalla-8005.json 2"
curl -s localhost:8005/status     # {"version":"3.8.2","tileset_last_modified":1785932567,…}
# … and, at the end:
docker rm -f rt-nbr-8005
```

```bash
# road identity of a retrace stretch and of its second pass
curl -s localhost:8005/trace_attributes -d '{"shape":[…],"costing":"motorcycle",
  "shape_match":"map_snap","filters":{"action":"include","attributes":[
  "edge.way_id","edge.names","edge.road_class","edge.speed","edge.speed_limit",
  "edge.surface","edge.use","edge.length","edge.density","edge.lane_count"]}}'

# every road within 150 m of a point, with curvature bucket and grade
curl -s localhost:8005/locate -d '{"locations":[{"lat":…,"lon":…,"radius":150}],
  "costing":"motorcycle","verbose":true}'      # edge.geo_attributes.curvature, edge.classification

# the counterfactual: cross the stretch without its own pavement
curl -s localhost:8005/route -d '{"locations":[A,B],"costing":"motorcycle",
  "costing_options":{"motorcycle":{…crates/domain/src/costing.rs at this curviness…}},
  "exclude_locations":[…22 samples of each traversal…],"units":"km"}'
```

```bash
# retrace stretches >= 300 m, per loop (pinned detectors, read-only import)
#   lqbs_lib.antimirror_runs(loop, xy, radius_m=25.0, min_run_m=300.0)
#   lqbs_lib.zone_of(loop, cum_lo, cum_hi)            -> exempt / seam / mid
#   run['partner_lo'], run['partner_hi']              -> the switchback test of §1
python3 -c "import sys; sys.path.insert(0,'/Users/xenix/.curvagen-scratch/p1'); …"

# ledger provenance: parse the ranking lines, key them by the 12-slot dist-err fingerprint
grep -a 'roundtrip ranking:' ~/.curvagen-scratch/p1.1/engine-p11-final.log      # 929 blocks
grep -a 'roundtrip ranking:' ~/.curvagen-scratch/p1.1/engine-p11-final-last.log # 553 blocks
grep -a 'roundtrip rungs:'   ~/.curvagen-scratch/p1.1/engine-p11-final.log
```

```bash
# the 10-case mini-gallery (Leaflet from ../leaflet.{js,css}, [lat, lon] order)
open /Users/xenix/Projects/curvagen-valhalla/tools/loopqual/results/p1-1-road-identity/gallery-same-road.html
```

## Appendix B — primary sources

- `tools/loopqual/results/p1-1-road-identity/{loops.jsonl,report.md,gallery-p1-1.html,responses/}` — the P1.1 corpus (552 requests, 6 602 loops, 2026-09-06T03:13:50, engine mode, K = 12)
- `tools/loopqual/corpus-v2.yaml` — block A/B run `avoid_motorways: false`, block C and `vracar_amw` run `true`
- `~/.curvagen-scratch/p1.1/{engine-p11-final.log,engine-p11-final-last.log,run-final.log}` — the ledger
- `~/.curvagen-scratch/p1/lqbs_lib.py` — `antimirror_runs`, `zone_of`, `xy_frame` (Appendix A of `2026-09-05-loopqual-blind-spots.md`)
- `src/thor/route_action.cc` — `kStartExemptionMeters` (`:1000`), tiered identity and the rung ladder (`:1560-1775`), the built-loop score and its **forward-only** overlap set (`:1831-1925`), the geometry gate (`:2009-2050`), the per-slot last resort (`:2260-2285`)
- `src/thor/road_twin_index.cc` — twin radius 30 m / parallel 80 m (`src/thor/worker.cc:84-85`), the switchback test (`:30-52`)
- `src/sif/motorcyclecost.cc` — `kHighwayFactor` (`:69-77`), `kSurfaceFactor` (`:82-89`), `kCurvatureFactor` (`:92-112`), `EdgeCost` (`:455-504`); `valhalla/sif/dynamiccost.h` — `kDensityFactor` (`:225-234`), `kMaxHighwayBiasFactor` (`:200`), `SpeedPenalty` (`:1173-1191`)
- `curvagen-orchestrator/crates/domain/src/costing.rs` — the served curviness → costing map
- `docs/curvagen/research/2026-09-06-p1-1-road-identity-iteration.md` §0, §1, §3, §4, §5, §8.4, §8.5, §11

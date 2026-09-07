# 0041 — Round-trip v4 loop-quality gate (Gate v2): rider-calibrated, three tiers, judged on the served surface

- **Status:** Accepted (2026-09-07); **amended 2026-09-07** — T2's comparison set, T3 for the v4 cutover, T5's surface (see *Amendment* below; ticket [#16](https://github.com/Lazark0x/curvagen-valhalla/issues/16))
- **Context:** Wayfinder map [curvagen-valhalla#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4) (round-trip v4), ticket [#7](https://github.com/Lazark0x/curvagen-valhalla/issues/7). Gate v1.2/v1.3 (ADR-0037) passed every absolute at every curviness on a corpus built from real demand while 64 % of demand-cell loops carried ≥ 500 m of retrace no gate input could see (defect atlas v2, #5). The 180-loop Rider-Verdict Calibration Set (#6, labeled blind by Andrey) then measured the detector bank against the rider: any detector at its pinned threshold catches 64 of the 65 loops he rejects and fires on 54 of the 115 he accepts (precision 0.54). Rings are not a rider defect (20/20 OK); the exempt stem is not (19/20); the parallel-carriageway near-mirror — the census's headline residual — is rider-visible 44 % of the time; same-pavement and reuse retraces are (89 % / 100 %). Same-pavement runs and reuse outside the exemption agree with the rider at F1 0.73–0.75; nothing else exceeds 0.6.

## Decision

1. **Three tiers.** A **reject tier** of absolutes calibrated on the Rider-Verdict Calibration Set; **ratchets** against Baseline v2 for what a rider cannot judge per loop; an **advisory tier** that is reported and never blocks. A meter earns a blocking role by agreeing with the rider, not by firing.
2. **Surface, corpus, baseline, configuration.** The blocking tier is judged on the **Served Surface** — slots 0–5 of the Candidate Bank (three served synchronously, the first Bank tap) — of blocks A (Vračar) and B (production demand cells) of corpus-v2 in rider mode, at c0.5 / c0.7 / c1.0. Block C (corpus-v1 origins, motorways avoided), slots 6–11 and c0.8 are reported. **Baseline v2** is the census run (`b4f514d7f`, corpus-v2, metrics v2 with the switchback-aware near-mirror read), re-read whenever a meter changes. Every ratchet is measured with **both engines at the production configuration** (`roundtrip_xcand_penalty` on at 0.2), same-session brackets.
3. **Reject tier (absolutes).**

   | # | bar | threshold | Baseline v2 | P1.1 knee | P2 v2 |
   |---|---|---|---|---|---|
   | R1 | **Retrace family** — share of served loops with a same-pavement run ≥ 500 m (D1) **or** reuse outside the Start Exemption ≥ 500 m (D4) **or** a near-mirror run ≥ 1.5 km (D1L) | ≤ 5 % (the rider's reject-tier rate on this surface) | 43.9 % | 2.4 % | 3.8 % |
   | R2 | exact-mirror stub at the Turnaround Seam (`spike_ge_500m`) | 0 at every level | 0 | 0 | 0 |
   | R3 | distance error (Gate v1.3 row 7) | mean ≤ 0.22 / p90 ≤ 0.42 at c0.5; 0.32 / 0.65 above | pass | pass | pass |
   | R4 | fills | K = 12 on every request | 550/552 | 552 | 552 |
   | R5 | failures | 0 | 0 | 0 | 0 |

4. **Ratchets vs Baseline v2 / prod.**

   | # | bar | threshold | P1.1 knee | P2 v2 |
   |---|---|---|---|---|
   | T1 | curviness retention per demand level | ≥ 0.95× | 1.01 / 1.07 / 1.04 | 1.05 / 1.28 / 1.22 |
   | T2 | **Served-Surface Distinctness** — each served loop against the *other served loops* of its bank (slots 0–5 vs 0–5; A+B, prod config). *Amended 2026-09-07: the original read compared against all twelve and is now the advisory Bank Distinctness row.* | mean ≤ 1.10× prod **and** share > 0.6 ≤ prod + 5 pp (prod **0.3171 / 11.6 %** → ≤ 0.3488 / ≤ 16.6 %) | not re-read | P2 0.5524 / 45.5 % ✗ · **P2.1 d 0.3370 / 13.8 % ✓** (1.063× / +2.2 pp) |
   | T3 | wall p50, pooled over corpus-v2 (same-session brackets) | ≤ 1.10×; **for the v4 cutover ≤ 1.25× vs Baseline v2** (*amended 2026-09-07*; the next revision re-pins the baseline to v4's own production-equivalent reading — Baseline v3 — and returns the ratchet to ≤ 1.10× against it) | 1.227× ✗ | P2 0.93× · **P2.1 d 1.22× ✓ (v4 bar)** |
   | T4 | wall p95 on blocks A+B | ≤ 1.30× | ~1.08× | 1.17× |
   | T5 | near-mirror magnitude: mean unseen metres on the Served Surface (A+B slots 0–5). *Erratum 2026-09-07: the 3 462 m first quoted was the all-blocks slots 0–2 read.* | ≤ 0.25 × Baseline v2 (**4 792 m → ≤ 1 198 m**) | 43 m | P2 58 m · **P2.1 d 36 m ✓** |
   | T6 | deep bank (slots 6–11): Retrace family share | ≤ Baseline v2's (53 %) | 14.3 % | 4.1 % |

5. **Canaries** (reported, flag a regression): `spike_ge_30m`; `edge_reuse_geom` (Gate v1.3 rows 3–5). **Advisory**: Rings (D3 / D3b, with the fixed counter), the stem-lollipop rows 6a/6b, block C, c0.8, 200–300 km asks and per-ask p95, D1b share per block, **Bank Distinctness** (each served loop against any loop of the bank — T2's original read; prod 0.392 / 14.4 %, P2.1 d 0.4443 / 24.3 %) and the per-leg split of the served overlap (forward vs return). **Retired**: the exempt-corridor meter (D2) and the distance-lobe meter (D5).
6. **The Start Exemption is unchanged** — exempt, at the shared constant. The calibration says the forced stem is not something riders mind.
7. **Provenance leaves the engine.** The fork's round-trip response gains an additive per-candidate `provenance` field (pair-built / rescue / fallback rung / gated tier); the harness's D4 becomes a true read and the served-tier check reads it; the orchestrator ignores the field (no app contract change); no `FINGERPRINT_VERSION` bump until the v4 cutover bumps it anyway.
8. **Home.** Metrics v2 (the Retrace family, the switchback-aware near-mirror read, the fixed ring counter, provenance) and `gate_v2.py` land in `tools/loopqual` inside the v4 build ticket; this record pins the thresholds.
9. **Vocabulary.** *Retrace*, *Spike* (narrowed to the seam canary), *Ring*, *Served Surface*, *Rider-Verdict Calibration Set*, *Loop-Quality Gate (Gate v2)* and *Baseline v2* are written to the orchestrator's `CONTEXT.md` (Backend Serving Context); the product terms Spur / Backtracking / Lollipop stay in curvagen-meta.

## Why precision-first, why the served surface

The rider's tolerance is wider than the meters': 64 % of prod's loops are rides he would take, only 12 % are rejects. A gate built on "any detector fires" would reject 47 % of acceptable loops (58 % at severity ≥ 2), and at K = 12 that is paid in fills and refill latency — the P1.1 budget sweep and P2's built-loop filter measured exactly that price. Gating the served surface keeps the bar where the rider reads it (OK 77 % in slots 0–2 against 39 % in slots 9–11 — the ranking already sinks the defects) and lets the deep bank be what it is: reserve. The retrace family is the only meter group whose fires the rider recognises (F1 0.76–0.81; recall 1.00 on every severity-3 loop); everything the census ranked by prevalence — near-mirror metres, rings, crossings — ranks differently by rider verdict.

## What this does to the ladder

Neither candidate passes today. The P1.1 knee fails T3 (1.227×, measured twice as unfixable on its mechanism) and T2 narrowly (1.14× / +7.6 pp). P2 fails T2 structurally (half of its served loops have a > 60 % twin: pair-eligible sinks cluster on the same trunk roads out of the start) and passes everything else. P2 stays the selection stage; **P2.1** = distinctness at selection, with T2 as its kill criterion while R1–R5 and T1, T3–T5 hold.

## Considered and rejected

- **Whole-bank (K = 12) gating, v1 style** — judges nine loops the rider never reads and is what cost P1.1 its latency and P2 its fills when enforced.
- **Blocking on the near-mirror meter or on rings** (the census's proposals M3 / M6) — rider-visible 44 % / 0 % of the time; they stay as engineering targets (T5) and reports.
- **Ratchet-only gate** — certifies prod's 44 % served retrace rate as the floor.
- **Absolutes-only gate** — leaves distinctness, latency and curviness unguarded.
- **The "dirty baseline" objection to ratchet 9** (that prod's distinctness is inflated by fallback loops) — measured negligible: excluding every loop with `edge_reuse_geom > 0.3` moves prod's `bank_overlap_mean` by 0.006 on the bank and 0.000 on the served surface.

## Amendment 2026-09-07 — the P2.1 front (ticket #16)

**Context.** Prototype P2.1 ([#14](https://github.com/Lazark0x/curvagen-valhalla/issues/14), `docs/curvagen/research/2026-09-07-p2-1-distinctness-at-selection.md`, branch `proto/v4-p2.1`) put distinctness *at selection* on top of the pair pass — a per-leg forward-sharing threshold on the pair's tree path, the whole-pair 0.6 test keyed on the built bank, a relaxation ladder for fills (relaxed-last, rescue-last ranking) and one bounded evaluation budget per request — and measured a Pareto front: the served forward legs reach prod's distinctness, the whole-bank T2 read stops at 0.4443 / 24.3 % (1.133× / +9.9 pp) because half of the remaining near-dups have their best sibling in slots 6–11 (the ladder's fills, which exist only to keep R4 at 552/552; prod: 20 %), and every point near T2 costs 1.22–1.34× at the median (the return builds of less-connected sinks, not the evaluation walk). Andrey's gallery verdict on the paired Vračar set and the worst-20 was *keep* ("even the worst examples are almost perfect routes"). Decided one question at a time on #16:

1. **T2 compares served loops among themselves.** The ratchet's purpose is the distinctness a Rider can hold side by side; comparing a served loop against loops the Rider reaches only after three Bank taps charged the fills for existing. T2 is now **Served-Surface Distinctness** (glossary, orchestrator `CONTEXT.md`): each Served-Surface loop's largest exemption-discounted overlap with another Served-Surface loop of its bank, mean and share > 0.6, A+B, prod configuration. Prod re-pinned at **0.3171 / 11.6 %**; bars ≤ 0.3488 / ≤ 16.6 %. The original whole-bank read stays as the advisory **Bank Distinctness** row so the deep bank cannot rot unseen. A provenance-aware read (fills excluded) was considered and rejected: it couples the gate to a field the build has not shipped.

   | run | Bank Distinctness (whole bank, original read) | **Served-Surface Distinctness (0–5 vs 0–5)** | vs prod | near-dups whose best sibling is in slots 6–11 |
   |---|---|---|---|---|
   | Baseline v2 / prod (xcand 0.2) | 0.3923 / 14.4 % | **0.3171 / 11.6 %** | — | 20 % |
   | P2 + xcand 0.2 | 0.5879 / 50.4 % | 0.5524 / 45.5 % | 1.74× / +33.9 pp ✗ | 17 % |
   | P2.1 c | 0.4521 / 22.4 % | 0.3512 / 12.2 % | 1.107× / +0.6 pp ✗ | 48 % |
   | **P2.1 d** | 0.4443 / 24.3 % | **0.3370 / 13.8 %** | **1.063× / +2.2 pp ✓** | 48 % |

2. **T3 for the v4 cutover is ≤ 1.25× vs Baseline v2, with T4 ≤ 1.30× held.** The 1.10× ratchet guarded regressions of the same product; v4 changes the product (no parallel-carriageway retrace, distinct loops) and the Rider judged that trade on the gallery. The exception is a cutover exception, not the new normal: the next gate revision re-pins the baseline to v4's own production-equivalent reading (Baseline v3) and returns the ratchet to ≤ 1.10× against it. **Debt line** (levers measured to matter, none gated): canonical-id caching on the pass's arcs (the tile lookups in the evaluation walk), degree-2 junction folding for the 200–300 km pass cost, the rescue-build share (3.4 per request vs P2's 0.9). A latency prototype before the build was rejected: it would delay a shippable engine for an unmeasured lever.
3. **T5's surface is the Served Surface** (A+B slots 0–5): Baseline v2 = 4 792 m, bar ≤ 1 198 m. The 3 462 m first quoted was read on all blocks at slots 0–2; every P2-family run passes both by two orders of magnitude.
4. **The engine the v4 build carries** is `proto/v4-p2.1` @ `f4a50bdbb` in P2.1's *d* configuration: per-leg forward-sharing 0.35 laddered (+0.15, +0.30, off), built-loop bank keys, whole-pair 0.6 against the built bank, one evaluation budget of 1 200 per request with 150 fresh evaluations per relaxation rung, relaxed-last and rescue-last ranking, `roundtrip_xcand_penalty` 0.2 cap 4 — all default-off knobs on the branch, gurka 51 green. *c* (budget 2 000, no rescue-last) sits at 1.107× on the amended read and was not chosen.
5. **Fills stay at K = 12.** The relaxation ladder keeps R4 at 552/552; relaxed loops reach the Served Surface only when nothing distinct remains; the advisory Bank Distinctness row and the provenance field (v4 build) keep the fills visible. Honest under-fill was rejected as a contract question (short banks in the app), not a gate one.
6. **The return-repair residual is deferred.** Served returns share 0.210 of the loop with a sibling against prod's 0.151 (forward legs 0.234 vs 0.242): the returns are `route_leg` repairs built after selection that no selection-time test sees. Levers exist and are unmeasured on top of P2.1 (a stronger cross-candidate surcharge on the repairs; ADR-0040's item-4 filter on repairs only); the advisory per-leg split watches it; a later item may take it. No ticket on the round-trip v4 map.
7. **Everything else stands** — the reject tier, T1, T4, T6, the canaries, the Start Exemption, provenance leaving the engine, the home of metrics v2 / `gate_v2.py` (the v4 build task, [#15](https://github.com/Lazark0x/curvagen-valhalla/issues/15), whose spec is this amended table).

**What this does to the ladder.** P2.1 d passes the amended Gate v2 on every tier; the v4 build task is unblocked with this table as its acceptance; cutover remains fog on the map (tile rebuild if the twin map goes `TaggedValue`, GitHub Actions re-enabled on the fork, the orchestrator `FINGERPRINT_VERSION` bump, deploy on Andrey's go with warm rollback).

## References

- `docs/curvagen/research/2026-09-05-loopqual-blind-spots.md` §8 (metrics v2 proposal), `2026-09-06-defect-atlas-v2.md` §8 (Baseline v2 candidate, prevalence curves), `2026-09-06-p1-1-road-identity-iteration.md` §12–§16, `2026-09-06-p2-suurballe-whole-loop.md` §7–§12.
- Calibration: `tools/loopqual/results/census-v2-b4f514d7f/calibration/{labels.jsonl,agreement-aware.md,agreement-aware-sev2.md,agreement-legacy.md}` and `disagreements.html` (local rig, gitignored).
- Tickets: #5 census, #6 calibration, #7 this decision, #10 / #12 / #11 the prototypes, #8 direction; #14 P2.1 (the front), #16 the amendment.
- Amendment inputs: `docs/curvagen/research/2026-09-07-p2-1-distinctness-at-selection.md` §5 / §8 / §9 (`proto/v4-p2.1`), the served-vs-served reading (`~/.curvagen-scratch/p21/served_vs_served.py`, local rig), the paired galleries `tools/loopqual/results/p2-1-d/gallery-p2-1-{vracar-paired,worst20}.html` (local rig).

# 0041 — Round-trip v4 loop-quality gate (Gate v2): rider-calibrated, three tiers, judged on the served surface

- **Status:** Accepted (2026-09-07)
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
   | T2 | **served-surface distinctness** (A+B slots 0–5, prod config) | `bank_overlap_mean` ≤ 1.10× prod **and** `near_dup > 0.6` ≤ prod + 5 pp (prod 0.392 / 14.4 %) | 1.14× / +7.6 pp ✗ | 1.50× / +36 pp ✗ |
   | T3 | wall p50, pooled over corpus-v2 | ≤ 1.10× | 1.227× ✗ | 0.93× |
   | T4 | wall p95 on blocks A+B | ≤ 1.30× | ~1.08× | 1.17× |
   | T5 | near-mirror magnitude: mean unseen metres on the served surface | ≤ 0.25 × Baseline v2 (3 462 m) | 44 m | 44 m |
   | T6 | deep bank (slots 6–11): Retrace family share | ≤ Baseline v2's (53 %) | 14.3 % | 4.1 % |

5. **Canaries** (reported, flag a regression): `spike_ge_30m`; `edge_reuse_geom` (Gate v1.3 rows 3–5). **Advisory**: Rings (D3 / D3b, with the fixed counter), the stem-lollipop rows 6a/6b, block C, c0.8, 200–300 km asks and per-ask p95, D1b share per block. **Retired**: the exempt-corridor meter (D2) and the distance-lobe meter (D5).
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

## References

- `docs/curvagen/research/2026-09-05-loopqual-blind-spots.md` §8 (metrics v2 proposal), `2026-09-06-defect-atlas-v2.md` §8 (Baseline v2 candidate, prevalence curves), `2026-09-06-p1-1-road-identity-iteration.md` §12–§16, `2026-09-06-p2-suurballe-whole-loop.md` §7–§12.
- Calibration: `tools/loopqual/results/census-v2-b4f514d7f/calibration/{labels.jsonl,agreement-aware.md,agreement-aware-sev2.md,agreement-legacy.md}` and `disagreements.html` (local rig, gitignored).
- Tickets: #5 census, #6 calibration, #7 this decision, #10 / #12 / #11 the prototypes, #8 direction.

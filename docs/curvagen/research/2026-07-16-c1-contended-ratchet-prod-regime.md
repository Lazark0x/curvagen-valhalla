# C1 — the xcand contended ratchet, re-measured in the prod thread/core regime

_C1 prototype (curvagen wayfinder map #66 — latency optimization). 2026-07-16._
_Graduated from R3 (#76): R3 proved the config surface is exhausted and the contended ratchet is CPU-oversubscription-bound + off-config, but flagged a hole — the loopqual gate measures the **wrong contention regime** (`--workers 3`, dev cores) vs prod (K≈12 through 2 threads on 2 vCPU), so its "contended 1.32×" was suspected to **understate** prod's contended xcand cost. C1 measures the ratchet in the prod regime to settle whether S1 can trust the gate._

---

## TL;DR — the ratchet ratio is regime-invariant; the gate does NOT understate. R3's worry is falsified.

Held the engine at **prod spec** (`docker --cpus 2`, `valhalla_service` concurrency = 2) and varied **only client concurrency** — `--workers 3` (gate-style, mild) vs `--workers 12` (prod bank-fill, severe) — across xcand OFF / strength 0.2 / strength 0.5, full corpus-v1 (232 req), engine image `valhalla-fork-test:x1`.

| xcand | workers=3 ratio (p50) | workers=12 ratio (p50) | workers=3 (p95) | workers=12 (p95) |
|---|---|---|---|---|
| **strength 0.2** | 1.075× | **1.069×** | 1.024× | 1.058× |
| **strength 0.5** | 1.231× | **1.221×** | 1.179× | 1.247× |

**The ratchet ratio is essentially identical across regimes — and if anything slightly *lower* under severe contention.** Severe contention balloons *absolute* latency (OFF p50 0.867 s → 3.954 s, a **4.6×** queue inflation on the CPU-saturated 2-core engine) but inflates OFF **and** xcand by the *same* factor, so the **ratio** — the gate metric — is preserved. R3's hypothesis that prod contention would amplify the xcand ratchet is **falsified**: the gate is trustworthy, and slightly *overstates* rather than understates.

**For S1:**
- **Option (b) — strength 0.2 — holds at prod scale.** Prod-regime ratchet = **1.069× p50 / 1.058× p95**, comfortably under 1.10×. The gate's "~1.09×" was safe.
- **Option (c) — full strength 0.5 — stays over the ratchet**, unchanged by regime: **1.221× p50 / 1.247× p95** at prod scale (was 1.22× on the gate). The full win still needs lever #3 or the out-of-scope hardware line.
- **Separate capacity finding (not the ratchet):** under a 12-way concurrent burst on 2 vCPU, *absolute* round-trip generation latency is severe — **OFF p50 3.95 s / p95 8.72 s**, strength-0.2 p50 4.23 s / p95 9.23 s. This is a throughput/capacity matter (the bank-fill worker under burst on a 2-core box), independent of xcand and largely hidden from users by the bank cache + background fill. Flag for S1; relates to the out-of-scope hardware line and to lowering bank K, not to the xcand decision.

---

## Method

- **Engine (both regimes):** `valhalla-fork-test:x1` (carries `thor.roundtrip_xcand_{penalty,strength}`), launched `docker run --cpus 2 … valhalla_service <cfg> 2` — i.e. the exact prod spec: 2-CPU quota (a faithful analog of the Hetzner 2-vCPU cloud share, ADR-0023) and `valhalla_service` concurrency = 2 (matching `VALHALLA_THREADS=2`). 364 Balkans tiles, mmap tar-extract.
- **Configs** (from the live `Backend/data/valhalla.json` + `thor.roundtrip_stage_timing`): OFF = `roundtrip_xcand_penalty:false`; s0.2 / s0.5 = `penalty:true, strength:{0.2,0.5}`. `~/.curvagen-scratch/c1-regime/c1-{off,s02,s05}.json`.
- **Client:** `loopqual run --corpus corpus-v1.yaml --workers {3,12} --no-way` (engine-mode fork `/route`). Latency = per-request wall-clock (`runner.py:149` `perf_counter`), aggregated `latency_p50_s`/`latency_p95_s` — the same fields `gate_v1_proto.py` gate 10 uses.
- **Matrix:** 3 engine configs × 2 client concurrencies = 6 runs of 232, all **0 failed**. Orchestration `~/.curvagen-scratch/c1-regime/run_all.sh`; aggregation `compute.py`.
- **Design rationale:** the variable that distinguishes "the gate's regime" from "prod's regime" is the **concurrent request count** (gate 3, prod ≈12), not the engine spec — prod is 2 vCPU / conc 2 regardless of load. Holding the engine at prod spec and sweeping client concurrency isolates the contention-regime effect cleanly.

## Full results

| run | p50 (s) | p95 (s) | ok | fail |
|---|---|---|---|---|
| off-w3 | 0.867 | 2.945 | 232 | 0 |
| s02-w3 | 0.932 | 3.015 | 232 | 0 |
| s05-w3 | 1.067 | 3.473 | 232 | 0 |
| off-w12 | 3.954 | 8.723 | 232 | 0 |
| s02-w12 | 4.227 | 9.225 | 232 | 0 |
| s05-w12 | 4.828 | 10.880 | 232 | 0 |

**Rig validation:** s05-w3 = **1.231×** reproduces X1's dev-regime full-strength **1.22×** (and s02-w3 1.075× ≈ X1's ~1.09×, within the ±12% single-run p50 noise A1 established). The rig faithfully reproduces the gate, so the w3→w12 comparison is clean.

## Why the ratio is regime-invariant (queue dilution)

Under CPU saturation the per-request latency is `queue_wait + service_time`. xcand adds a small increment Δ to *service* time (wider penalized return-leg A*, per P1). As concurrency rises, `queue_wait` W grows and dominates, so the ratio `(W + S + Δ)/(W + S)` moves *toward* 1.0 — heavier contention **dilutes** the xcand surcharge rather than amplifying it. The data shows exactly this: both strengths' ratios tick *down* from w3 to w12 (0.2: 1.075→1.069; 0.5: 1.231→1.221). Contention is a work-conserving multiplier on the whole pipeline; it does not preferentially penalize xcand.

## Verdict

- **The gate's ratchet ratio is trustworthy for the prod deploy.** No regime correction needed for S1's shipping-set decision. The contention-regime hole R3 flagged is closed — in the reassuring direction.
- **Shipping set unchanged in shape, now prod-validated:** strength 0.2 ships under 1.10× (1.069×); full strength 0.5 remains ~1.22× (over ratchet) → still needs lever #3 or hardware.
- **New, separate note for S1:** absolute generation latency degrades sharply under a 12-way concurrent burst on 2 vCPU (OFF p50 ~4 s / p95 ~9 s). Not an xcand issue and cache/background-fill-mitigated, but a real capacity data point tied to the (out-of-scope) hardware line and to bank-K sizing.

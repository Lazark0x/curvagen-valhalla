# M1: capacity metric + current-prod burst evidence

**Ticket:** [curvagen #83](https://github.com/Lazark0x/curvagen/issues/83) (wayfinder map #81)
**Date:** 2026-07-16 · **Sources:** orchestrator source (`Backend/orchestrator`), prod box `37.27.86.38` — nginx access history (full 5-week container stdout), orchestrator logs, `cache.db` read-only.

## TL;DR

1. **Verdict: the 2-vCPU burst latency is NOT user-visible today.** Real traffic is single-user and serial — 115 `POST /round-trip` requests in 5 weeks (10 active days), max concurrency **2 in the same second / 4 per 10 s window**, once each. Post-v3 (Jul 14+): cache hits 3–15 ms, cache-miss generates **p50 0.77 s / p95 2.45 s / max 2.59 s** — nowhere near C1's synthetic 12-way numbers (p50 ~4 s), because prod never enters that regime.
2. **The C1 burst scenario is structurally synthetic for today's serving path.** A user miss costs one *synchronous K=3* engine call; the K=12 expansion happens only in the background **single-flight** fill worker (one at a time, queue cap 4, "serving never waits on this queue"). Engine concurrency at current traffic ≈ ≤1 fill + ≤1 sync miss. Twelve concurrent *user misses* would be needed to reproduce C1-severe — that's a multi-user future, not the present.
3. **Metric of record defined** (below) — one-pass minable from nginx, no new instrumentation needed for the watch.

## 1. How a request actually flows (source-verified)

`crates/api/src/cache_layer.rs` / `handlers.rs`:

- Shuffle request (no seed): consume up to `SERVE_K = 3` banked candidates (`bank_consume`, :261) → **hit = SQLite read, ms-scale** ("round-trip bank serve").
- Miss → **synchronous engine call, `num_candidates = 3`** (handlers.rs:80/:162) — *this is the only user-visible generation wait*; 30 s reqwest total timeout.
- After a generated 200: memo (seeded) + **enqueue Bank Fill** — same-seed **K=12** engine call on a **single-flight-per-prefix, one-worker queue** (`FILL_QUEUE_CAP = 4`; dropped if full, skipped if in flight; cache_layer.rs:397–439). Fill banks slots 4–12.
- Every request UPSERTs `origin_stats(prefix, cell, hits, misses)` — persistent counters, **no timestamps** (rates only, no waits).

So the engine's worst concurrent load from organic traffic = (concurrent user misses × K=3 sync) + (1 × K=12 fill). C1's 12-way client fan-out models a *bank-fill benchmark*, not this path.

## 2. Prod evidence (5-week nginx history, origin-side `request_time`)

Nginx logs every request with upstream-inclusive `request_time` (last field) — user-visible latency at the origin (Cloudflare edge hop excluded). Full container stdout (up 5 weeks) parsed; `POST /round-trip` n=115, 98× 200, 13× 422 (no-route probes), 2× 502 (Jun 13 deploy churn), 2× timeout-era 19.7 s max (pre-v3 Python stack).

| window | n(200) | hits (<0.1 s) | misses | miss rate | miss p50 | miss p95 | miss max |
|---|---|---|---|---|---|---|---|
| all 5 weeks | 98 | 26 | 72 | 73 % | 1.30 s | 12.60 s | 19.66 s |
| **post-v3 (Jul 14+)** | 40 | 15 | 25 | **62 %** | **0.77 s** | **2.45 s** | **2.59 s** |

- The two latency modes separate cleanly at 0.1 s (hits 3–42 ms; misses ≥ 150 ms) — hit/miss classification needs no orchestrator join.
- Pre-v3 rows (4–20 s misses) are the old stack; they demonstrate the *metric*, not the current pipeline.
- **Concurrency:** max 2 same-second (once, Jul 12), max 4 per 10 s window (once, Jul 14 — four sub-0.6 s bank serves in 2 s, i.e. cheap hits, not stacked generates). Zero instances of overlapping *misses*.
- Miss rate 62 % is high but is the single-user-exploring-fresh-cells + cache-churn signature: three cache-identity resets in the window (v3 cutover Jul 14, Balkans tileset, fingerprint v2 + xcand strength walks Jul 15–16 — `cache.db` now holds exactly the #78 smoke: one prefix, 2 hits/2 misses, 3 banked rows). Hit rate will climb as banks refill; the *cost* of a miss (≤2.6 s) is the user-visible quantity either way.
- Orchestrator log window is only container-age (10 h at inspection — restarted at #78) and its lines carry no durations; nginx is the durable, timed source. #78-style smokes that hit `:8000` directly bypass nginx — invisible here, fine (not user traffic).

## 3. Metric of record (the C2 input)

**User-visible capacity metric:** from `docker logs curvagen-nginx`, `POST /round-trip` lines —

1. **Miss-mode p95 `request_time`** (requests ≥ 0.1 s; the generate wait a user actually feels),
2. **miss rate** (share ≥ 0.1 s of all 200s),
3. **burst proxy:** max requests per 10 s window, and specifically *overlapping misses* (two ≥0.1 s requests within one window).

One-pass, zero new instrumentation. Suggested **watch triggers** (C2 to ratify): sustained miss p95 > ~5 s, or overlapping-miss windows appearing at all (that's when 2-vCPU stacking starts), or fill-queue drops in orchestrator logs ("Bank Fill queue full").

**Caveats:** docker-stdout log lives with the container (nginx up 5 weeks; a recreate resets history — the in-container `/var/log/nginx/access.log` file is the same stream); CF edge latency not included; `origin_stats` gives lifetime rates but no waits and its prefixes churn with fingerprint/tileset bumps.

## 4. Hand-off to C2 (verdict ticket, post-B1)

- Today: **no user-facing pain — mitigation "none" is the evidence-supported default.** No synthetic burst run was needed; logs sufficed.
- Re-check after B1 only if the chosen mechanism changes fill cost: family (a) selection re-scoring adds ~0 to fill; (c) over-build raises the background K=12 call's cost (still single-flight, still invisible to users unless misses overlap); penalty-0 *lowers* it (~1.07×→~1.0×).
- The capacity question becomes user-visible only with multi-user concurrent misses or an app-side fan-out change — both watch triggers, not present states.

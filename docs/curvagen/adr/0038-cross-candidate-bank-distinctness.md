# Cross-Candidate Bank Distinctness

> **Status: Accepted (2026-07-15) — implemented on `curvature-costing` (uncommitted),
> config-gated OFF by default; enable-in-prod and filter both gated on the latency budget
> and a serving-side decision (below).** Outcome of the loop-route survey (wayfinder #46,
> [2026-07-13-loop-route-algorithms.md](../research/2026-07-13-loop-route-algorithms.md)),
> instrumented and A/B-measured against the ADR-0037 done-run engine (`:t6` @ `9ca37c5f3`)
> on Balkans tiles. Evidence at ship strength 0.5: bank overlap **0.570 → 0.463 (−19 %)**,
> near-duplicate rate (>60 % shared) **47.8 % → 25.6 %**, lollipop 1.8 % → 0.3 %, dist_err
> 0.220 → 0.178, spikes held at 0.00 %, K=12 fill held; reuse@100 km gate clears (0.0486
> ≤ 0.05); latency +125 ms engine mean (1.32× p50 — the accepted cost, see §4).

**Amends [ADR 0037](./0037-roundtrip-v3-defect-gated-loop-construction.md) §5** ("same
node-dedup bank distinctness"; "lollipop rate … a measured KPI, not separately
engineered"). ADR 0037's distinctness rested on turnaround **node-dedup + the T9
min-separation guard**, which spread turnaround *endpoints* — not the *corridors* the loops
ride to reach them. This ADR adds the cross-candidate corridor penalty, the item 4 sharing
filter, and the bank-overlap meter that made the gap visible. Keeps ADR 0037's pipeline,
wiring, and API surface intact; the penalty is additive to the existing intra-loop reuse
leash (ADR 0031) and rides its `EdgeFactor` tier. One-Way untouched.

## Why

The loopqual harness (ADR 0037 / #48) and Gate v1.2 measure **per-loop** defects only —
spikes, intra-loop reuse, lollipop stem. A new **bank-distinctness meter** (per-bank
pairwise corridor overlap) surfaced an axis every per-loop gate was blind to: in the
done-run corpus, **50 % of served loops share >60 % of their fresh-road corridor with a
sibling in the same K=12 bank** (mean overlap 0.57), and **v3 hardening slightly *worsened*
it** (pre-v3 0.555 → done-run 0.576 — optimizing each candidate toward the same curvy ideal
converges their corridors). The rider gets "12 distinct fun loops" that are half retreads.

Root cause: the fork's anti-reuse is **intra-loop only** — the leash marks one candidate's
own forward leg before its return (ADR 0031); nothing tells candidate N to avoid the roads
candidates 1..N−1 already claimed. This is the GraphHopper "penalize each leg against all
previous legs" the fork never had. Turnaround min-sep spreads endpoints, not corridors:
**only 3.7 % of near-dups are true twins** (turnarounds closer than min-sep); 96 % have
well-separated turnarounds yet still share corridor. And the overlap is **pairwise-avoidable,
not network-forced** — the bank-wide common trunk (edges ≥75 % of the bank use) is only
0.079; the alternatives provably coexist (different loop-pairs ride different corridors).

## Decision

### 1. Bank-distinctness meter + Gate (loopqual, metrics v1.3)

The missing measurement, added first — you cannot tune a fix you cannot see.

- **`bank_distinctness`** (`metrics.py`, `METRICS_VERSION` 1.2 → 1.3): per loop, the maximum
  length-weighted undirected-edge overlap against any bank sibling — "what fraction of this
  loop retreads its nearest twin." Exemption-discounted (the shared forced start stem does
  not count — same Start-Exemption zone as `edge_reuse_geom`). Reports `bank_overlap` (mean,
  p50/p90), `near_dup_gt_{050,060,070}_frac`, `best_twin_sep_m` (the min-sep-guard blind-spot
  split), and `common_trunk_frac_{25,33,50,75}` (the forced-spine-vs-pairwise decomposition).
  Wired through the runner aggregates and the `report.md` headline.
- **Gate 9** (`gate_v1_proto.py`): a regression gate — candidate `bank_overlap_mean` ≤ 1.02×
  baseline and `near_dup>0.6` ≤ baseline + 2 pp. New in v1.3, meaningful only once both runs
  carry the metric; the pre-existing latency (→10) and failures (→11) gates renumber.

### 2. Cross-candidate corridor penalty (the fix — ships as default-off)

Each committed loop registers its **fresh-road edges** (both directions, outside the Start
Exemption disk, counted once per loop) in a bank-wide `edge → prior-loop-count` map. Every
later candidate's return leg pays a **soft surcharge** `1 + strength · min(count, cap)` on
those edges, graded by how many prior loops piled onto the edge and **capped** (the ATMOS
per-edge-increase-cap lesson — a genuinely single corridor stays routable). The surcharge is
merged into the existing progress-graded rejoin tier (max wins), so `EdgeFactor` keeps one
lookup. The exemption disk is never surcharged — the network-forced start stem must stay
routable home.

- **Ship value `strength` 0.5**, swept 0.3–0.9 on corpus-v1: distinctness scales monotonically
  with strength; **0.5 is the knee that clears the reuse@100 km gate** (0.0486 ≤ 0.05) while
  0.6 trips it (0.0513) — pushing returns off shared corridors nudges intra-loop reuse up at
  100 km, and 0.5 keeps the margin. `cap` 4.
- Result (§ status): overlap −19 %, near-dup halved, **quality improved** (lollipop 1.8→0.3 %,
  dist_err 0.220→0.178, curviness held), K=12 fill held. Rejected: a naive standalone item-4
  filter without the penalty — it churns or under-fills, the penalty must feed the refill.

### 3. Item 4 sharing filter (conditional — gated on serving-side adaptive-K)

A build-time K×K near-duplicate reject: a built loop whose fresh-road length overlaps an
already-kept loop beyond `sharing_frac` (0.6) is rejected and the slot refilled from the
queue — the byte-identical dedup (ADR 0037) generalized from "identical" to "near-identical".

- Near-eliminates near-dups (>0.6 rate **47.8 % → 6.2 %**, overlap → 0.384) — but **under-fills
  to the network's true distinct-corridor count**: mean 10.67/12 served, 34 % of banks below
  K=12 (down to K=3–5 in sparse rural cells). This is the **Network Ceiling** — many origins
  genuinely cannot offer 12 loops sharing <60 %; padding them with near-dups was fake variety.
- **Ships OFF.** Realizing it needs serving-side **adaptive-K** (a variable bank size the
  orchestrator serves honestly, ADR 0036 surface) — a Backend decision, not an engine one.
  The engine side (honest under-fill, no padding) is done; the serving side is deferred.

### 4. Latency — the cost is intrinsic; strength is the only clean dial

The penalty widens the return A* (`astar` 429 → 489 ms, `astar_fb` 160 → 234 ms) because
surcharged edges cost more to route around — **the widening IS the distinctness mechanism**.
+125 ms engine mean at strength 0.5. Three decoupling levers were built and measured, all
**rejected**:

- **fallback-clear** (drop the penalty on the last-resort Fallback return): best latency
  recovery (astar_fb 234 → 132 ms) but **trips the reuse@100 km gate** (0.052) — the fallback
  then retreads.
- **top-N cap** (surcharge only the N most-shared edges): dominated — 400 ≪ bank edge count,
  so it drops most of the penalty (overlap back to 0.54, near baseline).
- **min-count ≥2** (surcharge only pile-ups): gate-clean but redundant with `strength` (same
  distinctness/latency curve).

None reduces latency without an equal distinctness or gate cost. `strength` is the dial; the
levers were removed. Governed by the rig-ratio gate (≤ 1.10×); xcand at 0.5 lands **1.32× p50
/ 1.18× p95**, so **the feature ships OFF by default** — the default deploy is byte-identical
to the ADR 0037 done-run and passes every gate. Enabling xcand is the opt-in that spends the
latency; per Andrey (2026-07-15) the 1.10× ratchet stands and xcand stays off until the box
latency budget admits it.

### 5. Config surface, tests, productionization

- **Config-gated on `thor_worker_t`, all default off/neutral**: `roundtrip_xcand_penalty`
  (bool), `roundtrip_xcand_strength` (0.5), `roundtrip_xcand_cap` (4),
  `roundtrip_sharing_filter` (bool), `roundtrip_sharing_frac` (0.6). Documented on the member
  decls (worker.h), read in the ctor (worker.cc) — matching the `roundtrip_stage_timing`
  pattern.
- **gurka coverage** (`test/gurka/test_roundtrip_distinctness.cc`, the fork's test idiom):
  the penalty lowers bank overlap (mechanism); the penalty never surcharges the exemption disk
  (cul-de-sac still routes home); the sharing filter keeps served loops under the threshold.
  The existing 24-test round-trip suite still passes flags-off (unchanged behavior).
- Clean implementation on `curvature-costing`; the intra-loop leash, hard-exclude return, and
  rejoin tier are reused, not duplicated. Uncommitted pending Andrey's go.

### 6. Rollout

1. Merge to `curvature-costing` → fork `build-amd64` CI → ghcr amd64 image (flags off = the
   current done-run engine, no behavior change).
2. Enable `roundtrip_xcand_penalty` + `strength 0.5` in prod config **when the latency budget
   admits ≥1.32× p50** on the box (not now — §4).
3. On enable, a **done-run with Gate v1.3**: gate 9 (bank distinctness) must show the
   improvement and gates 1–8 must hold; **`FINGERPRINT_VERSION` bumps** (xcand changes the
   served loops — riders must not get cached pre-xcand loops from identical keys). Rollback =
   flag off (no key change needed to disable) or redeploy the previous image.
4. **Zero API surface change**: same request/response, same alternates, same node-dedup base
   distinctness; xcand is additive engine-internal. No new rider knobs.
5. The sharing filter waits on the serving-side adaptive-K decision (§3) — a separate effort.

## Consequences

- The loopqual harness now measures the **cross-candidate** axis it was blind to; gate 9
  catches bank-distinctness regressions (it would have caught v3 hardening's 0.555 → 0.576
  drift). Baseline and candidates re-read under metrics v1.3.
- Engine config surface grows by five knobs (all off/neutral by default); the fork-maintenance
  surface grows by the bank-edge memory, the sharing filter, and the `loop_keylen` decode —
  bounded by the three gurka tests and ADR 0032/0033's engine-first trade.
- Latency and distinctness are coupled through `strength`; the rig-ratio gate (→10) is red
  whenever xcand is on. Accepted: ship off-by-default, enable on the latency budget.
- The item 4 filter is real and strong but **blocked on serving-side adaptive-K** — the
  Network Ceiling means K is variable, and serving that honestly is an orchestrator effort.
- Backend Serving Context glossary gains **Bank Overlap**, **Cross-Candidate Penalty**,
  **Sharing Filter**, **Network Ceiling**; the Loop-Quality Gate entry tracks metrics v1.3 /
  gate 9.
- Fork changes are uncommitted and the feature is dark by default — nothing ships until the
  latency budget and (for the filter) the adaptive-K decision land, each on Andrey's go.

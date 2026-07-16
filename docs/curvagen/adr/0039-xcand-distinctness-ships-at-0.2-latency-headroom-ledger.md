# Cross-Candidate Distinctness Ships at Strength 0.2 — Latency Headroom Ledger

> **Status: Accepted (2026-07-16) — supersedes ADR-0038's ship-status: cross-candidate
> distinctness moves from OFF-by-default to ON at `strength` 0.2**, config-tunable, on the
> `curvature-costing` fork rebased onto Valhalla 3.8.2. Outcome of the latency-headroom map
> (wayfinder #66): the fork rebased to 3.8.2 (v3 loopqual gate green on the new base) and
> every serving-path algorithm audited and ranked by *measured* Δp50, producing the headroom
> ledger below. Ships the **half win** (overlap 0.570 → ~0.51) at **1.069× p50**
> (prod-regime-validated, under the 1.10× ratchet); the **full win** (strength 0.5, overlap
> 0.463, 1.22×) is not reachable by any serving-path or config lever on the current 2-vCPU box
> and is handed to **lever #3** (harvest/bucketing rewrite, backlog). Deploy off-map on Andrey's
> explicit go (ADR-0037 §7.8). Fork base tip `a2fd86343` (3.8.2), penalty at `d5033082d`
> — committed, **not pushed**.

## Context

ADR-0038 introduced the cross-candidate corridor penalty — each later candidate's return leg
pays a soft surcharge on edges prior bank-siblings already used, cutting bank overlap
**0.570 → 0.463 (−19%)** at ship `strength` 0.5. It shipped **OFF**: full strength cost
**1.32× p50**, over the **1.10× latency ratchet** (a fixed constraint, Andrey 2026-07-15).
The rider gets "12 distinct fun loops" that are otherwise half retreads, but not at that price.

Map #66 existed to find the latency headroom to ship it ON: either make the rest of the
pipeline cheaper so xcand fits, or make xcand itself cheaper — both under the ratchet. The
falsifiable headline was "xcand ON at full distinctness under 1.10× on 3.8.2." The durable
deliverable — regardless of whether the headline proved reachable — is the **ledger**: every
serving-path algorithm audited and ranked by measured Δp50/cost, so the shipping decision rests
on evidence, not guesswork.

## The latency headroom ledger

All Δ measured on the 3.8.2 base (`valhalla-fork-test:rebase38`), loopqual corpus-v1
(232 req, Balkans tiles), stage-timing ledger `thor.roundtrip_stage_timing`. Research assets
linked per row.

### Serving-path cost, ranked — and whether any yields headroom

| Rank | Stage | Share of p50 | Headroom | Finding |
|---|---|---|---|---|
| 1 | Return leg `astar` + `astar_fb` | **56%** (astar 45% / fb 11%; ≤65% @300 km) | **none** | Locked from three sides — no config lever (A1: harvest hardcodes `kFullExploreRadiusM=90000`, never reads `unidirectional_astar`; return bidir A\* penalty-bound, sweeps byte-identical), fallback two-pass structurally irreducible (A2: high-leash collapse exploded astar 6×/max 19 s), penalty-form soft-optimal (X1: hard-exclusion strictly worse, p95 doubles) |
| 2 | Framing | 13% (73 ms) | **none** | No dedicated lever (F1); the named pieces are footnotes (loki 0.9 / odin 0.1 / serialize 0.6 ms), ~30 ms is irreducible three-hop PBF plumbing, and ~40 ms is un-ledgered `roundtrip_impl` thor compute (diffuse, K-scaled) |
| 3 | Harvest / build / rest | ≤11% | **none** | Footnotes |
| — | **Config surface** | — | **none** | Exhausted (R3): no p50 knob on the round-trip path (matrix/trace/narrative/one-way-alternates inert, tile-cache knobs moot in mmap-extract, rest is build-time/ceiling/observability); the contended ratchet is **off-config** (thread count is a `valhalla_service` CLI arg) and **CPU/hardware-bound** on 2 vCPU; RSS levers slack (400 MiB / 3.7 GB). Two R2 corrections: **drop-elevation falsified** (`prefer_elevation:0.3` + serve-time `/height` = load-bearing), **`server_threads` off-config + dead** on 2 vCPU |
| — | **Contended ratchet** | — | n/a | **Regime-invariant** (C1): the loopqual gate is trustworthy — prod contention (K≈12 through 2 threads on 2 vCPU) does not amplify the xcand ratio; it slightly *dilutes* it (queue-wait dominates). The gate's ratio ≈ prod's |

Assets: [P1](../research/2026-07-15-p1-latency-profile.md) · [A1](../research/2026-07-15-a1-return-leg-astar-audit.md) · [A2](../research/2026-07-15-a2-fallback-return-astar-audit.md) · [X1](../research/2026-07-15-x1-xcand-penalty-reformulation.md) · [F1](../research/2026-07-15-f1-framing-bucket-split.md) · [R3](../research/2026-07-16-r3-valhalla-config-surface-audit.md) · [C1](../research/2026-07-16-c1-contended-ratchet-prod-regime.md).

### xcand cost vs. win — the only two dials left

The penalty widens the return A\* (the widening *is* the distinctness mechanism), so its cost is
intrinsic — every audit confirmed no serving-path or config lever offsets it. **`strength` is
the only clean dial.** Prod-regime-validated (C1); quality gates from ADR-0038 (§1 gate 9
regression, §2 reuse@100 km ≤ 0.05 — which caps strength from *above*, tripping at 0.6):

| `strength` | latency (p50) | bank overlap (win) | quality gates |
|---|---|---|---|
| OFF | 1.00× | 0.570 (none) | pass |
| **0.2 (ships)** | **1.069×** — under ratchet | **~0.51 — half win (−10%)** | pass (further from the reuse gate than 0.5) |
| 0.5 | 1.221× — over ratchet | 0.463 — full win (−19%) | pass (0.5 = reuse-gate knee) |

## Decision

**The full win is not reachable under 1.10× on current hardware** — every serving-path
algorithm (return leg, framing) and the entire config surface were audited and yield zero
offsetting headroom, and the contended ratchet is CPU/hardware-bound and off-config. So:

1. **Ship cross-candidate distinctness at `strength` 0.2, default-ON, config-tunable.** Set
   `thor.roundtrip_xcand_penalty:true` / `roundtrip_xcand_strength:0.2` in prod `valhalla.json`.
   This is a real, gate-clean, prod-validated distinctness improvement (overlap 0.570 → ~0.51,
   1.069× p50 / 1.058× p95) — the half win, banked now. Rollback is a **config edit**
   (`strength` → 0), no redeploy. Hold the 0.2 margin (~3 pp under the ratchet) rather than
   chasing the precise 1.10× knee — prod latency is noisy under burst (see §Capacity) and
   0.2 → ~0.25 buys little distinctness (0.3 is already 1.19×, over).
2. **The full win goes to lever #3** (harvest/bucketing distinctness — fold distinctness into
   candidate *selection* rather than penalized return-re-routing, so banks are distinct with no
   return-leg penalty → overlap 0.463 at ~no latency). A design-heavy rewrite; its own future
   effort, not a one-shot ticket. The only route to the full win under budget now that config
   and every serving-path algorithm are exhausted.
3. **Deploy is off-map**, on Andrey's explicit go (ADR-0037 §7.8) — prototype-and-measure was
   in-map; the ship task is handed to the execution backlog.

## Consequences

- **Capacity note (not the ratchet):** under a 12-way concurrent bank-fill burst on 2 vCPU,
  *absolute* round-trip generation latency is severe (C1: OFF p50 ~4 s / p95 ~8.7 s;
  strength-0.2 p50 ~4.2 s / p95 ~9.2 s). This is a throughput matter — the bank-fill worker
  under burst on 2 cores — largely hidden from users by the bank cache + background fill, and
  tied to the out-of-scope hardware line and bank-K sizing, not to the xcand decision. Tracked
  as a backlog watch-item.
- **Rebase to 3.8.2 (K1):** the fork now bases on 3.8.2 (`a2fd86343`); existing 3.7.0-built
  Balkans tiles load and serve unchanged; loopqual v3 gate green (p50 1.00× / p95 0.95×). The
  one config-generator field collision (`prefer_curvature` 97 → 101) is resolved.
- **ADR-0038 supersession:** its ship-status ("OFF by default, gated on the latency budget")
  is resolved by this ADR — the budget was audited exhaustively; it ships ON at 0.2, and its
  "implemented (uncommitted)" note is stale (penalty committed at `d5033082d`).

## Execution / hardening backlog (handed off, like ADR-0037's #52 for v3)

1. **Ship xcand `strength` 0.2 to prod** — push the fork (`curvature-costing` @ `a2fd86343`,
   not pushed), rebuild the serving image, flip the config, redeploy + prod-smoke. **Gated on
   Andrey's explicit go.**
2. **Lever #3 — harvest/bucketing distinctness** — the full-win route; its own future
   wayfinder effort (design-heavy).
3. **Capacity watch** — round-trip generation under concurrent burst on 2 vCPU; mitigations =
   bank cache + background fill (in place), lower bank K, or the hardware line.

ADR-0038's **item-4 K×K near-dup filter** stays parked under ADR-0038 (gated on serving-side
adaptive-K), out of this map's scope.

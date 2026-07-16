# X1 — xcand-direct: cut the penalized return-search width, hold overlap

_Wayfinder map #66 (latency optimization), ticket #73. 2026-07-15._

**Verdict: the cross-candidate penalty cannot be reformulated cheaper — the soft
per-edge surcharge is already the efficient form.** Converting the penalty from a
soft surcharge to a *hard exclusion* (the only buildable "prune earlier" lever left
after A1 killed search-bounding) makes latency **worse, not better** — p50 1.22× →
1.46× and the p95 tail **doubles** (3.3 s → 6.2 s), because hard-excluding the
*scattered* bank edges fragments the return graph and triggers the exact A2
fallback thrash. With A1 (bounding is inert) and A2 (the fallback is irreducible),
the return-leg cost of xcand is now **structurally locked from three sides**.
xcand at the ship distinctness (overlap 0.463) does **not** fit under the 1.10×
ratchet on current hardware, and no penalty reformulation changes that. The only
surviving dials are **strength** (a smooth partial-win frontier, measured below)
and lever #3, **folding distinctness into harvest/bucketing** (a different stage —
backlog-scale, untested, handed to S1).

## What xcand's cost actually is (from P1/#70)

The cross-candidate penalty (`route_action.cc:1678`) registers every committed
loop's fresh-road edges in a bank-wide `edge → prior-loop-count` map
(`route_action.cc:2106`) and adds a soft surcharge `1 + strength·min(count, cap)`
(strength 0.5 / cap 4) to every later candidate's return leg. P1 decomposed its
+98 ms p50 / +243 ms p95 as **almost entirely wider penalized return-leg A\*
expansion** (`astar` +286 ms / `astar_fb` +271 ms at p95), for the overlap win
0.570 → 0.463. The three-way ticket question: cap the search (**A1: inert**),
reformulate the per-edge penalty to prune earlier (**this ticket**), or fold
distinctness into harvest/bucketing (**lever #3, backlog**).

## Method

Same rig as P1/A1/A2 (`run_one.sh` → engine on :8003, Balkans tiles, `loopqual`
corpus-v1, **workers=3** — the contended ratchet condition, not P1's clean
workers=1). All runs on **one build** (`valhalla-fork-test:x1` = rebase38
`a2fd86343` + a 3-file change adding the `roundtrip_xcand_hard_count` knob), so
soft vs hard vs OFF are same-binary comparisons. Overlap is deterministic
(byte-identical reruns, A1); p50 carries a ~±6 % workers=3 noise floor, so
latency claims lean on effects above it. Baseline = `rebase38-tip` (p50 **0.835 s**,
overlap 0.5701) = the K1 anchor; **1.10× ratchet = 0.918 s p50**. Scratch:
`~/.curvagen-scratch/cfg-x1-*`, `results/x1r-*`.

**The reformulation prototyped** (`src/thor/route_action.cc`, the apply loop): split
the bank penalty by pile-up count. Edges with `count ≥ hard_count` (the common
trunk many loops piled onto) become a **hard exclusion** on the primary return A\*
(appended to the same `AddUserAvoidEdges` set as the forward corridor — a sharp
hole); edges below the threshold keep the **soft** surcharge (the pairwise
distinctness). `hard_count = 0` is the pure ADR-0038 soft form. Hypothesis (from
A1): a *sharp* hole searches narrower than the diffuse soft "cost fog," so
sharpening the trunk cuts the width tail while the soft layer holds overlap.

## 1. The reformulation is falsified — hard is strictly worse

| config | p50 | ratio | p95 | overlap | near_dup>0.6 | fill |
|---|---|---|---|---|---|---|
| OFF (rebase38-tip) | 0.835 | 1.00× | 2.729 | 0.5701 | 0.478 | 2780 |
| **soft s0.5 (hard=0)** | **1.016** | 1.22× | **3.319** | **0.4632** | 0.256 | 2780 |
| hard=3 (sharp-trunk) | 1.215 | 1.46× | **6.163** | 0.4433 | 0.220 | 2780 |
| hard=2 | 1.310 | 1.57× | **6.824** | 0.4572 | 0.259 | 2780 |
| hard=1 (all bank hard) | 1.430 | 1.71× | **9.075** | 0.4900 | 0.357 | 2780 |
| hard=1, strength 0 | 1.531 | 1.83× | 7.601 | 0.4900 | 0.357 | 2780 |

The p95 tail is **monotonic in the size of the hard set** (3.3 → 6.2 → 6.8 → 9.1 s
as the threshold drops from ∞→3→2→1). That is the A2 signature exactly: hard
exclusion *removes* edges from the graph, so a candidate boxed off its fresh-road
corridor finds **no route home under exclusion** and spills into the unbounded
soft-leash fallback — and the more edges excluded, the more (and deeper) the
spills. hard=3 buys a sliver more distinctness (0.4433 vs 0.4632) at +20 % p50 and
a doubled tail — a strictly worse trade than simply raising strength.

**Why A1's "hard is fast" does not carry over.** A1 found the *forward-corridor*
hard exclusion gives a sharp, fast optimum. But the forward corridor is **one
contiguous path** — excising it leaves a clean hole with a clean detour. The bank
edges are **scattered** across many prior loops' corridors; hard-excluding a
scattered set punches holes everywhere, leaving no clean detour → thrash + fallback.
Hard exclusion is fast for a contiguous exclusion, catastrophic for a diffuse one.
The soft surcharge is efficient **because** it keeps every edge routable (never
forces a fallback) while still steering the search off shared corridors.

### The knob is real (controls, not a dead flag)

This finding survived a false negative. The first build silently shipped the stale
installed binary (`cmake --build` writes `build/valhalla_service`; the container
runs `/usr/local/bin/valhalla_service` from the image's `make install`, and a
`dock​er cp` mtime older than the cached `.o` also let cmake skip the recompile).
On that binary the `hard_count` knob was inert and **every threshold read as pure
soft** — a byte-identical sweep that *looked* like "reformulation is inert" for the
wrong reason. Two controls on the corrected binary prove the knob drives routing:
each threshold yields a **distinct** loop-geometry hash, and **hard=1 is
byte-identical at strength 0.5 and strength 0** (`geomsig 8097c30e89df`) — exactly
right, since at hard=1 all bank edges take the hard branch and strength is bypassed.
Lesson for the fork: after an incremental build, `cp build/valhalla_service
/usr/local/bin/` and `strings … | grep <new-knob>` before trusting a run.

## 2. The strength frontier — the only surviving dial (for S1)

Soft xcand, strength swept on the same build (overlap deterministic; p50 ±6 %):

| strength | p50 | ratio | overlap | near_dup>0.6 | under 1.10×? |
|---|---|---|---|---|---|
| OFF | 0.835 | 1.00× | 0.5701 | 0.478 | — |
| 0.1 | 0.898 | 1.08× | 0.5374 | 0.413 | yes (marginal) |
| 0.2 | 0.909 | 1.09× | 0.5114 | 0.365 | yes (marginal) |
| 0.3 | 0.992 | 1.19× | 0.4943 | 0.324 | no |
| 0.5 (ship) | 1.016 | 1.22× | 0.4632 | 0.256 | no |

A smooth, monotone tradeoff: every step of distinctness costs latency, and the
curve does not bend. **xcand fits under 1.10× only at strength ≤ 0.2, where the win
is weak** — overlap 0.51 (a −10 % reduction) vs the ship's 0.463 (−19 %), near-dup
0.365 vs 0.256. The full ADR-0038 distinctness win (0.463) lands at **1.22× p50**,
over the ratchet. (s0.1/s0.2 sit ~1 % under the 0.918 gate, inside the noise floor —
treat "fits" there as "on the line," not comfortably under.) This matches
ADR-0038 §4 ("strength is the dial; latency and distinctness are coupled") and now
quantifies it against the ratchet on the 3.8.2 build.

## 3. Ledger line (for #75 synthesis)

- **xcand penalty reformulation (X1's lever): no headroom.** The soft surcharge is
  the efficient form; hard-exclusion (any threshold) is worse — +20–70 % p50 and a
  2–3× p95 tail (A2 fallback thrash on a scattered exclusion set). **Falsified.**
- **Return-leg cost is structurally locked from three sides:** A1 (search-bounding
  inert), A2 (fallback two-pass irreducible), X1 (penalty form irreducible — soft is
  optimal). The width **is** the distinctness mechanism; it is invariant to how the
  "avoid these edges" signal is expressed.
- **The strength dial is the only lever, and it is a partial-win frontier:**
  under 1.10× ⇒ strength ≤ 0.2 ⇒ overlap ~0.51 (half the ship win). Full win
  (0.463) ⇒ 1.22×, over the ratchet on current hardware.
- **Sole untested path = lever #3: fold distinctness into harvest/bucketing.**
  Diversify candidate turnaround/corridor *selection* up front so the return legs
  are naturally distinct without a per-edge return surcharge — moving the cost out
  of the hot return A\* entirely. A1 flagged it algorithmic / backlog-scale; not a
  one-session prototype. **The only way left to ship the full distinctness win under
  the ratchet without new hardware — S1 decides whether to open that backlog.**

## Consequences for the headline / S1

The map's falsifiable headline (xcand ON, full distinctness, under 1.10× on 3.8.2)
is **not reachable by any lever inside the serving path's tuning + penalty-shape
surface.** S1's shipping-set choice narrows to: **(a)** ship xcand OFF (default,
no cost, no win); **(b)** ship at strength ~0.2 — under the ratchet, ~half the
distinctness win; **(c)** ship full strength 0.5 at 1.22× — needs the box-budget
(out-of-scope hardware line) or a ratchet relaxation (out of scope, fixed by
Andrey); or **(d)** open the harvest/bucketing rewrite (lever #3) as a backlog
effort — the only untested route to the full win under budget.

## Caveats

- workers=3 single-run p50, ±6 % noise; the reformulation-worse and full-strength-
  over-ratchet findings are well above it, the "s≤0.2 fits" claim is on the line.
- Overlap/geometry deterministic (byte-identical reruns); the distinctness figures
  are exact.
- Balkans-tile / warm-cache absolutes; ratios travel, absolutes don't.
- The prototype knob (`roundtrip_xcand_hard_count`) is a **rejected** throwaway —
  reverted from the fork working tree (the finding, not the code, is the artifact),
  same as ADR-0038 §4's removed levers. Reproducible from `valhalla-fork-test:x1`.

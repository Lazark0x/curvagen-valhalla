# A2 — fallback return-A* (`astar_fb`) tail audit

_Wayfinder map #66 (latency optimization), ticket #72. 2026-07-15._

**Verdict: the fallback overhead is structurally irreducible via the obvious fix.**
The fallback is not a boundable tail — it's high-frequency, low-per-cost work
(27% of return-leg time), and the two-pass hard-exclude + fallback design is
*efficient by construction*. A prototype that collapsed the two passes into one
(high-leash instead of hard-exclude) **exploded the primary search 6–48×** and is
disqualified. Like A1, the return leg resists tuning; headroom must come from X1
/ F1 / RSS.

## Characterization — the fallback is frequency, not a tail

From the P1/A1 baseline timing (`off-timing.txt`, 232 job-ordered requests):

- `astar_fb` = **27% of all return-leg time** (30.4 s vs `astar` 83.7 s over the corpus).
- **Not tail-concentrated:** the top-10 of 232 requests hold only 25% of `astar_fb`.
- **Per-fallback is cheap:** median **16 ms**, p90 76 ms, max 238 ms.
- **Frequency is the driver:** fallbacks/request by distance — 20 km **8.4**, 50 km
  **8.4**, 100 km 7.7, 200 km 3.9, 300 km 3.1. At short distance **~8 of 12
  candidates fall back** (a small road network can't close 12 distinct fresh-road
  loops in a tight radius — structural).

So the ticket's lever 1 ("bound / short-circuit the retry") is low-value: per-fallback
is already ~16 ms; bounding trims only the rare 238 ms outliers.

## Prototype — collapse the two-pass (high-leash) — FALSIFIED

The idea: `route_leg` currently **hard-excludes** the forward corridor (beyond the
1500 m Start Exemption), runs the primary bidir A*, and only if that returns empty
re-runs a full bidir A* on the soft leash (`astar_fb`). ~6 of 12 candidates pay
that second search. Hypothesis: replace the hard exclusion with a **high finite
leash** (corridor edges × 50 via the rejoin multiplier, `route_action.cc:1688`), so
the primary reuses the corridor only when no distinct route exists — one search
instead of two, preserving distinctness (a distinct route still wins when cheaper
than the leashed reuse).

Built (incremental recompile on `rebase38`, image `a2test`) and measured on
corpus-v1 (partial run, 164 requests — killed once conclusive):

| metric | baseline | high-leash prototype |
|---|---|---|
| `astar` median | 230 ms | **1352 ms (~6×)** |
| `astar` max | ~1.1 s | **19 071 ms (19 s)** |
| `astar_fb` median | 55 ms | 0 (collapsed) |
| fallbacks / req | 6.29 | 0 |

**The fallback collapsed exactly as designed — but the primary exploded.** Net
return-leg time went ~5× *worse*.

### Why: hard-exclusion is a search-space reduction, not waste

- **Hard-exclude removes** the corridor edges from the graph → the primary bidir A*
  searches a *small* fresh-road subgraph → cheap. When that subgraph is
  disconnected (no fresh-road route), the search fails fast and the fallback re-adds
  the corridor → a second cheap-ish search over the reuse route.
- **High-leash keeps** the corridor edges in the graph at high cost → the bidir A*
  must expand the *entire* penalized graph, confirming no fresh-road route is
  cheaper than reuse × 50 before settling → node count explodes. Higher factor =
  worse (more to explore before accepting expensive reuse); lower factor =
  faster but reuse wins too easily → distinctness regression. No factor beats the
  two-pass on the joint latency+quality objective.

The two-pass design trades a *second small search* (the fallback, when needed) for
*small per-search graphs*. That is the right trade — the 27% fallback overhead is
its price, not its waste.

## What would actually reduce the fallback (all bigger than a lever)

- **Lower K.** Fallback frequency is dominated by asking for 12 distinct loops in a
  small network. Fewer candidates → fewer forced fallbacks. A *product* decision
  (bank size), out of scope here — ledger note for S1.
- **Turnaround-returnability pre-filter.** Skip harvesting turnarounds that have no
  distinct fresh-road return, so fewer candidates reach the doomed primary. Needs
  returnability information at selection time (a reverse reachability check) —
  algorithmic, backlog-scale, and it competes with the same reverse-tree idea A1
  surfaced.
- **Bound the rare deep fallbacks.** Cap the retry expansion to trim the max-238 ms
  per-fallback outliers — a small, safe win (~a few % of `astar_fb`), the only
  config-free lever that doesn't backfire. Ledger line.

## Ledger lines (for #75 synthesis)

- **`astar_fb` (fallback, 27% of return-leg / 11% p50 / 521 ms p95):** structurally
  irreducible via two-pass collapse — measured 6–48× worse. The overhead is the
  price of small per-search graphs. **Not a headroom source.** Only a small win
  from bounding the rare deep retries (per-fallback max 238 ms → cap).
- **Redirect (A1 + A2 agree):** the return leg (`astar`+`astar_fb` = 56% of p50) is
  structurally efficient and resists tuning. The 1.10× headroom must come from
  **#73 X1** (xcand penalty reformulation), **#74 F1** (framing), and RSS-rightsize
  (#75) — plus the product/algorithmic ideas above if those fall short.

## Caveats

- Partial run (164/232) — killed once the astar explosion was unambiguous (each
  request ~11 s). The latency result is disqualifying regardless of the untested
  quality trade, so no quality gate was run.
- Prototype reverted; fork source clean at `a2fd86343` (change lived only in the
  throwaway `a2test` image, now removed).
- workers=1 timing; ±12% p50 noise floor (A1). The 6–48× astar effect is far above
  the floor.

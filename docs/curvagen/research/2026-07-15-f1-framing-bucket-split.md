# F1 — splitting the `framing` bucket (loki / odin / serialize / HTTP)

_Wayfinder map #66 (latency optimization), ticket #74. 2026-07-15._

P1 left `framing` — the residual `latency_s − Σ9(roundtrip_impl stages)` — as a
single 69 ms p50 (13 %) lump labelled "loki + odin + serialize + HTTP loopback",
un-attributed internally. F1 splits it. **Headline: the four hypothesised
components are all footnotes (loki 0.9 ms, odin directions 0.1 ms, JSON serialize
0.6 ms, true transport ~30 ms of diffuse multi-process PBF plumbing). The split
exposed that ~55 % of "framing" (40 ms p50, up to 100 ms @ 300 km) is NOT framing
at all — it is un-ledgered `roundtrip_impl` thor compute that the Σ9 stage-ledger
misses.** So no framing component is a standalone audit target, but P1's ledger
has a blind spot worth ~40 ms p50 — the third-largest serving-path cost after the
two A\* stages.

## Method

Valhalla runs loki→thor→odin as three prime_server workers linked by ZMQ; the
serving process is a pipeline, not one function. Each worker logs `Got {Loki,
Thor,Odin} Request N` at `work()` entry, and every log line carries a
nanosecond-precision timestamp (`append_timestamp`, `midgard/logging.cc:22`). At
**engine concurrency = 1** the pipeline is strictly serial (one request fully
drained before the next), so these landmarks decompose each request directly —
**no client-side attribution needed**.

| | |
|---|---|
| Engine | `valhalla-fork-test:f1` — `rebase38` (K1 base `a2fd86343`) + two throwaway `std::chrono` timers in `odin_worker_t::narrate` splitting `DirectionsBuilder::Build` (directions) from `tyr::serializeDirections` (serialize), logged as `framing timing: odin_directions_ms=… serialize_ms=…`. Instrumentation reverted after; `:f1` image kept for repro. |
| Config / rig | `valhalla-A.json` (xcand OFF, `roundtrip_stage_timing:true`), `corpus-v1.yaml` (232 req, K=12), `loopqual … --workers 1 --no-way` |
| Landmarks (per job) | `Got Loki` → `Got Thor` → `roundtrip timing:` (Σ9 + ts) → `Got Odin` → `framing timing:` (dir/ser + ts) |
| Zip | 232 `roundtrip timing:` lines in fire order ↔ `enumerate_jobs`; `latency_s` from `responses/*.json` `meta` |
| Scratch | `~/.curvagen-scratch/f1-framing/` (`parse_f1.py`, `f1-engine.log`); loopqual `results/f1-full/` |

Decomposition (all derivable from the landmark timestamps + Σ9 + `latency_s`):

- **loki** = `t(Got Thor) − t(Got Loki)` — HTTP parse + correlation + zmq→thor
- **thor_unledgered** = `t(roundtrip timing) − t(Got Thor) − Σ9` — thor PBF decode +
  `route()` setup + **`roundtrip_impl` regions outside the 9 timed stages**
- **thor_post_rt** = `t(Got Odin) − t(roundtrip timing)` — `serialize_to_pbf` of the
  24-leg Api + zmq→odin
- **odin directions / serialize** = the injected `framing timing` log
- **odin_plumb** = `t(framing timing) − t(Got Odin) − dir − ser` — odin `ParseFromArray`
- **client_http** = `latency_s − (t(framing timing) − t(Got Loki))` — request/response socket transit

The pieces sum back exactly: `loki + dir + ser + (thor_unledgered + thor_post_rt +
odin_plumb + client_http) ≡ latency_s − Σ9 = framing`. **Identity check: median
`Σparts − framing` = +0.00 ms in every slice** (overall and all five distance
bands) — the split is exact, not a fit.

## 1. The split — overall (n=232, framing p50 = 73 ms of a 570 ms request)

| component | p50 ms | p95 ms | mean | % framing | nature |
|---|---|---|---|---|---|
| **thor_unledgered** | **40.0** | **193.3** | 58.1 | **55 %** | un-timed `roundtrip_impl` thor compute |
| thor_post_rt (`serialize_to_pbf`+zmq) | 13.9 | 39.6 | 17.0 | 19 % | marshalling |
| odin_plumb (`ParseFromArray`) | 7.7 | 24.1 | 9.8 | 11 % | marshalling |
| client_http (socket transit) | 8.3 | 20.6 | 10.0 | 11 % | transport |
| loki correlation | 0.9 | 4.5 | 1.9 | 1 % | compute — footnote |
| serialize (`serializeDirections`, 12 routes) | 0.6 | 2.2 | 0.8 | 1 % | compute — footnote |
| odin directions (`DirectionsBuilder`, 24 legs) | 0.1 | 0.5 | 0.2 | 0 % | compute — footnote |
| **framing (Σ)** | **73.0** | 260.0 | 96.4 | 100 % | |

Grouped honestly:

- **The three named compute components are footnotes:** loki 0.9 ms, odin
  directions 0.1 ms, JSON serialize 0.6 ms — **~1.6 ms p50 combined (2 %).** Odin
  narrating 24 legs and serialising 12 full-geometry routes costs essentially
  nothing; the multi-process design front-loads all the geometry work into thor's
  `TripLegBuilder` (the `build` stage, already in Σ9).
- **True transport / plumbing** (thor encode + odin decode + client socket) =
  **~30 ms p50 (41 %)**, spread across three PBF hops. Irreducible cost of the
  loki→thor→odin process pipeline; scales with the serialized 24-leg payload.
- **The surprise — un-ledgered thor compute = 40 ms p50 (55 %).** This is not
  framing. It is the connective tissue of `roundtrip_impl` that lies *between* the
  nine timed stages (`route_action.cc`): entry setup before `t_harvest` (1490),
  the bank/candidate assembly between harvest and the K-loop (1492–1613),
  per-candidate glue between the timed stages ×12, and the ~180-line pre-build
  candidate finalisation (1977–2157). None of it is inside a `ms_since(t_*)` span,
  so Σ9 misses it and P1's residual absorbed it.

## 2. Scaling by distance — the un-ledgered blob is the whole growth

p50 ms; `transport` = thor_post_rt + odin_plumb + client_http:

| dist | n | framing | thor_unledgered | transport | loki | dir | ser |
|---|---|---|---|---|---|---|---|
| 20 km | 40 | 19 | 7.7 | ~9.6 | 0.9 | 0.0 | 0.2 |
| 50 km | 56 | 37 | 16.8 | ~18 | 0.9 | 0.0 | 0.3 |
| 100 km | 40 | 97 | 67.2 | ~30 | 1.0 | 0.1 | 0.7 |
| 200 km | 56 | 137 | 81.0 | ~52 | 0.9 | 0.3 | 1.1 |
| 300 km | 40 | 177 | **99.7** | ~73 | 0.9 | 0.4 | 1.7 |

- **thor_unledgered scales ~13×** (8 → 100 ms) across the distance range while
  **loki stays flat at ~0.9 ms.** Fixed setup (thor decode, `parse_costing`,
  `adjust_locations`) cannot grow 13× with route length — so the bulk is
  **geometry-proportional `roundtrip_impl` internals** (candidate/edge-list
  handling that grows with leg length, ×K=12), not thor request setup. The ~8 ms
  @ 20 km is the near-fixed setup floor.
- Transport grows too (payload is bigger for longer routes) but stays the minority.
- loki, odin directions, serialize never clear ~2 ms even at 300 km.

## 3. Verdict — is any component a worthwhile audit target?

**No single *framing* component is.** The ticket's four hypotheses resolve as:

- **loki correlation** — 0.9 ms, flat. Footnote.
- **odin directions (24 legs)** — 0.1 ms. Footnote; the fear that narrating all K
  candidates server-side is expensive is false — directions are cheap, geometry is
  already built upstream.
- **JSON serialize (12 full-geometry routes)** — 0.6 ms. Footnote.
- **HTTP loopback** — ~30 ms p50 of diffuse three-hop PBF plumbing. Irreducible
  without changing Valhalla's worker topology (collapse loki/thor/odin into one
  process, or have thor emit JSON directly) — a large, risky architecture change,
  disproportionate to a 13 % bucket. Only shrinks with a smaller payload (lower K
  / less returned geometry).

**But F1 surfaced a real, previously-invisible cost: ~40 ms p50 of un-ledgered
thor compute** — bigger than `harvest` (24 ms) or `build` (20 ms), the
third-largest serving-path cost after `astar` (230) and `astar_fb` (55). It is
*not* a cheap dedicated lever, because it is (a) **diffuse** — spread across setup
+ K loop-glue + pre-build finalisation, no single hotspot the code structure
predicts; and (b) **K- and geometry-scaled** — so the already-backlogged **lower-K**
path cuts it, the same lever that cuts `astar`, `harvest`, and the marshalling.
Attributing it to a specific reducible sub-region needs a finer sub-ledger
(instrument the un-timed regions) — a follow-on prototype, not F1's scope.

**For S1:**

1. **Framing offers no dedicated headroom lever.** The transport floor (~30 ms
   p50) is irreducible pipeline plumbing; the compute footnotes are ~1.6 ms. Drop
   framing from the shipping-set optimisation menu — it reinforces P1's "the
   return leg is the only real lever" conclusion.
2. **Correct the ledger.** "framing" is a misnomer — >half of it is thor compute.
   The honest labels: transport ~30 ms p50, un-ledgered thor compute ~40 ms p50,
   named-compute footnotes ~2 ms.
3. **Optional follow-on (low expected value):** a sub-ledger prototype to attribute
   the 40 ms un-ledgered thor compute, *if* S1 needs to squeeze the strength-0.2
   margin. Prior: diffuse + K-scaled ⇒ no cheap standalone win; the lower-K lever
   already reaches it.
4. **Framing is xcand-neutral** (P1: +5 ms p50 with xcand ON) — it does not bear on
   the xcand-under-1.10×-ratchet question either way.

## Caveats

- workers=1 / no-way ⇒ clean serial-pipeline attribution but understated contended
  absolutes; ratios travel, the ratchet gate stays a contended `--workers 3` measure.
- `Σ9` uses `static_cast<int>` per stage (truncation), a systematic ≤9 ms
  undershoot — a floor on `thor_unledgered`, not the source of its 40 ms.
- Balkans-tile / warm-cache specific; the 55/41/2 % compute/transport/named split
  is condition-independent, the absolute ms are not.
- `thor_unledgered` bundles thor request setup with `roundtrip_impl` un-timed
  internals; the distance-scaling argument (§2) attributes the bulk to the latter,
  but a sub-ledger would be needed to prove the exact split (recommendation 3).

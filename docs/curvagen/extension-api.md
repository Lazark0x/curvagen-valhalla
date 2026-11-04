# Curvagen Extension API

The surface this fork adds over upstream Valhalla, consumed by **curvagen-orchestrator** (`crates/engine-client`). Provider-owned per curvagen-ios ADR history (D5, wayfinder map Lazark0x/curvagen#104): changes to this surface ride the same commit as the implementing code, logged in the Changelog below.

## Surface

### `/roundtrip` (thor action)

Native round-trip loop construction (ADR-0033, rebuilt as v3 per ADR-0037, then as **v4 per ADR-0042** — see `docs/curvagen/adr/`).

- Request: origin `locations[0]`, `distance` (**meters**), `prefer_curvature` (0..1), `seed`, costing (motorcycle). **Unchanged by v4** — no new request field, no new rider knob. Since 2026-11-04 a Shuffle may add `exclude_turnarounds` (below).
- Response: standard Valhalla directions shape; one leg per candidate loop. **v4 adds one additive per-candidate field** (below).
- Behavior contracts: defect-gated construction (spike/backtrack gates), distance tolerance per ADR-0037, K-candidate bank fill served through the orchestrator's cache. **v4** adds road identity (twin/parallel exclusion), harvest hygiene, disjoint-pair selection and distinctness at selection; the loop *shapes* change, the shape of the wire does not.

### `roundtrip.exclude_turnarounds` (additive, curvagen-valhalla#28)

Optional request fields inside `roundtrip`, for a Shuffle that must not serve the loops it already served:

```json
"roundtrip": {"target_distance": 60000, "num_candidates": 3, "seed": 1234,
              "exclude_turnarounds": [{"lat": 44.81, "lon": 20.58}, {"lat": 44.70, "lon": 20.46}],
              "exclude_radius_m": 3000}
```

- `exclude_turnarounds`: turnarounds already served, as `{lat, lon}` degrees. A served loop's turnaround is the last point of its leg 0 shape. Entries without a numeric `lat`/`lon` are skipped; at most 64 are read (every harvest candidate is tested against every entry). **Oldest first**: the recency fallback below reads the tail of the list as the most recently served.
- `exclude_radius_m`: **meters**, optional. Default **0.05 × `target_distance`** (= 0.1 × target / 2): the min-separation guard's distance (ADR-0036 T9), the radius inside which two turnarounds already count as the same loop within one answer. An excluded turnaround is treated like a loop already in the bank. Negative values are ignored (default applies). The default is what "the same loop" means inside one answer; across Shuffles near-twins reach farther, and the orchestrator sent **0.08 × `target_distance`** (rig corpus, 6 Shuffles: 16.5 % of later loops near-twins of an earlier one at 0.05, 7.0 % at 0.08, 4.6 % at 0.10 at a p90 distance error of 26 %); with the ride-through test below catching those near-twins it sends **0.06** (curvagen-orchestrator `fix/shuffle-repeats-guard`).
- Behavior: a candidate turnaround within the radius of any excluded point is dropped from the harvest band before node dedup, bearing bucketing and pair selection, and from the Distance Flex re-scan.
- **Deep pick** (only when the list is non-empty): each pair-mode bearing sector evaluates its top 64 candidates (32 before 2026-11-05) instead of `roundtrip_pair_shortlist` (8) and serves its best-scoring survivor instead of the seed-rotated one. The excluded run also gets twice the pair-evaluation budget (`roundtrip_pair_eval_cap`, `roundtrip_pair_eval_total`) and `kAttemptSlack` (8) more builds, since the exclusions take the cell's best pairs first. The variety comes from the exclusions, so the pick maximises quality (research doc `2026-11-04-shuffle-variety-levers.md` §3.2, branch `research/shuffle-variety`).
- **Recency fallback** (`thor.roundtrip_exclude_recency_fallback`, default on; lists of 2 or more). When the excluded run builds fewer than `num_candidates` loops, the engine first re-runs it excluding only the most recent half of the list and tops the answer up from that run (same min-separation rule as below, ranked after the fresh loops). A spent cell then repeats the loops served longest ago, not the plain run's best, which is often what the rider saw last. Off-target guard and ride-through test apply to that run too.
- **Fill guarantee.** When the answer is still short, the engine runs the cell once more without the exclusions and tops the answer up from that run, in its order, skipping any loop whose turnaround sits within the min-separation guard of a fresh one. Fresh loops rank first, the top-up (the repeats) last. If that would serve fewer loops than the plain run alone, the plain run is served as is. A request with exclusions therefore never returns fewer loops than the same request without them; the 442 stays exactly where it is today. The top-up costs a second run, and only in cells the exclusions exhausted.
- **Off-target guard** (`thor.roundtrip_exclude_disterr_guard`, default on). With exclusions, a fresh loop more than `thor.roundtrip_disterr_tier` (0.3) off the asked distance is never served: wherever it is built (the walk, the twin last resort, the gated stash) it is refused and the search goes on to the next candidate. If that leaves the excluded run short, the recency fallback and the fill guarantee above top it up.
- **Ride-through test** (`thor.roundtrip_exclude_pass_m`, default 50 m; under the guard). A fresh loop whose shape passes within that distance of an excluded turnaround is refused the same way: it rides through the far point of the loop served there. That is the near-twin the radius cannot see; its own turnaround sits just past the radius on the same road (on the rig, 83 of 96 repeats under the 0.08 radius were near-twins of an excluded loop, their turnarounds 2.4-3 km from it, against a 2.4 km radius). Without it, deep Shuffles served loops 2-3x the asked distance: exclusions empty a cell's on-target pairs first, and what the walk then builds closes on full-repair returns (Belgrade 30 km, Shuffle 6: 92 km).
- **No field, no change.** A request without `exclude_turnarounds` (or with an empty list) takes the v4 path untouched; verified byte-identical against `5b29936f0` on 24 rig requests (6 origins × 30/60/120 km × K 3/12). The seed is unchanged too: rotating it over the pre-pair top-M would change seeded output (a FINGERPRINT bump and a Gate v2 re-run) and still could not stop near-twins from adjacent turnarounds.
- Engines that predate the field ignore it and answer as if it were unsent (verified on `5b29936f0`).

### Per-candidate `provenance` (v4, additive)

Each route's `trip` object carries a fork-local `provenance` object (`TripRoute.CurvagenProvenance`, proto tag 1100 (100 before 2026-11-04); ADR-0041 §7, ADR-0042 §5):

```json
"provenance": {"builder": "pair", "rung": 0, "tier": 0, "relaxed": 2,
               "gated": false, "bridges": 0, "full_repair": true}
```

`builder` is `pair` / `rescue` / `route_leg`. **Additive and consumer-optional**: no other action sets it, the orchestrator drops unknown fields, and no app-contract change follows from it. It exists so the loop-quality harness can *read* a candidate's origin instead of guessing it geometrically.

### Costing option `prefer_curvature`

Proto field **1101** on costing options (97 until the 3.8.2 rebase, commit `a2fd86343`; 101 until 2026-11-04). 0 = neutral; higher = curvier roads favored.

Since (b2) (2026-11-04, `feat/b2-curvature-rescale`) the motorcycle costing also scales every transition cost (turn costs, `maneuver_penalty`, the `curviness_continuity` penalty, gates, tolls) by the smallest edge factor over the smallest edge factor without the fork's discounts: 0.61 / 0.47 / 0.24 at c0.5 / 0.7 / 1.0 with the orchestrator's options, 1 when `prefer_curvature` and `prefer_elevation` are 0. The request shape does not change; route output does. ETAs do not move (only the cost is scaled).

### Proto field numbers

Every fork field in an upstream message takes a fork-private number, old number + 1000, so upstream's dense growth from 1 never collides with it on a rebase (upstream took `Costing.Options` 99 for `steps_factor` in valhalla#6394). Fields of fork-owned messages (`RoundTrip`, `CurvagenProvenance`) keep their own numbering.

| Message | Field | Number |
|---|---|---|
| `Costing.Options` | `reuse_penalty` | 1098 |
| `Costing.Options` | `curviness_continuity` | 1099 |
| `Costing.Options` | `prefer_elevation` | 1100 |
| `Costing.Options` | `prefer_curvature` | 1101 |
| `Options` | `roundtrip` | 1067 |
| `TripRoute` | `curvagen_provenance` | 1100 |

The numbers matter only to protobuf (`format=pbf`) clients; the JSON keys never changed, and the orchestrator speaks JSON.

### Cross-candidate distinctness (xcand) — config knobs

`thor.*` in the service config (ADR-0038 mechanism, ADR-0039 ship decision):

| Knob | Prod value | Meaning |
|---|---|---|
| `thor.roundtrip_xcand_penalty` | `true` | enable the cross-candidate soft surcharge |
| `thor.roundtrip_xcand_strength` | `0.2` | surcharge strength; 0 = off (rollback lever); 0.5 = full win but over the 1.10× latency ratchet on 2-vCPU prod |
| `thor.roundtrip_xcand_cap` | `4` | pile-up cap for the surcharge |

Latency ledger + why 0.2: ADR-0039. Selection-time alternative falsified: ADR-0040 (this repo's `docs/curvagen/adr/`).

**v4 changes where these values come from, not what they are.** ADR-0042 §2 flips fifteen `thor.roundtrip_*` defaults in `src/thor/worker.cc` to the measured configuration, xcand `true` / `0.2` / cap `4` among them — so **a service config carrying no `roundtrip_*` key at all is the measured engine**, and the box's operator-supplied `valhalla.json` no longer has to be right for prod to be right. Any `roundtrip_*` key still present in a deployed config **overrides** a v4 default: read the live `thor` block before a cutover and keep only keys whose values match the defaults. Every knob remains a knob — setting them back reaches v3 / P1 / P2 behaviour for a bisect.

### Distance-error tier — config knob

| Knob | Default | Meaning |
|---|---|---|
| `thor.roundtrip_disterr_tier` | `0.3` | within a rung tier, a built loop whose distance error is above this ranks after every loop within it, however curvy; `0` = off (the ranking before 2026-11-04) |
| `thor.roundtrip_exclude_disterr_guard` | `true` | with `exclude_turnarounds` only: a fresh loop whose distance error is above `roundtrip_disterr_tier` is not served (refused wherever it is built); a short answer goes to the recency fallback, then the plain-run top-up. `false` = the behaviour before 2026-11-05 (and no ride-through test). No effect without exclusions, or with the tier at `0` |
| `thor.roundtrip_exclude_pass_m` | `50` | with `exclude_turnarounds` and the guard on: a fresh loop whose shape passes within this many **meters** of an excluded turnaround is refused like an off-target one. `0` = off |
| `thor.roundtrip_exclude_recency_fallback` | `true` | with 2 or more `exclude_turnarounds`: a short excluded run re-runs excluding only the most recent half of the list (oldest first) and tops up from it before the plain run. `false` = straight to the plain-run top-up |

It only reorders a bank, so it costs no search and no fills. A K = 3 request builds three loops and the tier only orders them.

## FINGERPRINT rule

**Any change in this fork that alters route output** (geometry, candidate selection, scoring, gates, knob defaults) **requires a `FINGERPRINT_VERSION` bump in curvagen-orchestrator** (`crates/cache` keying) — otherwise stale cached banks serve old-engine routes. The same rule is documented consumer-side in `engine-client`. Coordinate via a cross-linked issue pair (driving repo ↔ consumer repo).

## Changelog

- 2026-07-16 — document created at the repo split (curvagen-ios ADR-0042). Surface as shipped: v3 round-trip live since 2026-07-14, xcand at strength 0.2 since 2026-07-16, `prefer_curvature` = field 101 on Valhalla 3.8.2 base.
- 2026-09-07 — **round-trip v4** (valhalla ADR-0042, judged by ADR-0041's Gate v2): loop construction changes, the request shape does not, the response gains the additive per-candidate `provenance`, and the measured configuration moves into `worker.cc` defaults. **v4 alters route output, so the FINGERPRINT rule fires: `FINGERPRINT_VERSION` 4 → 5** in curvagen-orchestrator, in the same commit as the cutover deploy (wayfinder curvagen-valhalla#18). Rollback target is the previous immutable image tag `ghcr.io/lazark0x/valhalla-curvature:amd64-<sha>` plus the previous orchestrator — v4 keys and v3 keys coexist inside the 30 d cache TTL.
- 2026-11-04 — **`roundtrip.exclude_turnarounds` + `exclude_radius_m`** (curvagen-valhalla#28): optional, additive request fields; served turnarounds are dropped before selection, each sector then takes the best of its top 32 (deep pick), and a short excluded run is topped up from a plain run, so fills never drop below today's. Gate v2's T1 for these Shuffles is read against the engine's own best-16 distinct loops (ADR-0041, amendment 2026-11-04). **The FINGERPRINT rule does not fire:** a request without the field is byte-identical to `5b29936f0`, and the orchestrator never caches an excluded answer (no Memo for Shuffles, no Negative Cache, no Bank Fill). Consumer: curvagen-orchestrator's Shuffle memory, which replaces its interim distance rotation (curvagen-orchestrator#67). Deploy the engine first: an orchestrator that sends the field to an older engine is harmless (ignored) but loses the rotation's partial relief until the engine lands.
- 2026-11-04 — **fork proto fields move to a private range** (old number + 1000, table above) ahead of the 3.9.1 rebase. JSON keys are unchanged, so JSON output is byte-identical (verified on the rig against `a3ea836af`). **The FINGERPRINT rule does not fire.** A `format=pbf` client built against the old numbers would no longer see these fields; there is none.
- 2026-11-04 — **engine rebased onto upstream Valhalla 3.9.1** (from 3.8.2; plan in `docs/curvagen/research/2026-11-04-upstream-since-fork.md` on `research/upstream-since-fork`). The surface above is unchanged. Output on the rig tiles, measured against `a3ea836af`: 165/165 byte-identical on the endpoint corpus (round trip K=3/K=12/excluded, one-way + alternates, OSRM navigate, `trace_route`, `/height`), 432/432 Shuffle serves identical, and on loopqual corpus-v2 15 of 552 K=12 answers differ (28 of 6 624 loops, 0.4 %; c0.5 at two urban origins only), where a candidate's pair-pass return no longer builds. Gate v2 meters are equal to the decimal. **The FINGERPRINT rule fires on any output change: `FINGERPRINT_VERSION` 5 → 6** in curvagen-orchestrator, in the same cutover as the engine. Tiles are unchanged (layout-compatible; the engine reads the 3.8.2-built tileset).
- 2026-11-04 — **(b2) curvature rescale + distance-error tier** (branch `feat/b2-curvature-rescale`, on `rebase/3.9.1`; results in `docs/curvagen/research/2026-11-04-b2-curvature-rescale.md`). The motorcycle costing scales its transition costs by how far the fork's discounts take the edge factor below upstream's floor (section `prefer_curvature` above), and `thor.roundtrip_disterr_tier` (default 0.3, table above) ranks loops more than 30 % off the asked distance last. The fork's `kCurvatureFactor` is renamed `kForkCurvatureDiscount` (source only, identical machine code) so upstream #6449's `valhalla::sif::kCurvatureFactor` cannot collide. **Route output changes, so the FINGERPRINT rule fires.** Cut over together with the 3.9.1 rebase and it is the same single bump, **`FINGERPRINT_VERSION` 5 → 6**. Shipped separately after 3.9.1, it needs its own bump (6 → 7). The request and response shapes do not change, so `costing.rs` does not.
- 2026-11-05 — **off-target guard for excluded Shuffles** (`thor.roundtrip_exclude_disterr_guard`, default on; branch `fix/deep-shuffle-distance`). Under `exclude_turnarounds`, a fresh loop past `thor.roundtrip_disterr_tier` is refused, so a deep Shuffle serves a repeat (the plain-run top-up) instead of a loop 2-3x the asked distance. End to end through the orchestrator (8 Shuffles × 8 cities × 30/60 km × curviness 0.5/0.7/1.0, two passes): Shuffles 2-8 max distance error 2.81 → 0.30, p90 over all Shuffles 0.47 → 0.20, loops past 30 % 16 % → 0.5 % (Shuffle 1 only, which carries no exclusions); repeats 9 % → 25 %, fills unchanged, median latency +0.06 s. **The FINGERPRINT rule does not fire:** a request without exclusions is byte-identical (216/216 rig requests, K 3/12, against `f956b19b7`), and the orchestrator never caches an excluded answer.
- 2026-11-05 — **deep-Shuffle repeats under the guard** (same branch). The guard above cut the off-target loops but raised repeats 9 % → 25 %. Four changes, all under `exclude_turnarounds` only: the guard refuses an off-target loop wherever it is built (the twin last resort and the gated stash used to keep it and drop it at the end, leaving the slot to the plain run); the **ride-through test** (`thor.roundtrip_exclude_pass_m`, 50 m) refuses a near-twin that rides through an excluded turnaround; the excluded run searches wider (deep-pick shortlist 64, twice the pair-evaluation budget, 8 more builds); and the **recency fallback** (`thor.roundtrip_exclude_recency_fallback`) tops a short run up from a re-run excluding only the most recent half of the list before the plain run. With curvagen-orchestrator `fix/shuffle-repeats-guard` (its memory keeps cycling a spent cell oldest first instead of restarting it, and sends a 0.06 radius), end to end on the same grid, two passes: repeats in Shuffles 2-8 25 % → 17 %, repeats of a loop from the previous two Shuffles 10 % → 4.5 % (prod: 9.4 % and 5.6 %), distance error p90 0.20 and max 0.30 on Shuffles 2-8 unchanged, fills unchanged, latency median 0.25 → 0.32 s, p90 0.65 → 1.03 s. **The FINGERPRINT rule does not fire:** a request without exclusions is byte-identical (216/216 against `f956b19b7`).

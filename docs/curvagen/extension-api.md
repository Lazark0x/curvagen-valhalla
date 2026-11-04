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

- `exclude_turnarounds`: turnarounds already served, as `{lat, lon}` degrees. A served loop's turnaround is the last point of its leg 0 shape. Entries without a numeric `lat`/`lon` are skipped; at most 64 are read (every harvest candidate is tested against every entry).
- `exclude_radius_m`: **meters**, optional. Default **0.05 × `target_distance`** (= 0.1 × target / 2): the min-separation guard's distance (ADR-0036 T9), the radius inside which two turnarounds already count as the same loop within one answer. An excluded turnaround is treated like a loop already in the bank. Negative values are ignored (default applies). The default is what "the same loop" means inside one answer; across Shuffles near-twins reach farther, and the orchestrator sends **0.08 × `target_distance`** (rig corpus, 6 Shuffles: 16.5 % of later loops near-twins of an earlier one at 0.05, 7.0 % at 0.08, 4.6 % at 0.10 at a p90 distance error of 26 %).
- Behavior: a candidate turnaround within the radius of any excluded point is dropped from the harvest band before node dedup, bearing bucketing and pair selection, and from the Distance Flex re-scan.
- **Deep pick** (only when the list is non-empty): each pair-mode bearing sector evaluates its top 32 candidates instead of `roundtrip_pair_shortlist` (8) and serves its best-scoring survivor instead of the seed-rotated one. The variety comes from the exclusions, so the pick maximises quality (research doc `2026-11-04-shuffle-variety-levers.md` §3.2, branch `research/shuffle-variety`).
- **Fill guarantee.** When the excluded run builds fewer than `num_candidates` loops, the engine runs the cell once more without the exclusions and tops the answer up from that run, in its order, skipping any loop whose turnaround sits within the min-separation guard of a fresh one. Fresh loops rank first, the top-up (the repeats) last. If that would serve fewer loops than the plain run alone, the plain run is served as is. A request with exclusions therefore never returns fewer loops than the same request without them; the 442 stays exactly where it is today. The top-up costs a second run, and only in cells the exclusions exhausted.
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

## FINGERPRINT rule

**Any change in this fork that alters route output** (geometry, candidate selection, scoring, gates, knob defaults) **requires a `FINGERPRINT_VERSION` bump in curvagen-orchestrator** (`crates/cache` keying) — otherwise stale cached banks serve old-engine routes. The same rule is documented consumer-side in `engine-client`. Coordinate via a cross-linked issue pair (driving repo ↔ consumer repo).

## Changelog

- 2026-07-16 — document created at the repo split (curvagen-ios ADR-0042). Surface as shipped: v3 round-trip live since 2026-07-14, xcand at strength 0.2 since 2026-07-16, `prefer_curvature` = field 101 on Valhalla 3.8.2 base.
- 2026-09-07 — **round-trip v4** (valhalla ADR-0042, judged by ADR-0041's Gate v2): loop construction changes, the request shape does not, the response gains the additive per-candidate `provenance`, and the measured configuration moves into `worker.cc` defaults. **v4 alters route output, so the FINGERPRINT rule fires: `FINGERPRINT_VERSION` 4 → 5** in curvagen-orchestrator, in the same commit as the cutover deploy (wayfinder curvagen-valhalla#18). Rollback target is the previous immutable image tag `ghcr.io/lazark0x/valhalla-curvature:amd64-<sha>` plus the previous orchestrator — v4 keys and v3 keys coexist inside the 30 d cache TTL.
- 2026-11-04 — **`roundtrip.exclude_turnarounds` + `exclude_radius_m`** (curvagen-valhalla#28): optional, additive request fields; served turnarounds are dropped before selection, each sector then takes the best of its top 32 (deep pick), and a short excluded run is topped up from a plain run, so fills never drop below today's. Gate v2's T1 for these Shuffles is read against the engine's own best-16 distinct loops (ADR-0041, amendment 2026-11-04). **The FINGERPRINT rule does not fire:** a request without the field is byte-identical to `5b29936f0`, and the orchestrator never caches an excluded answer (no Memo for Shuffles, no Negative Cache, no Bank Fill). Consumer: curvagen-orchestrator's Shuffle memory, which replaces its interim distance rotation (curvagen-orchestrator#67). Deploy the engine first: an orchestrator that sends the field to an older engine is harmless (ignored) but loses the rotation's partial relief until the engine lands.
- 2026-11-04 — **fork proto fields move to a private range** (old number + 1000, table above) ahead of the 3.9.1 rebase. JSON keys are unchanged, so JSON output is byte-identical (verified on the rig against `a3ea836af`). **The FINGERPRINT rule does not fire.** A `format=pbf` client built against the old numbers would no longer see these fields; there is none.

# Curvagen Extension API

The surface this fork adds over upstream Valhalla, consumed by **curvagen-orchestrator** (`crates/engine-client`). Provider-owned per curvagen-ios ADR history (D5, wayfinder map Lazark0x/curvagen#104): changes to this surface ride the same commit as the implementing code, logged in the Changelog below.

## Surface

### `/roundtrip` (thor action)

Native round-trip loop construction (ADR-0033, rebuilt as v3 per ADR-0037, then as **v4 per ADR-0042** — see `docs/curvagen/adr/`).

- Request: origin `locations[0]`, `distance` (**meters**), `prefer_curvature` (0..1), `seed`, costing (motorcycle). **Unchanged by v4** — no new request field, no new rider knob.
- Response: standard Valhalla directions shape; one leg per candidate loop. **v4 adds one additive per-candidate field** (below).
- Behavior contracts: defect-gated construction (spike/backtrack gates), distance tolerance per ADR-0037, K-candidate bank fill served through the orchestrator's cache. **v4** adds road identity (twin/parallel exclusion), harvest hygiene, disjoint-pair selection and distinctness at selection; the loop *shapes* change, the shape of the wire does not.

### Per-candidate `provenance` (v4, additive)

Each route's `trip` object carries a fork-local `provenance` object (`TripRoute.CurvagenProvenance`, proto tag 100; ADR-0041 §7, ADR-0042 §5):

```json
"provenance": {"builder": "pair", "rung": 0, "tier": 0, "relaxed": 2,
               "gated": false, "bridges": 0, "full_repair": true}
```

`builder` is `pair` / `rescue` / `route_leg`. **Additive and consumer-optional**: no other action sets it, the orchestrator drops unknown fields, and no app-contract change follows from it. It exists so the loop-quality harness can *read* a candidate's origin instead of guessing it geometrically.

### Costing option `prefer_curvature`

Proto field **101** on costing options (relocated from 97 at the 3.8.2 rebase, commit `a2fd86343`). 0 = neutral; higher = curvier roads favored.

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

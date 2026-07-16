# Curvagen Extension API

The surface this fork adds over upstream Valhalla, consumed by **curvagen-orchestrator** (`crates/engine-client`). Provider-owned per curvagen-ios ADR history (D5, wayfinder map Lazark0x/curvagen#104): changes to this surface ride the same commit as the implementing code, logged in the Changelog below.

## Surface

### `/roundtrip` (thor action)

Native round-trip loop construction (ADR-0033, rebuilt as v3 per ADR-0037 — see `docs/curvagen/adr/`).

- Request: origin `locations[0]`, `distance` (**meters**), `prefer_curvature` (0..1), `seed`, costing (motorcycle).
- Response: standard Valhalla directions shape; one leg per candidate loop.
- Behavior contracts: defect-gated construction (spike/backtrack gates), distance tolerance per ADR-0037, K-candidate bank fill served through the orchestrator's cache.

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

## FINGERPRINT rule

**Any change in this fork that alters route output** (geometry, candidate selection, scoring, gates, knob defaults) **requires a `FINGERPRINT_VERSION` bump in curvagen-orchestrator** (`crates/cache` keying) — otherwise stale cached banks serve old-engine routes. The same rule is documented consumer-side in `engine-client`. Coordinate via a cross-linked issue pair (driving repo ↔ consumer repo).

## Changelog

- 2026-07-16 — document created at the repo split (curvagen-ios ADR-0042). Surface as shipped: v3 round-trip live since 2026-07-14, xcand at strength 0.2 since 2026-07-16, `prefer_curvature` = field 101 on Valhalla 3.8.2 base.

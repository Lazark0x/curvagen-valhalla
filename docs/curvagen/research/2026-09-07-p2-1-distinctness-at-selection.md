# P2.1 — distinctness at selection: the per-leg sharing threshold on the pair's forward leg

- **Date:** 2026-09-07 (curvagen-valhalla [#14](https://github.com/Lazark0x/curvagen-valhalla/issues/14), P2.1 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); Gate v2 = [ADR-0041](../adr/0041-roundtrip-v4-loop-quality-gate-v2.md))
- **Scope:** a **throwaway prototype** on branch `proto/v4-p2.1`, cut from `proto/v4-p2` @ `4402a2604` (the measured P2 binary `83837debc` plus its docs). One question: can the pair pass's bank be made distinct **at selection** — before any search, where the pass already holds every candidate's pair — so that P2 meets Gate v2's served-surface distinctness ratchet (**T2**) while keeping what P2 won (R1–R5, T1, T3–T5). Nothing pushed; no production traffic; `valhalla-local` (:8002) and the :8791 results server never addressed.
- **Method:** P2 §7's paired protocol on corpus-v2 (552 requests, K = 12, engine mode, 3 workers, way pass on), **both engines at the production configuration** (`roundtrip_xcand_penalty` 0.2 cap 4): baseline `b4f514d7f` on :8004 (`rt-p11-base`), P2.1 on :8003 (`rt-p1-build`), one engine at a time, same-session baseline brackets A and B around the variants. Readings: `served_surface.py` (T2, reproduces ADR-0041's prod / knee / P2 = 0.3923 / 14.4 %, 0.4475 / 22.0 %, 0.5879 / 50.4 %), `gate_v2_read.py` (every tier; reproduces the ADR's R1 43.9 / 2.4 / 3.8 %, T6, T1), the switchback-aware detector pass (`p2-analyse-all.sh`), `leg_overlap_served.py` (per-leg anatomy), the engine's own ledger.
- **Session note:** built and measured by an AFK agent 2026-09-07; this document was written incrementally as each run landed (the P2 session's lesson).

## TL;DR verdict

_(filled when the primary reading lands — see §4.)_

## 1. The mechanism

P2's diagnosis (its §7.8, §12): the pair-keyed K × K filter is nearly inert (0.018) because it judges the *pair* while the rider gets the *repair*, and because its 0.6 threshold is on the whole pair — a forward trunk shared over 60 % of the forward leg is ~30 % of a loop. On the Served Surface (blocks A + B, slots 0–5, prod config) P2's excess sits on **both** legs: best-sibling overlap 0.588 = forward 0.342 + return 0.246 against prod's 0.392 = 0.242 + 0.151; the forward share of the forward leg is 0.68 (> 0.5 on 81.9 % of served loops) against prod's 0.48, the return share of the return leg 0.49 against 0.30 (`leg_overlap_served.py`, this session). The working hypothesis: sinks clustered on the same trunk also share the way home, so thinning forward trunks at selection pulls the returns along.

Everything below sits in `src/thor/route_action.cc` on top of P2, all behind knobs that default **off** (the P2 behaviour is byte-identical when unset; the P2.1 configs turn them on):

| knob (`thor.`) | default | what it does |
|---|---|---|
| `roundtrip_pair_leg_sharing` | false | the **per-leg threshold at selection**. `PairKeys` gains `fwd_ridden` (canonical road id → metres beyond the Start Exemption on the TREE path, which *is* the served forward leg; the 2.5 % orientation swaps ride the other path out and are ignored) and `fwd_total` (the tree path's length). `fwd_shared_fraction(cand, prev)` = Σ metres of `cand.fwd_ridden` whose key is among `prev.ridden` or `prev.twins`, over `cand.fwd_total`. A candidate is refused when that fraction `>` the threshold against ANY entry of the bank being tested: at the sector shortlist and the backfill against `pair_selected_keys`, at dequeue (and inside `attempt_pair_build`) against `pair_built_keys`. Ledger `leg_share=`. |
| `roundtrip_pair_leg_sharing_frac` | 0.5 | the threshold, as a fraction of the forward leg (0.5 = variants a/b, 0.35 = variant c). |
| `roundtrip_pair_built_keys` | false | **built-loop keys**: at the commit site the bank entry is keyed on the BUILT loop via `loop_keylen(*built)` — both legs as served, twins as `twins` — for EVERY committed loop (rescue-built too), not `pair_cache[cand].keys`. Later candidates are then tested against the returns actually served. |
| `roundtrip_pair_diversity_w` | 0 | the **diversity term** in the sector pick: survivors are ordered by `score / (1 + w · s)`, `s` = max over `pair_selected_keys` of the whole-pair twins-aware `shared_fraction`; the seed rotation over the ordered survivors is unchanged (it bounds the term: the term orders, the seed still picks among the survivors). |
| `roundtrip_pair_leg_relax` | false | the **relaxation ladder** for the fills bar: after the main `build_loop()` and before the twin last resort / rescue pass, if `loops.size() < want`, the queue is re-walked (`qi = 0`) at `frac + 0.15`, then `frac + 0.30`, then with the leg test off — stopping when full. Loops admitted under a rung carry `Loop.relaxed` (1–3). Builds spend attempts like any other; the ladder is granted the missing count + `kAttemptSlack` once (the rescue pass is granted `want` + slack). The whole-pair filter stays on at every rung — rung 3 *is* P2's selection contract inside a budget. Ledger `leg_relaxed=` (loops), `leg_relax_rung=` (highest rung used). |
| `roundtrip_pair_relaxed_last` | false | ranking: within a tier, non-relaxed before relaxed, then score. |
| `roundtrip_pair_eval_cap` (P2 knob, re-purposed in leg mode) | 600 | **the per-rung evaluation budget.** In P2 the cap *dropped the filter* past N evaluations and the walk went on. In leg mode it bounds the WALK: fresh pair evaluations per rung (selection + backfill + main walk = rung 0; each relaxation rung its own), and a rung whose budget is spent ends its walk (`underfill=evalcap`) and hands the queue to the next rung. Cached verdicts are free on a re-walk. Set to **3 000** in every P2.1 config (≈ 2× P2's mean of 1 710). The budget bounds *fresh* evaluations only — a cached verdict (the chosen set, everything the backfill already walked) is always processed; the first cut checked the budget at the top of the build loop and starved rung 0 of its own chosen candidates whenever the selection walk had spent it (run **a0**, §7.2; fixed in `a4591a321`). Ledger `rung_evals=e0/e1/e2/e3`. |

| `roundtrip_pair_eval_total` | 0 (off) | **a per-request total** of fresh evaluations across all rungs — a0 (§7.2) showed the per-rung budget alone lets a hard request spend four budgets (5 224 evaluations per request, p50 3 461; eval 672 ms mean; wall p50 1.46×), so the tail needs one bound. a1 / a2 run at **2 000** (≈ P2's own volume of 1 710). |
| `roundtrip_pair_relax_eval_cap` | 0 (= `eval_cap`) | the relaxation rungs' own cap on FRESH evaluations — a re-walk re-tests the cached candidates for free and should evaluate only a few new ones. a1 / a2: **300**. |
| `roundtrip_pair_share_relax` | false | ladder the **whole-pair** threshold with the leg: 0.6 → 0.75 → 0.9 → off. In a0 the whole-pair 0.6 test — P2's `pair_shares`, now keyed on built loops (forward + served return + twins) — was the dominant rejecter (1 773 per request against the leg test's 224) and was never relaxed, so rung after rung was spent re-rejecting on it. Variant **a1** drops the whole-pair test (`roundtrip_pair_sharing false`), **a2** ladders it; a1 vs a2 says whether the T2 gain comes from the leg test or from the whole-pair-vs-built test. |

Two things the v0 run forced (§7): rejected evaluations are **memory-light** — a `no_pair` / `band` / `fwd_illegal` / `twin` verdict keeps `checked / ok / reject` and releases the arc paths and the key containers (`release_eval`); accepted candidates keep their record. The ledger carries a coarse memory guard: `cache=` (evaluations cached) and `keys_held=` (entries still holding keys). A second ledger line, `roundtrip pair-leg: req=<lat>_<lon>_<target>_<seed>_<curv>_<hw> slots=<slot>:<rung>/<pair_built>/<tier> …`, gives the per-slot provenance keyed by request so it can be joined to the response (three workers interleave the ledger). Every new pair-select field is **appended** after `bad_ret_edge=` (`leg_share leg_relaxed leg_relax_rung div_w built_keys rung_evals share_relax relax_cap eval_total cache keys_held`); the P2 parsers (`p2_ledger.py`, `ledger_agg.py`, `stage_total_p2.py`) read the prefix and still run on the new ledger (§Appendix A).

## 2. Gurka — 51 green

`gurka_roundtrip_audit` **26/26** (4.6 s) · `gurka_motorcycle_roundtrip` **22/22** · `gurka_roundtrip_distinctness` **3/3** on the P2.1 binary (`~/.curvagen-scratch/p21/gurka-p21-audit.log`, `gurka-p21-others.log`); the 49 P2 tests unchanged (every new knob is off by default).

**The comb map** (`RtP21Comb`, `test/gurka/test_roundtrip_audit.cc`): two trunk roads leave the start S, each carrying two curvy lobes at the band distance, every lobe with its own long straight road home through an exempt stem — trunk 1 `S-M-J` (M inside the Start Exemption, M-J 3.01 km, curvature 8), lobes `J-A` / `J-B` (0.8 km, curvature 15), returns `A-Q-S` / `B-Q-S`; trunk 2 the mirror image with lobes at curvature 6. Target 11.5 km. The geometry is chosen so that the lobes' forward legs (5.25 / 5.32 km) are in the band, the trunk-only sinks J / K (4.46 km) and the *outward* arrivals along the return roads (7.27 / 7.52 km) are out of it — the first cut of the map had those in band, and the lobe candidates inherited the straight chain's curviness from the first label seen at the junction (P2's `pair_adopt` re-hangs the label, not the score), which put the trunk-only sinks first and made the control land on different trunks by accident. The trunk beyond the exemption is **57 % of the second lobe's forward leg but 23 % of its loop**: P2's whole-pair 0.6 cliff keeps both trunk-1 lobes, the per-leg threshold at 0.5 refuses the second one, and the ladder's first rung (0.65) admits it while the trunk-only sinks (67 %) stay refused.

| test | pins | result |
|---|---|---|
| `RtP21Comb.P2e_ForwardLegsLeaveOnDifferentTrunks` | **control** (P2, leg sharing off, K = 2): slots 0–1 = `S-M-J-A` and `S-M-J-B` — both on trunk 1 (asserted, so the pin is non-vacuous); **treatment** (per-leg 0.5): slot 0 `S-M-J-A`, slot 1 `S-N-K-C` — the second slot leaves on the other trunk, refused at selection before any search | PASS |
| `RtP21Comb.P2f_RelaxationFillsTheBankAndRanksLast` | K = 4 against a forward-distinct supply of 2 with the ladder on: the bank fills 4/4; the engine's own counters read `leg_share=6 leg_relaxed=2 rung=1` and the per-slot line `0:0/1/0 1:0/1/0 2:1/1/0 3:1/1/0` (rung / pair-built / tier) — slots 0–1 are the two strict loops on different trunks (A, then D — the seed rotation's pick in sector 1), slots 2–3 the rung-1 loops (B, C); **control** with `relaxed_last` off: the score order puts both trunk-1 lobes into slots 0–1 | PASS |

One harness finding on the way: nothing under `gurka::do_action` can re-point the logger — `midgard::logging::GetLogger` is a one-shot static, initialised as the null logger by `buildtiles` — so P2b's `std_err` setting never printed anything, and P2f reads the engine's `ROUNDTRIP_DEBUG` stderr mirror (which now also echoes the per-slot line) instead of the ledger.

## 3. The runs

All on the prod-equivalent Serbia tiles, corpus-v2 (552 requests, K = 12), engine mode, 3 workers, way pass on, one engine at a time, **both engines with `roundtrip_xcand_penalty` on at 0.2 cap 4** (the prod condition). Baseline = `rt-p11-base` (:8004, `b4f514d7f`, `/tmp/v8004-xcand.json` + stage timing); P2.1 = `rt-p1-build` (:8003, `proto/v4-p2.1` @ `6f396e85f`, configs `/tmp/v8003-p21-*.json` layered on `/tmp/v8003-p2v2-xcand.json` = the P2 knee + pair pass + xcand 0.2). Every P2.1 config: `roundtrip_pair_eval_cap 3000`. Drivers and logs in `~/.curvagen-scratch/p21/` (`p21-run.sh` adds an engine watchdog + a memory log per run); ledgers in `~/.curvagen-scratch/p2/eng-<tag>.log`.

| run | tag | config on top of P2 + xcand 0.2 | results dir | purpose |
|---|---|---|---|---|
| v0 | `p21v0` | P2 binary, `roundtrip_pair_eval_cap` 1e6, `roundtrip_sharing_frac` 0.4 | `p2-1-v0/` | **OOM-killed after 15 requests** (§7.1) — evidence only |
| baseline bracket **A** | `p21baseA` | — | `census-v2-p21-baseA/` | latency bracket before the variants |
| a0 | `p21a0` | `leg_sharing` 0.5, `built_keys`, `leg_relax`, `relaxed_last`, `eval_cap` 3 000/rung — binary `6f396e85f` (budget checked at the top of the build loop) | `p2-1-a0/` | **the starved rung 0** (§7.2): effectively the ladder at 0.65–0.80; prices the 3 000 budget |
| v0b | `p21v0b` | new knobs off, `roundtrip_sharing_frac` 0.4, `eval_cap` 3 000 (P2 semantics: filter dropped past the cap) | `p2-1-v0b/` | what P2's whole-pair filter buys inside a budget |
| **a1** | `p21a1` | binary `23ae41e64`: per-leg 0.5, built keys, ladder, relaxed_last, **whole-pair test off**, `eval_total` 2 000, `eval_cap` 2 000, `relax_eval_cap` 300 | `p2-1-a1/` | the leg test alone, at P2's evaluation volume |
| **a2** | `p21a2` | a1 with the whole-pair test **on, fixed at 0.6** (redefined after a1 — see below) | `p2-1-a2/` | leg laddered + whole-pair-vs-built at T2's own criterion |
| **c** | `p21c` | the better of a1 / a2 with `leg_sharing_frac` 0.35 | `p2-1-c/` | the tighter threshold |
| b | `p21b` | the better of a1 / a2 + `diversity_w` 2.0 — if time allows | `p2-1-b/` | the diversity term |
| **d** | `p21d` | a2 at `eval_total` 1 200 (`eval_cap` 1 200, `relax_eval_cap` 150) + `roundtrip_pair_rescue_last` (rescue-built loops rank behind pair-built ones within a tier) — binary `f4a50bdbb` | `p2-1-d/` | the decision run: the T3 side of the front |
| baseline bracket **B** | `p21baseB` | — | `census-v2-p21-baseB/` | latency bracket after the variants (T3 pools A + B) |

**Bracket A** (12:23–12:32Z, `census-v2-p21-baseA/`): wall p50 **1.165 s**, p95 3.337 s, fills 550/552, failures 0, engine-stage total 824.4 ms mean / 709.5 ms p50, attempts 12.56 — against the P2 session's prod-condition bracket X (1.189 s / 3.123 s, 860.7 / 757.0 ms). Its meters are **byte-identical to bracket X's** (`same_meters.py`: 0 of 6 621 loops differ — deterministic engine, same binary and config), so the switchback-aware detector read of X (`p21bx`) is the read of every prod-condition baseline bracket in this session.

**a0** (12:40–12:54Z): T2 **0.4829 / 21.0 %** (1.231× / +6.6 pp, FAIL — but −0.105 / −29 pp from P2's 0.5879 / 50.4 %; block A 0.4677 / 23.3 %, block B 0.4899 / 19.9 %; c0.5 0.4716 / 16.6 %, c0.7 0.5061 / 24.9 %, c1.0 0.4848 / 24.9 %); fills 552/552, failures 0; wall p50 **1.700 s** (1.46× bracket A), p95 **10.06 s**; engine-stage total 1 442 ms (eval 672 ms mean, p50 269, max 6 829); evaluations 5 224 per request (p50 3 461; `rung_evals` means 2 730 / 1 221 / 738 / 535), `leg_share` 224, whole-pair `share` **1 773** (P2: 130 — the built-loop keys see the repaired returns), `leg_relaxed` 8.48 per request (464 requests with any; highest rung 1 / 2 / 3 on 318 / 90 / 56), rescue loops 955 (P2: 477), `underfill=evalcap` on 438; container memory max 2.4 GiB. Per slot: relaxed share 59 % (slot 0) → 82 % (slot 11); pair-built 81–91 %; tier 0 95 % in slots 0–9.

**v0b** (12:54–13:05Z, `p2-1-v0b/`; the P2 mechanism, new knobs off, `roundtrip_sharing_frac` 0.4 inside `roundtrip_pair_eval_cap` 3 000 with P2's semantics — the filter is dropped past the cap, the walk goes on): T2 **0.5508 / 42.0 %** (1.404× / +27.6 pp, FAIL; block A 0.4832 / 30.2 %, block B 0.5815 / 47.4 %); fills 552/552, failures 0; wall p50 **1.567 s** (1.35× bracket A), p95 5.15 s; engine-stage total 1 165 ms (eval 338 ms mean, max 4 938); evaluations 3 700 per request (p50 3 057, **max 31 070** — with the filter dropped the walk is bounded only by the bank filling), whole-pair `share` rejects 803 per request, rescue loops 667 (70 requests); memory max 1.65 GiB. **P2's own filter, tightened to 0.4, buys 0.037 of the served mean and −8 pp of near-dups for a third more latency** — the pair-keyed whole-pair test is the wrong instrument, as P2 §5 said.

**a1** (13:07–13:18Z, `p2-1-a1/`; the leg test alone — whole-pair test off — total budget 2 000, relax rungs 300 fresh): T2 **0.4937 / 29.1 %** (1.259× / +14.7 pp, FAIL — *worse than a0 on both*; block A 0.4719 / 30.2 %, block B 0.5036 / 28.6 %); fills 552/552, failures 0; wall p50 **1.416 s** (1.22× bracket A), p95 5.69 s; engine-stage total 1 205 ms (eval 217 ms mean, p50 133, max 4 516); evaluations 1 867 per request (p50 2 001 = the whole budget, spent at rung 0 on the median request: `rung_evals` 1 867 / 0.14 / 0.11 / 0.03 — the relaxation rungs only re-test the cache), `leg_share` 782 per request, chosen 7.33, relaxed loops 2.89 (highest rung 1 / 2 / 3 on 139 / 173 / 116 requests), rescue loops 1.67 (156 requests), `underfill=evalcap` on 473; memory max 1.66 GiB. Per slot: relaxed share 0 / 0 / 0.4 / 0.9 / 2.9 / 6.7 % in slots 0–5 (the ladder's loops sit in slots 6–11), pair-built 83–85 %.

**What a1 says.** (i) The whole-pair-vs-built test was doing the T2 work in a0: T2's near-dup criterion *is* whole-loop overlap > 0.6 against **any** bank member, and with the ladder filling slots 6–11 with loops admitted at 0.65–0.80 of the forward leg, those deep loops are charged to the served ones — without a whole-pair test at 0.6 nothing stops a relaxed loop from being a 70 % copy of slot 1. (ii) Laddering the whole-pair threshold (the a2 as first defined) cannot buy latency, because the relaxation rungs already spend ~0 fresh evaluations under a total budget. So **a2 was redefined before it launched**: whole-pair test **on and fixed at 0.6**, the leg test laddered, total 2 000 — a0's configuration with the correct rung-0 semantics and a bounded budget. (The `engine` note in `p2-1-a2/report.json` still says "laddered"; the ledger's `share_relax=0` is the truth.)

**a2** (13:21–13:32Z, `p2-1-a2/`; leg 0.5 laddered, whole-pair test fixed at 0.6 against the built bank, total budget 2 000, relax rungs 300 fresh): T2 **0.4657 / 21.7 %** (1.187× / +7.3 pp, FAIL — the best of the session; block A **0.4135 / 19.0 %**, block B 0.4895 / 23.0 %; c0.5 0.4510 / 18.1 %, c0.7 0.4706 / 24.6 %, c1.0 0.4838 / 25.1 %; slots 0–2 0.4690 / 21.7 %); fills 552/552, failures 0; wall p50 **1.546 s** (1.33× bracket A), p95 4.71 s; engine-stage total 1 058 ms (eval 183 ms mean, p50 140, max 1 364); evaluations 1 880 per request (p50 2 002 — the budget, spent at rung 0), whole-pair `share` rejects 865, `leg_share` 183, chosen 7.08, built 9.39 pair-built + rescue loops **2.98** per request (301 requests with any; slots 0–5 only 66–75 % pair-built — the whole-pair test sends more slots to P1.1's rescue builder), relaxed loops 1.32 (none in slots 0–5); memory max 1.57 GiB.

_(c and bracket B follow as each lands; the fixed **a** at 3 000/rung was dropped from the matrix once a0 and v0b had priced that budget — see §3.1.)_

### 3.1 Why the matrix changed mid-session

a0 (the first cut) put T2 within 1.6 pp of the near-dup bar and 0.05 off the mean bar — but at 1.46× wall p50, and the ledger said why: the per-rung budget multiplies (four rungs × 3 000 fresh evaluations on hard requests), and the dominant rejecter was not the leg test but P2's whole-pair 0.6 `pair_shares`, now keyed on built loops and never relaxed, so every rung was spent re-rejecting on it. The evaluation volume *is* the latency regression (P2: 113 ms of evaluation at 1 710 per request; a0: 672 ms at 5 224). So instead of b / c on the 3 000 budget: **one total budget per request (2 000, P2's own volume), the relaxation rungs re-testing the cached candidates first and evaluating at most 300 new ones each, and the whole-pair test either off (a1) or laddered with the leg (a2)** — a1 vs a2 tells whether the T2 gain comes from the leg test or from the whole-pair-vs-built test; then c (0.35) on the better of the two, b (diversity) if time allows. The coordinator's independent read of a0 reached the same diagnosis.

## 4. Gate v2 — all tiers, all runs

Read by `gate_v2_read.py` (served surface = blocks A + B, slots 0–5; R1 = D1 `am10_max_run_m ≥ 500` ∨ D4 `reuse_disc_m ≥ 500` ∨ D1L `am25_max_run_m ≥ 1500` on the switchback-aware detector output; the reader reproduces ADR-0041's R1 43.9 / 2.4 / 3.8 %, T6 and T1). Bars: T1 per level vs the census run `census-v2-b4f514d7f` (`curviness_geom_clean`, whole corpus); T3 vs the pooled brackets A + B (until B lands: bracket A 1.165 s); T4 on A + B vs prod's A + B p95 (bracket X 2.977 s, bracket A 3.337 s — the pooled figure is used when B lands); T5 reported on both surfaces (§4.1). Columns: census (`cen`), prod condition (`prodX`, = brackets A/B byte-for-byte), P2 + xcand (`p2x`, the same detector read for the first time), the P2.1 runs.

| tier | bar | cen | prodX | p2x | a0 | v0b | a1 | a2 | c |
|---|---|---|---|---|---|---|---|---|---|
| R1 Retrace family, served | ≤ 5 % | 43.9 | 43.8 | 2.8 | **3.0** ✓ | n/a | | | |
| R2 `spike_ge_500m` | 0 | 0 | 0 | 0 | **0** ✓ | 0 ✓ | | | |
| R3 dist err c0.5 / c0.7 / c1.0 mean (p90) | ≤ 0.22 (0.42) / 0.32 (0.65) / 0.32 (0.65) | .169/.108/.134 | .139/.109/.134 | .134/.104/.109 | **.155/.115/.119** (p90 .27/.20/.20) ✓ | .136/.108/.108 ✓ | | | |
| R4 fills 12/12 | 552 | 550 | 550 | 552 | **552** ✓ | 552 ✓ | | | |
| R5 failures | 0 | 0 | 0 | 0 | **0** ✓ | 0 ✓ | | | |
| T1 curviness c0.5 / c0.7 / c1.0 (all, ×census) | ≥ 0.95× | 1 | 1.01/1.01/1.00 | 1.06/1.28/1.21 | **1.04/1.24/1.11** ✓ | 1.04/1.24/1.12 ✓ | | | |
| **T2** served mean / near_dup > 0.6 | ≤ 0.4315 / ≤ 19.4 % | .4412/23.8 | .3923/14.4 | .5879/50.4 | **.4829/21.0** ✗ (1.231×/+6.6 pp) | .5508/42.0 ✗ | | | |
| T3 wall p50 (× bracket A 1.165 s) | ≤ 1.10× | 1.305 | 1.189 | 1.486† | **1.700 = 1.46×** ✗ | 1.567 = 1.35× ✗ | | | |
| T4 wall p95 A+B (× prod 2.98–3.34 s) | ≤ 1.30× | 3.50 | 2.977 | 5.459† | **7.72 = 2.3×** ✗ | 4.42 = 1.3–1.5× ✗ | | | |
| T5 near-mirror mean m, A+B served (bar 1 198 = 0.25 × 4 792) | ≤ 1 198 | 4 792 | 4 861 | 29 | **44** ✓ | n/a | | | |
| T5 on the ADR surface, all blocks s0–2 (bar 865 = 0.25 × 3 462) | ≤ 865 | 3 462 | 3 359 | 41 | **51** ✓ | n/a | | | |
| T6 deep-bank Retrace family | ≤ 53 % | 53.0 | 51.1 | 2.7 | **1.6** ✓ | n/a | | | |
| C1 `spike_ge_30m` · C2 `edge_reuse_geom` served | report | 0 · .021 | 0 · .021 | 0 · 0 | 0 · 0 | 0 · 0 | | | |
| A4 D1b ≥ 500 m served | advisory | 55.1 | 56.7 | 3.1 | 4.7 | n/a | | | |

† the P2-session `p2-v2-xcand` run overlapped detector passes; its latency is not a reading (P2 §7.8 re-measured it uncontended at 1.102 s / 4.05 s).

### 4.1 The T5 erratum

ADR-0041's Baseline v2 figure for T5 (3 462 m) does not reproduce on the served surface as the ADR defines it (blocks A + B, slots 0–5 → **4 792 m**; prod-condition bracket 4 861); it reproduces exactly on **all blocks, slots 0–2** (3 462; P2 reads 41 there against the ADR's 44 — the ADR's P2 figure was read before the xcand re-run). Both bars are reported; every P2-family run passes both by two orders of magnitude, so the erratum does not change a verdict — the v4 build task should pin the surface in `gate_v2.py`.

## 5. T2 anatomy

_(per-leg split, per block / level / slot, relaxation and rescue counts, where near-dups remain.)_

## 6. Latency

_(brackets, stage anatomy, per-ask if the tail moved.)_

## 7. What did not work

### 7.1 v0 — the uncapped whole-pair filter (config only, the P2 binary)

Variant v0 (`v8003-p21-v0.json`: the P2 binary `83837debc`, xcand 0.2, `roundtrip_pair_eval_cap` 1 000 000, `roundtrip_sharing_frac` 0.4) answered **15 requests** and was **OOM-killed** at the first 200 km Belgrade asks (`rt-p1-build` `OOMKilled=true`; the remaining 537 requests read status 0 in ~1 ms; the Docker VM's 5.77 GiB is shared by both containers, 3 workers). The last completed 200 km request's ledger (`~/.curvagen-scratch/p2/eng-p21v0.log`, request 17): `pair-select: considered=44906 no_pair=11977 band=15456 twin=1234 share=19689 chosen=9; built=8` on a harvest of 617 684 labels / 285 753 junctions. With the sharing test never dropped and a strict threshold, the sector shortlist filled only 9 sectors, and the backfill and the refill queue walked essentially the whole band — 44 906 pair evaluations for one request against P2's mean of 1 710 (p50 730) — every one cached as a full `PairEval` (two arc vectors, a `PairKeys` with an `unordered_map` of ~300 roads and a twin set): three concurrent 200 km requests blew the memory, and the latency would have been seconds per request as well. **The uncapped K × K filter is not a usable variant as-is**; `results/p2-1-v0/` is kept as evidence and not measured. The consequences are built into P2.1: the per-rung evaluation budget and the memory-light rejects of §1; v0 is re-run bounded on the new binary as **v0b** (`roundtrip_pair_eval_cap` 3 000, `roundtrip_sharing_frac` 0.4, new knobs off) — "what does P2's whole-pair filter buy inside a budget".

### 7.2 a0 — the starved rung 0 (the first cut of the budget check)

The first corpus run of variant a (`p2-1-a0/`, binary `6f396e85f`) had the per-rung evaluation budget checked at the top of the build loop. On requests where the selection walk (sector shortlist + backfill under the strict test) had already spent rung 0's 3 000 evaluations, the build loop broke before building even the chosen candidates — a typical ledger line reads `chosen=11 … rung_evals=3000/62/0/0 … leg_relaxed=12 leg_relax_rung=1`: eleven chosen, none built at rung 0, the whole bank built at rung 1 (threshold 0.65) with 62 fresh evaluations. `underfill=evalcap` on 112 of the first 197 requests. The fix (`a4591a321`) bounds fresh evaluations only. a0 is kept as evidence and read in §4 as "the ladder-only variant" (effective threshold 0.65–0.80 on those requests); it is not the primary.

_(a0's numbers: filled from `read-a0.txt`.)_

### 7.3 Not built: the leg test before the pair construct

Suggested for d: walk the tree path (the label chain) for the forward keys and reject a leg-sharing sink *before* constructing its pair. The a2 ledger says it would not pay here: the leg test rejects 183 sinks per request, the whole-pair-vs-built test 865 (which needs the full walk), and 77 % of evaluations are `no_pair` / `band` rejects that today pay only the O(1) existence check and the construct — a pre-walk would add a tile-lookup walk to those. The evaluation budget is the lever that buys evaluation back, and d takes it (1 200 total, 150 per relaxation rung).

## 8. Pareto front / recommendation for the v4 build

_(filled at the end.)_

## 9. Open questions

_(filled at the end.)_

## Appendix A — commands

_(filled at the end.)_

## Appendix B — artefacts

_(filled at the end.)_

## Appendix C — the branch

_(filled at the end.)_

# P2.1 — distinctness at selection: the per-leg sharing threshold on the pair's forward leg

- **Date:** 2026-09-07 (curvagen-valhalla [#14](https://github.com/Lazark0x/curvagen-valhalla/issues/14), P2.1 of the round-trip v4 ladder [#4](https://github.com/Lazark0x/curvagen-valhalla/issues/4); Gate v2 = [ADR-0041](../adr/0041-roundtrip-v4-loop-quality-gate-v2.md))
- **Scope:** a **throwaway prototype** on branch `proto/v4-p2.1`, cut from `proto/v4-p2` @ `4402a2604` (the measured P2 binary `83837debc` plus its docs). One question: can the pair pass's bank be made distinct **at selection** — before any search, where the pass already holds every candidate's pair — so that P2 meets Gate v2's served-surface distinctness ratchet (**T2**) while keeping what P2 won (R1–R5, T1, T3–T5). Nothing pushed; no production traffic; `valhalla-local` (:8002) and the :8791 results server never addressed.
- **Method:** P2 §7's paired protocol on corpus-v2 (552 requests, K = 12, engine mode, 3 workers, way pass on), **both engines at the production configuration** (`roundtrip_xcand_penalty` 0.2 cap 4): baseline `b4f514d7f` on :8004 (`rt-p11-base`), P2.1 on :8003 (`rt-p1-build`), one engine at a time, same-session baseline brackets A and B around the variants. Readings: `served_surface.py` (T2, reproduces ADR-0041's prod / knee / P2 = 0.3923 / 14.4 %, 0.4475 / 22.0 %, 0.5879 / 50.4 %), `gate_v2_read.py` (every tier; reproduces the ADR's R1 43.9 / 2.4 / 3.8 %, T6, T1), the switchback-aware detector pass (`p2-analyse-all.sh`), `leg_overlap_served.py` (per-leg anatomy), the engine's own ledger.
- **Session note:** built and measured by an AFK agent 2026-09-07; this document was written incrementally as each run landed (the P2 session's lesson).

## TL;DR verdict

**Distinctness at selection moves the served surface most of the way and stops short of Gate v2's T2, and every variant that gets close costs 1.22–1.34× at the median.** P2.1's best points: **d** (per-leg 0.35 laddered, whole-pair 0.6 against the built bank, one 1 200-evaluation budget, rescue-built loops ranked last) — served `bank_overlap_mean` **0.4443** (1.133× prod; bar ≤ 1.10× = 0.4315) and `near_dup > 0.6` **24.3 %** (bar ≤ 19.4 %), wall p50 **1.22×** the pooled brackets (bar 1.10×); **c** (the same at a 2 000 budget, rescue loops not re-ranked) — 0.4521 / **22.4 %**, 1.23×; **a2** (0.5 threshold) — 0.4657 / **21.7 %**, 1.34×. From P2's 0.5879 / 50.4 % that is −0.14 / −26 to −29 pp. Everything else holds on every variant: R1 Retrace family 1.9–3.0 %, R2 spikes 0, R4 552/552, R5 0, T1 1.04–1.06 / 1.19–1.29 / 1.09–1.16×, **T4 1.12–1.28× (passes)**, T5 36–47 m, T6 1.6–4.2 %, memory ≤ 2.4 GiB.

- **What did it.** The per-leg test alone (a1) fixes the forward leg but not the count (0.494 / 29 %); the whole-pair 0.6 test against the *built* bank (a2 vs a1) is what brings the near-dups down, because T2's criterion is whole-loop overlap > 0.6 against any bank member; the tighter threshold (c) buys the mean; `rescue_last` (d) clears the served surface of the rescue builder's loops (16 % → 2 %). P2's own filter tightened (v0b) buys a quarter of that for the same latency.
- **What is left.** The served **forward** legs are now as distinct as prod's (0.234 vs 0.242 of the loop); the **return** legs carry the whole residual (0.210 vs 0.151) — the returns are `route_leg` repairs built after selection that no selection-time test can see — and half of the remaining near-dups have their best sibling in slots 6–11, the ladder's fills that T2 charges to the served loops. Read served-vs-served (slots 0–5 against 0–5 only; §9 Q1, measured after the session) d is **0.3370 / 13.8 % — 1.063× / +2.2 pp vs prod**, inside the ratchet's shape; c 1.107×. The diversity term (b2) is inert.
- **What it costs.** The evaluation budget was the first latency regression (a0: 5 224 evaluations, 1.47×; v0: OOM) and is now bounded (one total per request, fresh evaluations only, memory-light rejects); with it back at P2's cost (d: 107 ms) the median is still 1.22× — the rest is building the distinct sinks' returns (more full repairs, 3.4 rescue builds per request against P2's 0.9).
- **Gurka 51 green** (P2e / P2f on the comb map); the branch carries the mechanism as ten default-off knobs.
- **Verdict for the ladder:** T2 cannot be met inside T3 on this mechanism as measured; the Pareto front is §8, the recommendation there. The keep / shelve call is Andrey's on the galleries (Appendix B).

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
| b2 | `p21b2` | **d** + `roundtrip_pair_diversity_w` 2.0 (run after bracket B, same session) | `p2-1-b2/` | the diversity term (ticket lever 2) |
| **d** | `p21d` | **c** (0.35) at `eval_total` 1 200 (`eval_cap` 1 200, `relax_eval_cap` 150) + `roundtrip_pair_rescue_last` (rescue-built loops rank behind pair-built ones within a tier) — binary `f4a50bdbb` | `p2-1-d/` | the decision run: the T3 side of the front |
| baseline bracket **B** | `p21baseB` | — | `census-v2-p21-baseB/` | latency bracket after the variants (T3 pools A + B) |

**Bracket A** (12:23–12:32Z, `census-v2-p21-baseA/`): wall p50 **1.165 s**, p95 3.337 s, fills 550/552, failures 0, engine-stage total 824.4 ms mean / 709.5 ms p50, attempts 12.56 — against the P2 session's prod-condition bracket X (1.189 s / 3.123 s, 860.7 / 757.0 ms). Its meters are **byte-identical to bracket X's** (`same_meters.py`: 0 of 6 621 loops differ — deterministic engine, same binary and config), so the switchback-aware detector read of X (`p21bx`) is the read of every prod-condition baseline bracket in this session.

**a0** (12:40–12:54Z): T2 **0.4829 / 21.0 %** (1.231× / +6.6 pp, FAIL — but −0.105 / −29 pp from P2's 0.5879 / 50.4 %; block A 0.4677 / 23.3 %, block B 0.4899 / 19.9 %; c0.5 0.4716 / 16.6 %, c0.7 0.5061 / 24.9 %, c1.0 0.4848 / 24.9 %); fills 552/552, failures 0; wall p50 **1.700 s** (1.46× bracket A), p95 **10.06 s**; engine-stage total 1 442 ms (eval 672 ms mean, p50 269, max 6 829); evaluations 5 224 per request (p50 3 461; `rung_evals` means 2 730 / 1 221 / 738 / 535), `leg_share` 224, whole-pair `share` **1 773** (P2: 130 — the built-loop keys see the repaired returns), `leg_relaxed` 8.48 per request (464 requests with any; highest rung 1 / 2 / 3 on 318 / 90 / 56), rescue loops 955 (P2: 477), `underfill=evalcap` on 438; container memory max 2.4 GiB. Per slot: relaxed share 59 % (slot 0) → 82 % (slot 11); pair-built 81–91 %; tier 0 95 % in slots 0–9.

**v0b** (12:54–13:05Z, `p2-1-v0b/`; the P2 mechanism, new knobs off, `roundtrip_sharing_frac` 0.4 inside `roundtrip_pair_eval_cap` 3 000 with P2's semantics — the filter is dropped past the cap, the walk goes on): T2 **0.5508 / 42.0 %** (1.404× / +27.6 pp, FAIL; block A 0.4832 / 30.2 %, block B 0.5815 / 47.4 %); fills 552/552, failures 0; wall p50 **1.567 s** (1.35× bracket A), p95 5.15 s; engine-stage total 1 165 ms (eval 338 ms mean, max 4 938); evaluations 3 700 per request (p50 3 057, **max 31 070** — with the filter dropped the walk is bounded only by the bank filling), whole-pair `share` rejects 803 per request, rescue loops 667 (70 requests); memory max 1.65 GiB. **P2's own filter, tightened to 0.4, buys 0.037 of the served mean and −8 pp of near-dups for a third more latency** — the pair-keyed whole-pair test is the wrong instrument, as P2 §5 said.

**a1** (13:07–13:18Z, `p2-1-a1/`; the leg test alone — whole-pair test off — total budget 2 000, relax rungs 300 fresh): T2 **0.4937 / 29.1 %** (1.259× / +14.7 pp, FAIL — *worse than a0 on both*; block A 0.4719 / 30.2 %, block B 0.5036 / 28.6 %); fills 552/552, failures 0; wall p50 **1.416 s** (1.22× bracket A), p95 5.69 s; engine-stage total 1 205 ms (eval 217 ms mean, p50 133, max 4 516); evaluations 1 867 per request (p50 2 001 = the whole budget, spent at rung 0 on the median request: `rung_evals` 1 867 / 0.14 / 0.11 / 0.03 — the relaxation rungs only re-test the cache), `leg_share` 782 per request, chosen 7.33, relaxed loops 2.89 (highest rung 1 / 2 / 3 on 139 / 173 / 116 requests), rescue loops 1.67 (156 requests), `underfill=evalcap` on 473; memory max 1.66 GiB. Per slot: relaxed share 0 / 0 / 0.4 / 0.9 / 2.9 / 6.7 % in slots 0–5 (the ladder's loops sit in slots 6–11), pair-built 83–85 %.

**What a1 says.** (i) The whole-pair-vs-built test was doing the T2 work in a0: T2's near-dup criterion *is* whole-loop overlap > 0.6 against **any** bank member, and with the ladder filling slots 6–11 with loops admitted at 0.65–0.80 of the forward leg, those deep loops are charged to the served ones — without a whole-pair test at 0.6 nothing stops a relaxed loop from being a 70 % copy of slot 1. (ii) Laddering the whole-pair threshold (the a2 as first defined) cannot buy latency, because the relaxation rungs already spend ~0 fresh evaluations under a total budget. So **a2 was redefined before it launched**: whole-pair test **on and fixed at 0.6**, the leg test laddered, total 2 000 — a0's configuration with the correct rung-0 semantics and a bounded budget. (The `engine` note in `p2-1-a2/report.json` still says "laddered"; the ledger's `share_relax=0` is the truth.)

**a2** (13:21–13:32Z, `p2-1-a2/`; leg 0.5 laddered, whole-pair test fixed at 0.6 against the built bank, total budget 2 000, relax rungs 300 fresh): T2 **0.4657 / 21.7 %** (1.187× / +7.3 pp, FAIL — the best of the session; block A **0.4135 / 19.0 %**, block B 0.4895 / 23.0 %; c0.5 0.4510 / 18.1 %, c0.7 0.4706 / 24.6 %, c1.0 0.4838 / 25.1 %; slots 0–2 0.4690 / 21.7 %); fills 552/552, failures 0; wall p50 **1.546 s** (1.33× bracket A), p95 4.71 s; engine-stage total 1 058 ms (eval 183 ms mean, p50 140, max 1 364); evaluations 1 880 per request (p50 2 002 — the budget, spent at rung 0), whole-pair `share` rejects 865, `leg_share` 183, chosen 7.08, built 9.39 pair-built + rescue loops **2.98** per request (301 requests with any; slots 0–5 only 66–75 % pair-built — the whole-pair test sends more slots to P1.1's rescue builder), relaxed loops 1.32 (none in slots 0–5); memory max 1.57 GiB.

**c** (13:36–13:46Z, `p2-1-c/`; a2 with the per-leg threshold at 0.35): T2 **0.4521 / 22.4 %** (1.152× / +8.0 pp, FAIL — the best mean of the session, 2.9 pp of near-dups over the bar; block A **0.3810 / 17.8 %**, block B 0.4844 / 24.5 %; c0.5 0.4376 / 20.1 %, c0.7 0.4541 / 22.3 %, c1.0 0.4716 / 25.7 %); fills 552/552, failures 0; wall p50 **1.417 s** (1.22× bracket A — cheaper than a2's 1.546), p95 4.36 s; engine-stage total 991 ms (eval 176 ms mean, max 836); evaluations 1 908 per request (the budget), `leg_share` 403, whole-pair `share` 893, chosen **5.61** (a2: 7.08 — the tighter threshold finds fewer strict sinks within the budget), pair-built 9.27 + rescue loops 3.10 per request (301 requests), relaxed loops 2.36 (highest rung 3 on 189 requests), slots 0–3 100 % strict and 67–71 % pair-built; memory max 1.66 GiB. The tighter threshold trades near-dup count (+0.7 pp vs a2) for served mean (−0.014) and buys latency back (fewer chosen, fewer builds).

**d** (13:51–14:01Z, `p2-1-d/`; c at `eval_total` 1 200 / `relax_eval_cap` 150 + `rescue_last`; the `engine` note in its `report.json` was written before the re-base on c and says "a2" — the config file and the ledger are the record): T2 **0.4443 / 24.3 %** (1.133× / +9.9 pp, FAIL — the best served *mean* of the session, 0.013 above the bar, with the most near-dups of the leg + whole-pair variants; block A 0.4212 / 28.0 %, block B 0.4549 / 22.7 %; c0.5 0.4538 / 27.0 %, c0.7 0.4524 / 23.6 %, c1.0 0.4258 / 21.0 %); fills 552/552, failures 0; wall p50 **1.407 s** (1.21× bracket A), p95 4.26 s; engine-stage total 1 000 ms (eval **112 ms** mean, max 680 — the budget cut took 64 ms off c's evaluation and the wall p50 moved 10 ms: the leg variants' latency floor is not the walk); evaluations 1 172 per request (the budget), `leg_share` 241, whole-pair `share` 564, chosen 5.24, pair-built 8.87 + rescue loops **3.43** per request (340 requests), relaxed loops 1.95; `rescue_last` moved the rescue loops off the served surface — slots 0–1 are 94 % pair-built (c: 67–70 %), and the relaxed loops that were behind them now show in slots 2–5 (4.7–10.9 %); memory max 1.56 GiB.

**Bracket B** (14:04–14:12Z, `census-v2-p21-baseB/`): wall p50 **1.139 s**, p95 3.174 s, fills 550/552, failures 0, engine-stage total 809 ms mean / 715.5 ms p50, attempts 12.56; meters byte-identical to bracket A (0 of 6 621 loops differ). The two brackets sit 2.3 % apart (1.165 / 1.139 s); T3 is read against their pooled p50 (§6).

**b2** (14:21–14:32Z, `p2-1-b2/`; d + the diversity term, `roundtrip_pair_diversity_w` 2.0 — survivors of a sector's shortlist ordered by `score / (1 + 2·s)`, `s` = the whole-pair twins-aware sharing with the pairs already chosen; the seed rotation over the ordered survivors unchanged): T2 **0.4448 / 24.6 %** (1.134× / +10.2 pp) against d's 0.4443 / 24.3 %; fills 552/552, failures 0, spikes 0; wall p50 **1.405 s** (1.22× pooled; d 1.407 s), p95 4.48 s; engine-stage 1 009 ms mean / 817.5 ms p50 (d 1 000 / 816); curviness 298.6 / 245.5 / 253.2 (d 298.6 / 245.8 / 253.8). **6 373 of 6 624 loops are byte-identical to d's** by (request, slot, length). The term is inert, as §1 predicted: it only re-orders the survivors of one sector, and the seed rotation picks among them regardless of the order; ledger `div_w=2.0`, `share` 562 / `leg_share` 239 per request (d 565 / 241). Ticket lever (2) is measured and closed.

### 3.1 Why the matrix changed mid-session

a0 (the first cut) put T2 within 1.6 pp of the near-dup bar and 0.05 off the mean bar — but at 1.46× wall p50, and the ledger said why: the per-rung budget multiplies (four rungs × 3 000 fresh evaluations on hard requests), and the dominant rejecter was not the leg test but P2's whole-pair 0.6 `pair_shares`, now keyed on built loops and never relaxed, so every rung was spent re-rejecting on it. The evaluation volume *is* the latency regression (P2: 113 ms of evaluation at 1 710 per request; a0: 672 ms at 5 224). So instead of b / c on the 3 000 budget: **one total budget per request (2 000, P2's own volume), the relaxation rungs re-testing the cached candidates first and evaluating at most 300 new ones each, and the whole-pair test either off (a1) or laddered with the leg (a2)** — a1 vs a2 tells whether the T2 gain comes from the leg test or from the whole-pair-vs-built test; then c (0.35) on the better of the two, b (diversity) if time allows. The coordinator's independent read of a0 reached the same diagnosis.

## 4. Gate v2 — all tiers, all runs

Read by `gate_v2_read.py` (served surface = blocks A + B, slots 0–5; R1 = D1 `am10_max_run_m ≥ 500` ∨ D4 `reuse_disc_m ≥ 500` ∨ D1L `am25_max_run_m ≥ 1500` on the switchback-aware detector output; the reader reproduces ADR-0041's R1 43.9 / 2.4 / 3.8 %, T6 and T1). Bars: T1 per level vs the census run `census-v2-b4f514d7f` (`curviness_geom_clean`, whole corpus); T3 vs the pooled brackets A + B (until B lands: bracket A 1.165 s); T4 on A + B vs prod's A + B p95 (bracket X 2.977 s, bracket A 3.337 s — the pooled figure is used when B lands); T5 reported on both surfaces (§4.1). Columns: census (`cen`), prod condition (`prodX`, = brackets A/B byte-for-byte), P2 + xcand (`p2x`, the same detector read for the first time), the P2.1 runs.

| tier | bar | cen | prod (X ≡ A ≡ B) | P2 + xcand | v0b | a0 | a1 | a2 | c | d |
|---|---|---|---|---|---|---|---|---|---|---|
| R1 Retrace family, served | ≤ 5 % | 43.9 | 43.8 | 2.8 | n/a | 3.0 ✓ | 2.4 ✓ | 1.9 ✓ | 2.2 ✓ | **2.0 ✓** |
| R2 `spike_ge_500m` | 0 | 0 | 0 | 0 | 0 ✓ | 0 ✓ | 0 ✓ | 0 ✓ | 0 ✓ | **0 ✓** |
| R3 dist err mean c0.5 / c0.7 / c1.0 | ≤ .22 / .32 / .32 | .169/.108/.134 | .139/.109/.134 | .134/.104/.109 | .136/.108/.108 ✓ | .155/.115/.119 ✓ | .142/.107/.138 ✓ | .164/.117/.130 ✓ | ✓ | ✓ |
| R4 fills 12/12 | 552 | 550 | 550 | 552 | 552 ✓ | 552 ✓ | 552 ✓ | 552 ✓ | 552 ✓ | **552 ✓** |
| R5 failures | 0 | 0 | 0 | 0 | 0 ✓ | 0 ✓ | 0 ✓ | 0 ✓ | 0 ✓ | **0 ✓** |
| T1 curviness c0.5 / c0.7 / c1.0 (all, × census) | ≥ 0.95× | 1 | 1.01/1.01/1.00 | 1.06/1.28/1.21 | 1.04/1.24/1.12 ✓ | 1.04/1.24/1.11 ✓ | 1.06/1.29/1.16 ✓ | 1.05/1.24/1.11 ✓ | 1.05/1.24/1.10 ✓ | **1.04/1.19/1.09 ✓** |
| **T2** served mean / near_dup > 0.6 | ≤ 0.4315 / ≤ 19.4 % | .4412/23.8 | .3923/14.4 | .5879/50.4 | .5508/42.0 ✗ | .4829/21.0 ✗ | .4937/29.1 ✗ | .4657/21.7 ✗ | .4521/22.4 ✗ | **.4443/24.3 ✗** (1.133× / +9.9 pp) |
| T3 wall p50 (× pooled A+B 1.153 s) | ≤ 1.10× | 1.305 | 1.189 (X) | 1.102 = 0.96× | 1.567 = 1.36× ✗ | 1.700 = 1.47× ✗ | 1.416 = 1.23× ✗ | 1.546 = 1.34× ✗ | 1.417 = 1.23× ✗ | **1.407 = 1.22× ✗** |
| T4 wall p95 A+B (× pooled 2.993 s) | ≤ 1.30× | 3.50 | 2.977 (X) | 3.559 = 1.19× | 4.418 = 1.48× ✗ | 7.723 = 2.58× ✗ | 3.339 = 1.12× ✓ | 3.834 = 1.28× ✓ | 3.419 = 1.14× ✓ | **3.421 = 1.14× ✓** |
| T5 near-mirror mean m, A+B served (bar 1 198 = 0.25 × 4 792) | ≤ 1 198 | 4 792 | 4 861 | 29 | n/a | 44 ✓ | 39 ✓ | 47 ✓ | 45 ✓ | **36 ✓** |
| T5 on the ADR surface, all blocks s0–2 (bar 865 = 0.25 × 3 462) | ≤ 865 | 3 462 | 3 359 | 41 | n/a | 51 ✓ | 43 ✓ | ✓ | ✓ | ✓ |
| T6 deep-bank Retrace family | ≤ 53 % | 53.0 | 51.1 | 2.7 | n/a | 1.6 ✓ | 3.5 ✓ | 4.2 ✓ | 3.6 ✓ | **2.6 ✓** |
| C1 `spike_ge_30m` · C2 `edge_reuse_geom` served | report | 0 · .021 | 0 · .021 | 0 · 0 | 0 · 0 | 0 · 0 | 0 · 0 | 0 · 0 | 0 · 0 | 0 · 0 |
| A4 D1b ≥ 500 m served | advisory | 55.1 | 56.7 | 3.1 | n/a | 4.7 | 4.4 | 5.2 | 5.2 | 4.3 |

**Every P2.1 variant holds R1–R5, T1, T4, T5 and T6; none holds T2, and none holds T3** — the leg variants' median sits at 1.22–1.34× the pooled brackets, the mechanism's latency floor (§6). The P2 + xcand latency row is the uncontended re-run of P2 §7.8 (`p2-v2-xcand-uncontended`); the contended `p2-v2-xcand` run (1.486 s / 5.459 s) is not a reading.

### 4.1 The T5 erratum

ADR-0041's Baseline v2 figure for T5 (3 462 m) does not reproduce on the served surface as the ADR defines it (blocks A + B, slots 0–5 → **4 792 m**; prod-condition bracket 4 861); it reproduces exactly on **all blocks, slots 0–2** (3 462; P2 reads 41 there against the ADR's 44 — the ADR's P2 figure was read before the xcand re-run). Both bars are reported; every P2-family run passes both by two orders of magnitude, so the erratum does not change a verdict — the v4 build task should pin the surface in `gate_v2.py`.

## 5. T2 anatomy

### 5.1 Which leg carries the residual (`leg_overlap_served.py`, A+B slots 0–5, best sibling by exemption-discounted overlap = the T2 meter)

| run | served mean | = forward + return | forward share of the forward leg (mean; > 0.5) | return share of the return leg (mean; > 0.5) |
|---|---|---|---|---|
| prod | 0.3923 | 0.2416 + 0.1507 | 0.477 ; 54.5 % | 0.295 ; 21.9 % |
| P2 + xcand | 0.5879 | 0.3423 + 0.2456 | 0.682 ; 81.9 % | 0.492 ; 51.9 % |
| a1 (leg only) | 0.4937 | 0.2751 + 0.2186 | 0.562 ; 59.0 % | 0.424 ; 42.6 % |
| a2 | 0.4657 | 0.2629 + 0.2028 | 0.540 ; 56.7 % | 0.394 ; 37.3 % |
| c (0.35) | 0.4521 | 0.2544 + 0.1977 | 0.526 ; 55.9 % | 0.382 ; 36.4 % |
| **d** | **0.4443** | **0.2341 + 0.2103** | **0.489 ; 49.5 %** | **0.401 ; 39.4 %** |

**The forward leg is fixed; the return leg is the residual.** On d the served forward legs are as distinct as prod's (0.234 vs 0.242 of the loop; 49.5 % share more than half the leg with a sibling against prod's 54.5 %). The return legs came down with them — from P2's 0.246 to 0.198–0.210 — which is the working hypothesis half-confirmed (sinks off the same trunk do share less of the way home), but they stop 0.05–0.06 above prod's 0.151, and that difference *is* the T2 gap (0.4443 − 0.3923 = 0.052). The reason is the one P2 §12 named: the whole-pair test judges the candidate's pair (tree path + Suurballe's second path) against the *built* bank, but the candidate's own return is repaired by `route_leg` after selection in ~92 % of loops, and nothing sees where that repair goes; the xcand penalty (0.2, cap 4) is the only pressure on it. Strict-vs-strict served pairs still read 25 % near-dup on d for exactly this reason (§5.3).

### 5.2 Per block, level, slot (`served_surface.py`, `t2_anatomy.py`)

| | prod | a2 | c | d |
|---|---|---|---|---|
| block A (Vračar) | 0.3738 / 12.5 % | 0.4135 / 19.0 % | **0.3810 / 17.8 %** | 0.4212 / 28.0 % |
| block B (demand cells) | 0.4007 / 15.3 % | 0.4895 / 23.0 % | 0.4844 / 24.5 % | **0.4549 / 22.7 %** |
| c0.5 / c0.7 / c1.0 | .404/16.8 · .386/12.6 · .379/12.2 | .451/18.1 · .471/24.6 · .484/25.1 | .438/20.1 · .454/22.3 · .472/25.7 | .454/27.0 · .452/23.6 · .426/21.0 |
| slots 0 … 5 (mean) | .448 .467 .404 .317 .335 .384 | .451 .511 .445 .467 .488 .433 | .489 .464 .450 .462 .438 .410 | .494 .451 .410 .433 .441 .438 |
| near-dups whose best sibling sits in slots 6–11 | 19.9 % | 32.9 % | 47.7 % | 48.2 % |

Block A is within reach on c (0.381 / 17.8 % against A's own prod 0.374 / 12.5 %); block B — the demand cells, 50–300 km asks in the mountains and the plain — is where the gap sits. **Half of c's and d's near-dups have their best sibling in the deep bank**: the ladder's fills (and the rescue builder's) are what T2 charges to the served loops — the meter reads slots 0–5 against all twelve.

### 5.3 By provenance (`t2_anatomy.py`, the ledger's `pair-leg` line joined to the response; 320 of 320 A+B requests matched)

| served loop vs its best sibling | a2 | c | d |
|---|---|---|---|
| strict vs strict | 0.456 / 20.8 % (n 1 125) | 0.438 / 22.7 % (882) | 0.442 / 25.2 % (1 192) |
| strict vs relaxed | 0.481 / 23.8 % (365) | 0.462 / 24.9 % (567) | 0.466 / 28.6 % (461) |
| strict vs rescue | 0.502 / 27.8 % (97) | 0.430 / 18.2 % (88) | 0.391 / 14.0 % (164) |
| rescue vs rescue | 0.505 / 24.4 % (156) | 0.527 / 27.4 % (190) | 0.497 / 5.0 % (40) |
| served loops that are rescue-built | 313 (16 %) | 342 (18 %) | **42 (2 %)** |

`rescue_last` (d) takes the rescue builder's loops off the served surface (313 → 42) and their pairwise near-dups with it (rescue vs rescue 27 % → 5 %), which is where d's served-mean gain over c comes from; what it cannot touch is strict-vs-strict and strict-vs-relaxed — the return-repair residual of §5.1 and the deep-bank charge of §5.2.

### 5.4 Served-slot cleanliness on d (`slot_clean.py`, switchback-aware D1b; slots 0–2 / 3–5 / all)

Near-mirror mechanism (F02) **0.3 / 0.0 / 0.1 %**; same-pavement (F01) 0.0 / 0.0 / 0.1 %; D1b ≥ 500 m 3.4 / 2.1 / 3.4 % (mean unseen 53 / 46 / 53 m); rings (F22) 16.2 / 13.3 / 14.3 % of loops (34.2 % carry ≥ 1 near-rejoin ring, 0.42 per loop — the residual P2 named); `clean` 13.6 / 11.9 / 12.0 %; fallback-like heavy reuse 1.5 / 4.3 / 4.5 %. The retrace family stays where P2 put it (R1 2.0 %); distinctness at selection did not buy it back.

## 6. Latency

Same-session brackets A (12:23Z, before the variants) and B (14:04Z, after): **1.165 s / 1.139 s** wall p50 (2.3 % apart), A+B-block p95 2.922 / 3.174 s; pooled over both (1 104 requests): **p50 1.153 s, p95 3.225 s, A+B p95 2.993 s**. Per-request wall from each response's `meta.latency_s` (`pool_lat.py`); engine stages from the ledger (`stage_total_p2.py`, means ms; the pair pass and the pair evaluation are included in the total).

| run | wall p50 | wall p95 | p95 A+B | **T3** | **T4** | engine total (p50) | attempts | astar | astar_fb | pairs | eval |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pooled brackets A+B | 1.153 s | 3.225 s | 2.993 s | 1.00× | 1.00× | 824 / 809 (710 / 716) | 12.56 | 531 / 519 | 168 / 164 | — | — |
| P2 + xcand, uncontended | 1.102 s | 4.053 s | 3.559 s | **0.956×** | 1.19× | 850 (637) | 13.06 | 387 | 41 | 185 | 102 |
| v0b | 1.567 s | 5.149 s | 4.418 s | 1.36× | 1.48× | 1 165 (911) | 13.00 | 433 | 51 | 202 | 331 |
| a0 | 1.700 s | 10.055 s | 7.723 s | 1.47× | 2.58× | 1 442 (953) | 12.84 | 424 | 53 | 187 | 639 |
| a1 | 1.416 s | 5.691 s | 3.339 s | 1.23× | 1.12× | 1 205 (889) | 12.14 | 516 | 75 | 231 | 212 |
| a2 | 1.546 s | 4.711 s | 3.834 s | 1.34× | 1.28× | 1 058 (887) | 11.76 | 471 | 71 | 197 | 177 |
| c | 1.417 s | 4.360 s | 3.419 s | 1.23× | 1.14× | 991 (829) | 11.62 | 441 | 61 | 183 | 170 |
| **d** | **1.407 s** | 4.256 s | 3.421 s | **1.22×** | **1.14×** | 1 000 (816) | 11.19 | 489 | 68 | 194 | 107 |

**Anatomy.** Against P2 + xcand (0.956×), the leg variants add 0.3 s at the median from three places: (1) the **evaluation walk** — 2 000 evaluations cost 170–212 ms against P2's 102 at 600; the budget cut to 1 200 (d) takes it to 107 ms, i.e. back to P2's level; (2) **the return builds** — `astar` 441–516 ms against P2's 387 and `astar_fb` 61–75 against 41: the forward-distinct sinks are the less-connected ones, so more returns need the full P1.1 repair and more go to the rescue builder (3.0–3.4 rescue loops per request against P2's 0.9; requests with a geometry-gate reject 186 in a2 against 73); (3) the pair pass itself is unchanged (183–231 ms). d shows the floor: with the evaluation cost back at P2's level the median is still 1.22× — **the remaining cost is the price of building the distinct sinks' returns, not of finding them.** The tail behaves: T4 passes for every leg variant (1.12–1.28×), and the a0 outlier (2.58×) was the unbounded per-rung budget — the per-request total removed it. Memory: 1.6–2.4 GiB peak per container across the variants (v0: OOM at 5.8 GiB).

## 7. What did not work

### 7.1 v0 — the uncapped whole-pair filter (config only, the P2 binary)

Variant v0 (`v8003-p21-v0.json`: the P2 binary `83837debc`, xcand 0.2, `roundtrip_pair_eval_cap` 1 000 000, `roundtrip_sharing_frac` 0.4) answered **15 requests** and was **OOM-killed** at the first 200 km Belgrade asks (`rt-p1-build` `OOMKilled=true`; the remaining 537 requests read status 0 in ~1 ms; the Docker VM's 5.77 GiB is shared by both containers, 3 workers). The last completed 200 km request's ledger (`~/.curvagen-scratch/p2/eng-p21v0.log`, request 17): `pair-select: considered=44906 no_pair=11977 band=15456 twin=1234 share=19689 chosen=9; built=8` on a harvest of 617 684 labels / 285 753 junctions. With the sharing test never dropped and a strict threshold, the sector shortlist filled only 9 sectors, and the backfill and the refill queue walked essentially the whole band — 44 906 pair evaluations for one request against P2's mean of 1 710 (p50 730) — every one cached as a full `PairEval` (two arc vectors, a `PairKeys` with an `unordered_map` of ~300 roads and a twin set): three concurrent 200 km requests blew the memory, and the latency would have been seconds per request as well. **The uncapped K × K filter is not a usable variant as-is**; `results/p2-1-v0/` is kept as evidence and not measured. The consequences are built into P2.1: the per-rung evaluation budget and the memory-light rejects of §1; v0 is re-run bounded on the new binary as **v0b** (`roundtrip_pair_eval_cap` 3 000, `roundtrip_sharing_frac` 0.4, new knobs off) — "what does P2's whole-pair filter buy inside a budget".

### 7.2 a0 — the starved rung 0 (the first cut of the budget check)

The first corpus run of variant a (`p2-1-a0/`, binary `6f396e85f`) had the per-rung evaluation budget checked at the top of the build loop. On requests where the selection walk (sector shortlist + backfill under the strict test) had already spent rung 0's 3 000 evaluations, the build loop broke before building even the chosen candidates — a typical ledger line reads `chosen=11 … rung_evals=3000/62/0/0 … leg_relaxed=12 leg_relax_rung=1`: eleven chosen, none built at rung 0, the whole bank built at rung 1 (threshold 0.65) with 62 fresh evaluations. `underfill=evalcap` on 112 of the first 197 requests. The fix (`a4591a321`) bounds fresh evaluations only. a0 is kept as evidence and read in §4 as "the ladder-only variant" (effective threshold 0.65–0.80 on those requests); it is not the primary.

_(a0's numbers: filled from `read-a0.txt`.)_

### 7.3 Not built: the leg test before the pair construct

Suggested for d: walk the tree path (the label chain) for the forward keys and reject a leg-sharing sink *before* constructing its pair. The a2 ledger says it would not pay here: the leg test rejects 183 sinks per request, the whole-pair-vs-built test 865 (which needs the full walk), and 77 % of evaluations are `no_pair` / `band` rejects that today pay only the O(1) existence check and the construct — a pre-walk would add a tile-lookup walk to those. The evaluation budget is the lever that buys evaluation back, and d takes it (1 200 total, 150 per relaxation rung).

## 8. Pareto front / recommendation for the v4 build

Served-surface T2 against wall p50 (× the pooled brackets A + B, 1.153 s; T4 × their A+B p95 2.993 s), every P2-family point at prod configuration, K = 12, fills 552/552, spikes 0:

| point | mechanism | evaluations / request | **T2** mean / near_dup > 0.6 | T3 p50 | T4 p95 A+B |
|---|---|---|---|---|---|
| P2 + xcand | pair pass, pair-keyed 0.6 filter (cap 600) | 1 710 | 0.5879 / 50.4 % | 0.96× | 1.19× |
| v0b | whole-pair 0.4, P2 semantics inside 3 000 | 3 700 | 0.5508 / 42.0 % | 1.36× | 1.48× |
| a1 | leg 0.5 laddered, no whole-pair test, total 2 000 | 1 867 | 0.4937 / 29.1 % | 1.23× | 1.12× |
| a0 | leg 0.5 (rung 0 starved) + whole-pair 0.6 vs built, 3 000 / rung | 5 224 | 0.4829 / 21.0 % | 1.47× | 2.58× |
| a2 | leg 0.5 laddered + whole-pair 0.6 vs built, total 2 000 | 1 880 | 0.4657 / **21.7 %** | 1.34× | 1.28× |
| c | a2 at 0.35 | 1 908 | **0.4521 / 22.4 %** | 1.23× | 1.14× |
| d | c at 1 200 total + rescue-built last | 1 172 | **0.4443** / 24.3 % | **1.22×** | 1.14× |
| b2 | d + diversity term 2.0 | 1 172 | 0.4448 / 24.6 % | 1.22× | 1.16× |
| **bars** | | | **≤ 0.4315 / ≤ 19.4 %** | **≤ 1.10×** | **≤ 1.30×** |

**Verdict.** Distinctness at selection moves T2 a long way — from 1.50× / +36 pp (P2) to 1.13× / +9.9 pp (d) or 1.15× / +8 pp (c) — and it does so with R1–R5, T1, T4, T5 and T6 intact, no spikes, full fills and a bounded memory footprint. It does not reach T2, and every point that gets close costs 1.22–1.34× at the median (T3 bar 1.10×; P2 itself 0.96×), a floor that the evaluation budget does not move (d: evaluation back at P2's cost, median still 1.22×). Two structural reasons, both visible in the ledgers: (1) the Served Surface is judged against the *whole* bank, so the relaxation ladder that keeps R4 at 552/552 fills slots 6–11 with loops that T2 then charges to slots 0–5 — the whole-pair test against the built bank (a2 vs a1) is what caps that, and it already sits at T2's own 0.6; (2) the forward-distinct sinks are the less-connected ones — more rescue builds (3.0 per request against P2's 0.9), more geometry-gate fires, and a selection walk that spends its whole budget finding 5.6–7 strict candidates per request — so part of the latency is the price of distinct sinks, not of the walk.

**Recommendation for the v4 build task:** keep the pair pass as the selection stage; carry the mechanism as *knobs* (`roundtrip_pair_leg_sharing` 0.35, built-loop keys, whole-pair 0.6 against the built bank, the relaxation ladder with relaxed-last ranking, one evaluation budget per request) — it is the cheapest lever the ladder has for T2 and it is measured; and take the T2 question back to the ADR with the front above: either the ratchet is read on the served loops *against the served surface* (slots 0–5 vs 0–5 — the rider never sees slot 9), or the budget bar for distinct banks is written in latency (1.2×), or the bank is thinned by refusing fills the way ADR-0040's item 4 did — none of which this prototype may decide.

## 9. Open questions

1. **What T2 measures.** `max_pair_overlap` is each served loop against all twelve; with a ladder for fills, slots 6–11 are by construction the loops that failed the strict test, and §5.2 measured that 48 % of c's and d's near-dups have their best sibling there (prod: 20 %). A served-vs-served read of the meter (slots 0–5 against 0–5) is a five-line change in `metrics.py` and was not computed here. ADR question.
   **Measured after the session (14:40Z, `~/.curvagen-scratch/p21/served_vs_served.py` — each served loop against the other served loops of its bank only, slots 0–5 vs 0–5, the same exemption-discounted overlap):**

   | run | T2 as written (vs the whole bank) | served-vs-served (0–5 vs 0–5) | vs prod on that read | near-dups whose best sibling is in slots 6–11 |
   |---|---|---|---|---|
   | prod (X) | 0.3923 / 14.4 % | **0.3171 / 11.6 %** | — | 20 % |
   | P2 + xcand | 0.5879 / 50.4 % | 0.5524 / 45.5 % | 1.74× / +33.9 pp | 17 % |
   | c | 0.4521 / 22.4 % | 0.3512 / 12.2 % | 1.107× / +0.6 pp | 48 % |
   | **d** | 0.4443 / 24.3 % | **0.3370 / 13.8 %** | **1.063× / +2.2 pp** | 48 % |
   | b2 | 0.4448 / 24.6 % | 0.3383 / 13.8 % | 1.067× / +2.2 pp | 49 % |

   Among the loops the rider is served, d sits inside the ratchet's shape (≤ 1.10× and ≤ +5 pp) and c on its edge; P2 fails on either read; the T3 miss stands on both. Which read Gate v2 means is the ADR's call, not this prototype's — the front is the input.
2. **The return repair is the residual** (§5.1): the served returns are built after selection and the selection-time tests judge Suurballe's second path instead. The levers are P1.1's: a stronger xcand surcharge on the repairs for the pair mode, or the ADR-0040 built-loop filter applied to repairs only — both measured expensive before, neither measured on top of P2.1.
3. **The whole-pair test against the built bank rejects 865–893 candidates per request** and is what sends 3 slots per request to the rescue builder. A test that keys on the *forward leg + the pair's return* but tolerates the repaired return's sharing (the xcand penalty already pushes repairs off shared corridors) might keep the pair-built share up; unmeasured.
4. **Budget vs threshold.** Only two budgets (2 000; d's 1 200) and two thresholds (0.5, 0.35) were run at the correct semantics; the front is coarse. The evaluation cost per candidate (~95–130 µs, tile lookups in the arc walk) is the constant to attack if the mechanism is kept — caching canonical ids on the pass's arcs (built once per request) would remove the tile lookups from the walk.
5. **Rescue-built loops on the served surface** (16–18 % of served loops in a2 / c): `rescue_last` (d) removes them from it (2 %) and their pairwise near-dups with them; it cannot touch strict-vs-strict.
6. **Gurka cannot read the ledger** (`logging::GetLogger` is a one-shot static): P2b's "std_err for the record" never printed; the rt-debug mirror is the workaround. Worth a small fix in the harness before more ledger-driven pins are written.

## Appendix A — commands

```bash
# the engine patch (reproducible, anchor-asserted) and the build in the warm audit container (-j3: -j8 OOM-kills the compiler in a 5.8 GiB VM)
python3 ~/.curvagen-scratch/p21/patch_p21.py                       # the first commit's edits on 4402a2604
docker cp src/thor/route_action.cc rt-p1-build:/src/valhalla/src/thor/   # etc.; then
docker exec rt-p1-build bash -lc 'cd /src/valhalla/build && make -j3 valhalla_service gurka_roundtrip_audit'
docker exec rt-p1-build bash -lc 'cd /src/valhalla/build && ./test/gurka/gurka_roundtrip_audit --gtest_brief=1'   # + gurka_motorcycle_roundtrip, gurka_roundtrip_distinctness

# engine configs, layered inside rt-p1-build on /tmp/v8003-p2v2-xcand.json (P2 knee + pair pass + xcand 0.2 cap 4)
ls /tmp/v8003-p21-*.json      # v0, v0b, a (= a0), a1, a2, c-on-a2, d; baseline /tmp/v8004-xcand.json in rt-p11-base

# the runs: one engine at a time, 3 workers, way pass on, an engine watchdog + memory log per run
~/.curvagen-scratch/p21/p21-run.sh <base|p2> <tag> <cfg> <outdir> "<note>"          # wraps p2_lib.sh's run_base / run_p2
~/.curvagen-scratch/p21/p21-next.sh <prev-log> <PREV> <var> <tag> <kind> <tag> <cfg> <outdir> "<note>"   # hand-off: previous variant's detector pass in the gap, then the next run
~/.curvagen-scratch/p21/p21-rebuild-then-{a,a1,a2,d}.sh                              # wait for a marker, rebuild, gurka smoke, launch

# readings
python3 ~/.curvagen-scratch/p21/served_surface.py prod=results/census-v2-p2-baseX p2x=results/p2-v2-xcand c=results/p2-1-c ...   # T2
python3 ~/.curvagen-scratch/p21/gate_v2_read.py cen=results/census-v2-b4f514d7f:p2cen prodX=results/census-v2-p2-baseX:p21bx c=results/p2-1-c:p21c ...
~/.curvagen-scratch/p2/p2-analyse-all.sh p2-1-c p21c p2-v2-xcand p2v2x census-v2-p2-baseX p21bx   # switchback-aware detectors
python3 ~/.curvagen-scratch/p21/p21_ledger.py ~/.curvagen-scratch/p2/eng-p21c.log     # the appended fields, per-rung evaluations, memory guard, per-slot provenance
python3 ~/.curvagen-scratch/p2/{p2_ledger,ledger_agg,stage_total_p2}.py ...           # the P2 parsers, unchanged, still run
python3 ~/.curvagen-scratch/p21/leg_overlap_served.py ... ; python3 ~/.curvagen-scratch/p21/t2_anatomy.py c=results/p2-1-c:~/.curvagen-scratch/p2/eng-p21c.log
~/.curvagen-scratch/p21/p21-read.sh <var> <tag>   # light bundle;  ~/.curvagen-scratch/p21/p21-final.sh <best> <tag>   # the end-of-session bundle incl. galleries
```

## Appendix B — artefacts

| Path | What |
|---|---|
| `tools/loopqual/results/p2-1-{v0,v0b,a0,a1,a2,c,d,b2}/` | the runs (552 responses each except v0; `loops.jsonl`, `report.{json,md}`); `p2-1-<best>/compare/`, `gallery-p2-1-*.html` |
| `tools/loopqual/results/census-v2-p21-base{A,B}/` | the session's baseline brackets (meters byte-identical to `census-v2-p2-baseX`) |
| `~/.curvagen-scratch/p2/eng-p21{baseA,baseB,v0,v0b,a0,a1,a2,c,d,b2}.log` | engine ledgers (`pair-select` with the appended fields, `pair-leg`, the P1.1 lines) |
| `~/.curvagen-scratch/p21/` | drivers (`p21-run.sh`, `p21-next.sh`, `p21-rebuild-then-*.sh`), per-run logs + memory watch (`watch-*.log`), readings (`read-*.txt`, `gate-*.txt`, `final.txt`), the patch script, the readers (`served_surface.py`, `gate_v2_read.py`, `leg_overlap_served.py`, `t2_anatomy.py`, `p21_ledger.py`, `same_meters.py`, `p21_gallery.py`), the ticket comments |
| `~/.curvagen-scratch/{p21a0,p21a1,p21a2,p21c,p21d,p2v2x,p21bx}*.jsonl` | detector output (switchback-aware D1b) for the P2.1 runs and, for the first time, the prod-condition P2 and baseline runs |
| `~/.curvagen-scratch/p21/gurka-p21-audit.log`, `gurka-p21-others.log`, `build-p21-*.log` | the 51-green run and the builds |
| `~/.curvagen-scratch/p21/served_vs_served.py`, `served_surface.py`, `gate_v2_read.py`, `leg_overlap_served.py` | the T2 readers: served-vs-served (§9 Q1), the pinned served-surface read, the Gate v2 table, the per-leg split |

Containers `rt-p1-build` (:8003) and `rt-p11-base` (:8004) are left running with their engines stopped; `valhalla-local` (:8002) and the :8791 results server were never addressed.

## Appendix C — the branch

`proto/v4-p2.1`, cut from `proto/v4-p2` @ `4402a2604`; **nothing pushed**.

| commit | what |
|---|---|
| `6f396e85f` | the mechanism: per-leg forward-sharing threshold, built-loop bank keys, diversity term, relaxation ladder + relaxed-last ranking, per-rung evaluation budget (first cut), memory-light rejects, ledger fields, gurka P2e / P2f (comb map) |
| `a4591a321` | the budget bounds fresh evaluations only (a0's starved rung 0) |
| `53dbd1474` | `roundtrip_pair_eval_total` — one budget per request |
| `d3126916c` + `23ae41e64` | `roundtrip_pair_relax_eval_cap`, `roundtrip_pair_share_relax` (declared, then wired) |
| `f4a50bdbb` | `roundtrip_pair_rescue_last` (decision run d) |
| docs commits | this document, written incrementally (`206149156` … ) |

Knobs added (all `thor.`, all default off / 0): `roundtrip_pair_leg_sharing` (false), `roundtrip_pair_leg_sharing_frac` (0.5), `roundtrip_pair_built_keys` (false), `roundtrip_pair_diversity_w` (0), `roundtrip_pair_leg_relax` (false), `roundtrip_pair_relaxed_last` (false), `roundtrip_pair_eval_total` (0), `roundtrip_pair_relax_eval_cap` (0), `roundtrip_pair_share_relax` (false), `roundtrip_pair_rescue_last` (false); `roundtrip_pair_eval_cap` keeps its P2 meaning unless `roundtrip_pair_leg_sharing` is on. The diversity term (`roundtrip_pair_diversity_w`) is implemented and gurka-covered by construction (w = 0 is P2's order) but **was not measured** on the corpus — variant b was dropped from the matrix for time once the budget axis turned out to matter more. `clang-format` (host 23.1) was applied to the changed hunks only (`git clang-format`); whole-file formatting would have re-flowed ~700 unrelated lines.

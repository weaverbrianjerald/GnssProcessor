# Decomposition addendum — 2026-07-16 (freeze SHA `56da23a`)

**Status:** ACTIVE — this is the working reference for the increment series. Amends
`network_decomposition_plan.md` (Jul-4 design, still authoritative for philosophy/mechanics) and
`network_decomposition_resurvey_2026-07-13.md` (whose line anchors are now stale). Produced from a
10-agent survey workflow at HEAD `56da23a` (clean tree, Release build clean,
`gnss_tests` **780/779 pass/1 skip**).

---

## 1. Freeze state

| Item | Value |
|---|---|
| **Freeze SHA** | **`56da23a`** ("Missed a config for WL test", atop `89fe588` "Network enhancements, WLs, parallelizations.") — the MW-WL/UWL workstream (WL-1/WL-2/WL-3/UWL-1), all lineage configs, handoffs, and specs are IN the baseline. |
| `ApplicationNetwork.cpp` | **14,657 lines** (`wc -l`; was 11,739 at the Jul-13 resurvey — +2,918, all WL/UWL + H1-H3 hardening). `runNetwork()` spans 610→~14,655. |
| `GnssConfigLoad.cpp` | **6,000** (was 4,648 Jul-4). `GnssConfig.hpp` 4,705; `GnssConfigTypes.hpp` 2,340. |
| Gates 1–4 (resurvey §6) | All CLOSED: C1/S1/pdp committed+validated; Card 7/8 triage = **after INC-12** (recorded); v3 robust-M + per-LOS iono **stay holstered** for the series. |
| Gate 5 (TU freeze) | In force from this commit forward. Urgent fixes land only at increment boundaries. |
| `carrycpljoint` fingerprint fix | CONFIRMED in baseline at 3773-3774. |

## 2. ⚠️ Series-wide test policy (USER DIRECTIVE, Jul-16)

**Every in-series gate runs the FAST matrix only: 50 stations, ≤10 epochs, GPS-only where the
toggle allows, `network.parallel_carry=true` / `parallel_reduce=false`.** The heavy battery
(1h QC-tripping run, 305-sta arms, 3h crash-signature recheck, heavy resume pairs) is **deferred to
a single post-INC-12 validation phase**. Consequences:

- The Jul-4 cadence table ("1h QC at INC 4/6/7/9/10/12") is replaced by: fast matrix at EVERY
  increment + fast resume-pair (ARM 8 snapshot leg) at milestones 4, 7, 9b, 12.
- The 3h crash-signature re-pin moves from INC-0 to the post-series phase (signature compared
  pre/post series at that point; the crash stays out of scope).
- QC-path coverage in-series comes from row-count assertions on the QC traces the fast arms DO trip
  (harness fails on golden-had-rows/candidate-has-none), not from 1h runs.
- Rationale: token/wall-clock budget; the byte gate catches code-motion defects at any epoch count —
  long runs only add taken-branch coverage, which the post-series phase restores.

## 3. Fast golden matrix (INC-0 capture, this SHA)

Harness: `scripts/compare_network_run.py` (capture / compare / selfcheck / dump-maskspec; exit
0=pass 1=diff 2=usage; fails on missing/extra files and vacuous captures). Each arm: run TWICE →
`selfcheck` (mask discovery / determinism proof) → `capture` → goldens+masks READ-ONLY.
Parallel arms never use `--include-logs` (shared-logger line reorder); `run.log` content-compare +
`wall.txt` verbatim still apply. Per arm, grep the run log once for the loader's
pcarry/preduce-mutex WARN (must be absent).

Configs are harness-owned copies `config/test_suite/_dcmp_*.json` (the `_wlver_*` lineage stays
`pcarry=false` per the WL-card directive — do NOT mutate it):

| Arm | Config (all: 50 sta, ≤10 ep, pcarry=true/preduce=false) | Covers |
|---|---|---|
| 1 `alloff` | NEW — from `_wlver_50sta_10ep_tripleoff.json` with the full carry stack forced OFF (carry_invariant/innov_gate/calibrate/honesty/carry_vce/full_cov/coupling/coupling_TU/eq34 all false; mw_wl+lane_carry blocks removed; **carry_invariant_emit_rebase=false explicitly** — loader defaults it TRUE) | The default-OFF byte-identity contract (no 50-sta all-OFF config existed) |
| 2 `allon` | copy of `_neqexp_50sta_10ep.json` | carry_invariant + D5 gate/calibrate + eq34 + carry_vce + emit_rebase(default-true) + honesty + full_cov + coupling + coupling_TU + neq_export + emit_pb_filter + iono-deweight, GPS-only |
| 3 `jw` | copy of arm 2 + `carry_coupling_joint_whiten=true` | the lone unarmed Card-1 knob (only other jw config is 50 ep) |
| 4 `wlinj` | copy of `_wlver_50sta_10ep_wlinj.json` | WL-2 EWL cascade + WL inject, GPS-only |
| 5 `lane` | copy of `_wlver_50sta_10ep_lane.json` | WL-3 lane-coordinate carry, GPS-only |
| 6 `uwl` | copy of `_wlver_50sta_10ep_uwl_dualfam_mid.json` | UWL-1 all five sub-toggles (uwl+mid+dual_top+family_pairs+cold_fix)+ewl+lane, GPS+Gal |
| 7 `consumer` | copy of `_net50_allon_n4_em25_smoke.json` | closed-loop consumer + AR + product file (G-line mask path) + W6 sat-code-bias + decimation + serial-seam variant (~5 net epochs) |
| 8 `snap` | copy of arm 2 + `network.state_snapshot_path` armed | snapshot writer (full-precision artifact) + **fast resume-pair protocol**: leg A fixed 10 ep writes final snapshot → leg B resumes from it; both legs captured |
| 9 `snap_resume` | NEW at INC-4 — copy of arm 8 + `network.start_offset_hours = 0.08333333333333333` (exactly 300 s in IEEE754) | the resume-pair leg B: seeds the GOLDEN arm-8 snapshot (`--seed` in run_decomp_arm.py) at `logs/network_state_snapshot.json`, fast-forwards past leg A's span, WARM-resumes (age 30 s vs 900 s horizon, 31 D2 carries + emitter state restored) and processes epochs 11-20.  Baseline captured from the pre-INC-4 binary (byte-identity chain to INC-0); the warm gate's `first_live_s` honours start_offset, and neither start_offset_hours nor max_duration is fingerprinted, so the changed config still matches. |
| 10 `obs` | NEW at INC-4 — copy of arm 2 + `write_run_manifest` + `health_interval_s=60` + `trace_flag_rates` + `trace_epoch_status` + `trace_events` + `trace_rotate_mb=0.005` | the INC-4-moved observability writers ON-path (the original 8 arms leave ALL of them OFF): L15 manifest startup+complete records, RTB-6 heartbeat, L1 status rows, L2 event rows, B8 flag-rate + L3 run-summary CSVs.  P1 rotation reaches only the guard path (10-epoch files never hit the roll threshold).  Baseline captured from a pre-INC-4 build via git-stash round-trip. |

**Enumerated artifact deltas** (a golden arm may gain a file only when it is listed here, with the
card that added it and the reason the delta is confined to that file):

| Arm | Added artifact | Card | Why the rest stays byte-identical |
|---|---|---|---|
| 11 `estim` | `logs/network_estimability.csv` | BD-DATUM **DG5** (2026-07-22) | The spectrum dump's WARN text is unchanged — the CSV is FILLED FROM the same locals, in the same loops, in the same order (`NetworkEstimator::dumpEstimabilitySpectrum` returns an `EstimabilityDump` the driver writes).  The file appears only under `network.trace_estimability`; every OFF arm is untouched.  The DG6 run-wide gate telemetry rides in this file's trailing `RUN` record rather than in a new run-end log line, precisely so the arm's `run.log` stays comparable. |

Golden run dirs: `runs/decomp_goldens/<arm>[_b]/` (gitignored, machine-pinned); capture manifests
COMMITTED at `goldens/decomp/<arm>.manifest.json` so every increment can gate via
`compare --manifest` even if run dirs are lost. Runs use a renamed exe copy
(`gnss_processor_dcmp.exe`) per the build-lock directive; pre-kill WER zombies.

## 4. Anchor re-grep at `56da23a` (supersedes resurvey §1 lines)

Drift is non-uniform (+5 early → +3,619 late); **never reuse resurvey line numbers**. Definition
sites, current tree:

| Unit (dest TU) | Now | Was |
|---|---|---|
| NetStation / StationAssembly / whitenCouplingRows / robustProminence / medianPolish2Way (anon-ns, INC-2) | 132-183 / 316-365 / 378-425 / 513-526 / 543-590 | +5..+6 |
| runNetwork() start | 610 | 605 |
| Persistent cross-epoch decls | 2021-2272 (prev_carry 2021, clock_osc 2148, local_carry 2229, zwd_carry 2232, rpb_carry 2243) — continues interleaved to 3956 (prev_tow/iod/gen 2553-2559, **WL/UWL state 2677-3017**, sh_dev_carry 3280) | 1870-2213 |
| Knob const-snapshots | 2273-2365 AND 2633-2758 (two regions, L15 manifest writer 2367-2552 between; counters 2553-2615; FlagRate 2605) | 2362-2525 |
| splitStation / appendGim / appendSh | 3055-3175 / 3190-3248 / 3313-3451 | 2536 / 2671 / 2890 |
| Fingerprint build / restore / epoch-loop head | 3649-3776 (cpljoint 3773) / 3777-3947 / **3957** | 2982 / 3091 / 3264 |
| D5 pending seed / refloat_signal / refloat_pair / PASS-A head / if_phase_td driver | 4186-4189 / 4421-4439 / 4524-4536 / 4280 / 4515-4670 (SB_SLIP 4244-4672) | 3491 / 3720 / 3823 / 3584 |
| eq34 election (8b) | **4919-5556** (4-way chain: legacy 4797 / ensemble 4904 / eq34 4919 / default 5557-5606; +D3c RPB alloc sibling 5619-5636; shared_phase_keys decl 4231) | 4216-4903 |
| do_fold pins (INC-7, foldAmb 5908 / foldRpb 5924) | 5900-6019 | 5195-5314 |
| PendCplRow / TuEnt / buildCplRow (7b) | 6109 / 6170 / 6217-6352 (+tuReinitSince 6212) | 5404-5647 |
| buildEnsembleGauges / **buildCarryDdPriors** / beginEpoch+invoke | 6738-6793 / **6823-8199 (65→1,377 lines — see §5)** / 8201-8212 | 6032 / 6113-6177 / 6178 |
| W6 code-bias datum (Sbasis) | 8215-8520 (~306 L, size unchanged) | 6189-6494 |
| healNearNullPhaseBias / Card-6 relevel | 8752 / 8964-9054 | 6726 / 6935 |
| solve / assess / resolve_dropping / **installPosterior span** | 9360 / 9453 / 9533 / **9914-10271 (52→358 lines — see §5)** | 7306 / 7417 / 7497 / 7877-7929 |
| AR Phase-2 + NEQ export (SB_NETAR 10272-10544) | 10274-10452 + 10454-10542 | 7931-8081 |
| Emit head (SB_SOLPREP) / rebase+PB-filter (SB_EMITSTORE) / consumer iono | 10547-11284 / 11286-11433 / 11527-11630 | 8083 / 8801 / 8995 |
| dd_frac / vtec_sh_health(+H1 gate+VCE readout) / carry_vce beginEpoch / regcol+harvestCoupling / VCE addRow-1 | 11839 / 12152 (readout 12314-12315) / 12342 / 12363-12443 / 12499 | 9299 / 9602 / 9771 / 9790 / 9926 |
| post-fit RAIM (SB_RAIM) | 12668-13105 | 10067-10529 |
| osc block / emit_clock_rate C1 / clock monitor | 13108-13266 / 13268-13305 / 13307 | 10531 / — / 10691 |
| D2 stash (SB_STASH_STATUS 13465) / VCE addRow-2 / snapshot write | 13490-13708 / 13704 / 13826-13924 (wall_utc 13851) | 10918 / 11086 / 11192 |
| epoch loop closes / teardown / VCE estimate | 13925 / 13927-14657 (WL/UWL telemetry 14107-14245) / 14350-14374 | — / 11286 / 11442 |

## 5. WL/UWL delta map (what `89fe588` adds to the seams)

Six zones; destination TUs per the existing seam map:

1. **Station setup 1132-1169** → SlipQc (INC 6): MW-detector build-gate relaxation (built whenever
   any WL lane armed), `setBridgeEnabled()`, `setLaneTiers()`. Plus mw_detect carve-out 4299-4318
   (mw_wl-only ⇒ Welford-only, NO carry refloat) and the T0_mw_detect emit 4448-4457.
2. **Run-state block 2677-3017** → RunState/RunTypes (INC 2/4): flags (mw_wl_on 2697, ewl_on 2744,
   uwl_* 2749-2762, wl_inject_on 2815, lane_* 2818-2820), `mw_wl_layer` 2735, `EwlLane`+`ewl_lanes`
   2779-2807 (**preferred rung BEFORE fallback — order load-bearing**), `WlInjectPlan`/`wl_inject_epoch`
   2826-2835, `LaneTraceEntry`/`lane_trace_epoch` 2849-2857, `WlCounters`+`wlAccumulate` 2862-2935
   (in-source bit-rot warning: keep together), F6 types+`mw_wl_f6` 2952-2975, d5 tag sets 2858-2861,
   run-latched sets 3004-3017.
3. **Inside buildCarryDdPriors 6812-8199** → 9b: carry_arc_amb_key2 6812 (+joint clear 6827), hoisted
   machinery 6829-6929 (revmap/lin_of/iono_of/band_top/top2/mid/lane_pref_pair/lam cache), census
   6930-7043, family admission 7044-7095, pair routing 7198-7292 (`carry_driver.buildDdPriors` core
   call **7296**), WL-1 lane 7309-7428, WL-2/UWL EWL lanes 7430-7551, WL-3 MW increments 7562-7640,
   bridge→WL walk 7642-8137 (mid hop 7934, sibling closure 7987, inject stash 8128), lane-trace build
   8139-8192, **block fold order wl_out → ewl_out (gated ewl_fold_rows) → wl_hold_out 8193-8196
   (installPosterior index contract)**.
4. **Post-install 9935-10269** → 9b + Traces: counter folds 9938-9949/10060-10079, F6 CSV 9950-10058,
   inject apply 10087-10137, lane-trace CSV 10139-10176, D5 walk band-2 funnel 10213-10222, d5 dump
   10225-10265, **tagset rotation prev.swap(now) 10266-10269 (cross-epoch)**.
5. **Run-end telemetry 14107-14245** → Traces (INC 3): five summary blocks reading zone-2 state.
6. **Manifest fields 2518-2521** → Traces.

### New run-state entities (extends resurvey §4; cold-start carries rule applies to ALL)

| Entity (decl) | Tier | Hazard |
|---|---|---|
| **`lane_admit_pairs` (3016)** | 1, run-latched, **BEHAVIOR-BEARING** | Highest-risk entity in the delta: losing it mid-run de-admits family pairs → pair arcs silently fall back to per-band groups → refloat/regroup churn. The latch exists to prevent membership flicker. |
| `d5_inject_chords_prev`/`_now` (2860) | 1/2, **cross-epoch** via swap 10266 | Loss zeroes `injected_prev` d5-dump column — grading corruption, no crash. |
| `mw_wl_f6` (2975) | 1, one-shot latch | `.written` loss re-fires the F6 dump; `.pending` loss between build/install drops it silently. |
| `lane_trace_header_written` (2857), `d5_dump_header_written` (2861) | 1, run-latched | Loss ⇒ duplicate header mid-CSV (breaks `grade_lane_trace.py`). |
| `uwl_warned_cons` (3008), `lane_census_logged` (3010), `lane_family_logged` (3017) | 1, run-latched | Log-only (dup lines). |
| `wl_epoch`/`wl_cons_epoch` (2898/2902), `wl_inject_epoch` (2835), `mw_wl_epoch` (2944), `lane_trace_epoch` (2856), `d5_hold_chords` (2859), `d5_ewl_stasat` (2858), `carry_arc_amb_key2` (6812) | 2 | Reset inside the build lambda, consumed post-installPosterior — **must survive the build→install seam** (last-build-wins is RAIM-re-solve safe). |
| `ewl_lanes[].run` (2785), `wl_run`/`wl_cons_run`, `mw_wl_run`, `lane_pair_arcs`/`lane_resid_arcs`/`lane_pair_tree_mismatch` (2937-2939) | 1 | Folded at ACCEPTED install only; loss silently zeroes run-end telemetry (invariant #4). |
| `walk_decided`/`lane_incr_done` (7660), `ewl_fixed`/`_top` (7320/7323), `wl_out`/`ewl_out`/`wl_hold_out` (7319), hoisted maps (6849-6929) | build-local | Correctly constructed-fresh — do NOT hoist (carry_arc_amb_key precedent). |

### ⚠️ Known gap recorded (POST-series card, not fixed mid-series)

**No WL/UWL/lane fingerprint terms exist** (fingerprint block 3649-3776 read in full). WL-2/WL-3
carry solve-determining cross-epoch state that is neither fingerprinted nor snapshotted, so a warm
resume across a WL/lane toggle flip is ungated — the exact gap class the `carrycpljoint` Jul-13 fix
closed. All WL toggles are default-OFF research cards, and fixing it now would change bytes for the
armed arms 4-6, so it is deliberately deferred; queue with the WL card follow-ups.

## 6. Increment-plan amendments (vs resurvey §3)

- **INC-1 (config split) — VERIFIED READY.** One uninterrupted span `GnssConfigLoad.cpp` **2825-5897**
  (3,073 lines: anon-ns parseCoordEpochYear 2825-2862 [sole caller 3018, inside the move], banner
  2864-2866, loadNetworkSection_ 2867-5745, applyNetworkConsumerCouplings 5747-5819,
  deriveClosedLoopConsumerConfig 5821-5854, applyNetworkConsumerProfile 5856-5897) between
  loadBaselineSection_ (ends 2823) and loadLoggingSection_ (starts 5902). Decls at GnssConfig.hpp
  126/158/188/4389; only cross-unit call is the header-inline `networkConsumerArIgnoreMwWl()`.
  Cross-section forcing writes: **13 sites / 10 distinct fields** (supersedes "eight") — all inside
  loadNetworkSection_, move for free. estimation-section peek at `network.estimate_glonass` is
  line 1026, stays. New-TU includes: GnssConfig.hpp, GnssConfigJsonHelpers.hpp (using jval/jsection
  only — NOT toLower/joinStrings), Constants.hpp, NetworkProductEmitter.hpp, StationInfoFile.hpp,
  nlohmann/json, fmt, spdlog, `<optional> <cstdio> <algorithm> <cctype>`. Old-TU include cleanup
  deferred to INC-12 (keep INC-1 pure-motion).
- **INC-2 (type hoist)** — hoist set enlarged: + CarryPrior/sharedCarryKey/SharedCarryCov/
  CoupledShared/kCouplingSparsifyRelThresh/CplRowRaw/LocalCarry/PlAmb/PlZwd/ClockOsc(+Pred+predict)/
  MedianPolish/medianCopy/ifElevMap/parseIfPairResetKey (anon-ns, 126-608) and the WL PODs from
  zone 2 where separable. Order dependency: CoupledShared/CplRowRaw before LocalCarry/
  StationAssembly/whitenCouplingRows. `static` helpers become `inline` in headers (linkage change
  only — ODR-safe, but keep them header-inline, not TU-exported). robustProminence is a template ⇒
  header-only. NOT plain PODs (hoist with care or defer to INC-4): EwlLane (owns NetworkMwWlLayer),
  F6Capture (declared with instance), WlCounters (+wlAccumulate pair), CarryEigenScope/SubBlockTimer
  (RAII).
- **INC-3 (traces)** — the L15 manifest writer (2367-2552) sits mid-knob-region and echoes ~40 knob
  locals; dd_frac/vtec_sh_health decl+open sites are 1656-1888 with write sites at 11839/12152.
  Adds from the delta: 3 CSV writers (F6/lane-trace/d5-dump), 5 run-end WL summary blocks, manifest
  fields. carry_vce has TWO readout sites (12314-12315 per-epoch + 14350-14374 run-end) — both to
  whichever unit owns NetworkCarryVce.
- **INC-9b is now a MILESTONE, not glue.** Was ~200 lines; now ~1,700 (zone 3 + zone 4 counters/
  inject). It owns the three most dangerous channels (carry_innov_pending, carry_slipped_ambs,
  lane_admit_pairs). Still fable-only, still before INC-9 (RAIM re-solve re-invokes
  buildEnsembleGauges 9651 + buildCarryDdPriors 9655 — capture semantics change at BOTH call sites).
  Sub-split option (decide at 9b time): 9b-i eq34 group build + driver core; 9b-ii WL/UWL lane
  machinery as `filter/NetworkMwWlGlue` (zone 3 is cleanly `if (mw_mach_on)`-gated).
- **SB_* sub-timer brackets are seam constraints** (new): SB_SLIP 4244-4672 brackets geometry+slip;
  SB_NETAR 10272-10544 brackets AR diag+NEQ export; SB_EMITSTORE 11285-11704 brackets rebase+PB-filter
  +closed-loop consume; SB_STASH_STATUS opens 13465 before the D2 stash and the VCE
  shared-prior snapshot 13472-13488 must stay stash-side. A cut that splits a subBegin/subEnd pair
  changes D0 run totals → byte diff.
- **Cut-map anchor regen** is per-increment (re-grep at increment start — already a hard rule), not
  a monolithic INC-0 deliverable; §4 above covers INC 1-4 planning.
- Card-6 relevel coupling: decls 2154-2176, restore 3882-3886, block 8964-9054, snapshot fields
  13911-13912 — four far-flung sites, INC-4/10 coordinate.
- Card-2 `code_bias_prev_`/ifpin refresh 13618-13643 is deliberately NOT snapshotted (in-code note
  13622) — preserve the asymmetry at INC-4.

## 7. Order of operations from here

1. ✅ Housekeeping commit = this addendum + plan-doc status update + `_dcmp_*` configs (freeze SHA
   for goldens = that HEAD; production code unchanged from `56da23a`).
2. **INC-0 (fable-only):** capture the §3 matrix (twice-run selfcheck → capture → commit manifests).
   Goldens/masks READ-ONLY after.
3. **INC-1** config split → each increment thereafter: build → `gnss_tests` → fast-matrix compare vs
   INC-0 → commit. One increment per session where possible; pausable at every boundary.

# UFA: an audit of user integer fixing (2026-11-18)

**STATUS**

- **id:** UFA
- **topic:** consumer
- **status:** open
- **summary:** The decision rules of user integer fixing in the order applied, what a hold is in each mode and what releases it, the defects by priority with the three known wrong holds mapped to rules, a split plan for the long resolver functions and the pure functions worth unit tests. The observed-data users run with partial fixing OFF (`enable_partial_ar: false` in their arm.json files); partial fixing on is the simulator users' recipe of `scripts/irdu_users.py`, which the draft took for theirs, and the statements built on it are corrected in the last section. Defects P1, P2, P3, P5, P9 and P10 stand; P3 is wider than first stated (every failed attempt, upper-bound rejects included, drops its release lists).
- **source:** plan package 4 of the post-reset program (2026-11-18), the audit of user integer fixing; read-only, no build and no run; network code and the record read at 7fb0737 (branch `chain/audit`), user-engine code at main 5ddc422. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The observed-data users fix integers through `resolveWlConstrained` with ratio 5, partial fixing off, the wide-lane branch check on, hold variance 1e-7 m2 and release at 0.25 cycle (their arm.json files; `scripts/irdu_users.py` is the simulator recipe, partial on). Every test after a fix reads the state the fix wrote back, so nothing independent watches a hold. On Galileo E1/E5a the wide lane is admitted by the code test alone, and a one-cycle wide-lane error leaves a 0.051 cycle narrow-lane residue. Every failed attempt discards its release and code-bias reset lists. The Galileo wrong hold fits these rules; the two GPS ones remain partly mapped.

Read at: network code and the record at 7fb0737 (frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`); user-engine code at main 5ddc422 (`C:/GnssProcessor`). Abbreviations: AR = `src/core/filter/AmbiguityResolver.cpp`, IC = `src/core/filter/IlsCore.cpp`, RTE = `src/core/realtime/RealTimeEngineEstimate.cpp`, all on main unless marked. Part files: `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/UFA_user_fixing_audit_2026-11-18_parts/` (call-path-and-core, dd-path, wl-constrained, wl-only, mw-constraints-and-hold, critic).

Rulings carried from the skeptic pass: REAL (confirmed against the code), NOT A DEFECT, UNCONFIRMED (mechanism plausible, size or occurrence not shown). Where a first-stage part and the skeptic disagree, the disagreement is stated at the item.

## 1. Paths

### 1.1 The path of a user of the adopted network

Call chain (closed-loop replay and real time share it):
`runNetwork` -> `runConsumerReplay` (7fb0737 `src/core/network/driver/ApplicationNetwork.cpp:207`) -> `RealTimeEngine::processEpoch` -> `resolveAmbiguities` (main `src/core/realtime/RealTimeEngine.cpp:566`) -> RTE:241 -> `AmbiguityResolver::resolve` (AR:505) -> `resolveCore_` -> `resolveWlConstrained` (AR:686-717). Post-processing PPP reaches the same `resolve` overload from `Application::resolveEpochAmbiguities_` (main `src/core/app/PppEpochUpdate.cpp:602-660`), with an identical install sequence.

Keys that select it:

| Key | Value on the adopted users | Effect | Where read |
|---|---|---|---|
| `network.closed_loop.replay_products_path` | set | replay path | 7fb0737 ApplicationNetwork.cpp:207 |
| `network.closed_loop.consumer_undifferenced` | false | between-satellite (BSSD) user; true forces `ar_wl_only` off | 7fb0737 `src/core/config/GnssConfigLoadNetwork.cpp:13765` |
| `network.closed_loop.ar_mode` | 1 | overrides the base file's `estimation.ambiguity_resolution.mode` | 7fb0737 GnssConfigLoadNetwork.cpp:13948 |
| `network.closed_loop.ar_wl_only` | false (explicit) | copied in; loader default is TRUE | 7fb0737 :13951-13952, default :13391 |
| (derived) `setArIgnoreMwWl(!ar_wl_only)` | true | the MW wide-lane map passed to the resolver is empty | 7fb0737 `src/core/app/ApplicationConsumerReplay.cpp:370`; RTE:262-268 |
| `estimation.ambiguity_resolution.wl_only` | absent, default false | no wide-lane-only branch | main `src/core/config/GnssConfigLoad.cpp:1997-1998` |
| `estimation.ambiguity_resolution.wl_from_float` | true | `resolveWlConstrained`, no per-band fallback | AR:686-717 |
| `estimation.ambiguity_resolution.state_ambiguity_basis` | default `per_band` | the NlWl path is not taken | main GnssConfigLoad.cpp:2161-2169 |

[corrected 2026-11-19: see Corrections, item 1] [corrected 2026-11-19: see Corrections, item 2] Values of the fixing block (`scripts/irdu_users.py:43-46`, variant `fix` adds `wl_branch_check: true`): mode 1, ratio_threshold 5, ratio_upper_bound 1e4, elevation_mask 20, min_epochs 10, max_position_sigma_m 0.05, enable_partial_ar true, hold_variance 1e-7 m2 (sigma 0.32 mm), wl_from_float true, nl_sigma_floor_cyc 0.04, nl_iono_free_row true, hold_release_cyc 0.25, refloat_partner_signal true. Defaults that apply (main `include/filter/AmbiguityResolver.hpp`): min_success_rate 0.999, nl_max_per_pair_frac 0.15, wl_max_frac 0.40, min_arc_info 0 (gate off), wl_branch_persist_s 120, extra_signals off. The link from this recipe to the observed-data users of section 118.1 is taken from the record (NRF part15 section 112.4) and the earlier UHT draft, not from a run folder (inferred).

The float reference user is the `obs` variant: the same block with ratio_threshold 1e9 (`scripts/irdu_users.py:24, 46`). With 1e9 above the upper bound 1e4, no ratio can pass (AR:2232-2240). The resolver still exists and runs its arc bookkeeping and partner refloat (section 4, P8).

`network.require_signal_integer_for_ar` is not read by this resolver. It reaches it only by marking keys unresolvable through `phaseBiasCoveredForAr` (main `src/core/measurement/MeasurementModelRow.cpp:117-130`, marks at `src/core/measurement/MeasurementModelStates.cpp:752-786`), which the candidate filter then drops (AR:432). The stream flag `wide_lane_integer` has no reader on the user side (only I/O at `src/core/products/NetworkProductIO.cpp:249-250`).

### 1.2 The other paths and who uses them

| Path | Entry | Selected by | Who uses it |
|---|---|---|---|
| Per-band unconstrained LAMBDA | AR:720-992 | ar_mode > 0, wl_from_float false, MW map empty, per_band | `config/test_suite/_pppar_algo_static_gps_l1l2.json`; any real-time user with MW feed off and wl_from_float off |
| MW-fed WL-constrained | AR:686 | non-empty MW map | top-level PPP-RTK configs would, but all set mode 0 |
| Wide-lane only (`resolveWlOnly`) | AR:677, AR:2540-2875 | wl_only true, per_band | none active: every config with wl_only true sets mode 0 (`config/ppp_rtk_internal_AR.json`, `config/realtime_ppp_rtk_AR.json`, `config/ppp_rtk_network_*.json`). Trap: dropping the explicit `closed_loop.ar_wl_only false` routes a BSSD user here (default true) and turns the MW feed on |
| NlWl basis (`applyMwWlConstraintsNlWl_`) | AR:564-581, 2940-3494 | state_ambiguity_basis nl_wl | `config/test_suite/_nlwl_*.json`, `config/archive` only |
| Baseline DD (`resolveDd`, `holdAmbiguitiesDd`) | AR:1007-1307, 3579-3627 | processing.mode Baseline-Single/Multi and AR mode > 0 | only the archived `config/archive/test_suite/_usgs_n141_m141*.json`; `config/baseline_single.json:56` and `config/baseline_multi.json:63` set mode 0 |

Correction of a first-stage claim: the dd-path part stated that `buildDdPairs` is not reached by any configuration. That holds for the top-level files only. The closed-loop users reach it through `resolveCore_` (AR:584) because the override at 7fb0737 GnssConfigLoadNetwork.cpp:13948 sets ar_mode 1 whatever the base file says.

## 2. Decision rules in the order applied (adopted path, then the other paths)

Scope letters: C = common prelude, W = `resolveWlConstrained`, K = caller, B = per-band LAMBDA, O = wide-lane only, N = NlWl, D = baseline DD. Rows C, W and K are the adopted path in order; B, O, N and D are condensed.

| # | Rule | Test and number or key | Pass | Fail | path:line | Counter or log line |
|---|---|---|---|---|---|---|
| K0 | Resolver built | ar_mode > 0 | resolver exists | no resolver, no partner refloat | main `src/core/realtime/RealTimeEngineInit.cpp:298` | info at build |
| K1 | Applied update | `update_applied` | call resolve | skipped; arc ages not advanced | RTE:254 | `last_ar_ran_` false |
| K2 | MW feed | `ar_ignore_mw_wl_` | MW map passed | empty map | RTE:262-268 | none |
| C1 | Arc restart | `isAmbiguityPendingReseed` or a new generation | age and info reset | - | AR:217, 244-264 | none |
| C2 | Partner refloat | one signal at generation age 1 while another has >= 3 calls; `refloat_partner_signal` | satellite to release list | - | AR:270-281 | PARTNERREL info, `n_partner_releases_` |
| C3 | Hold forgotten | arc of either satellite restarted, or key left the state | held entry erased | - | AR:259-264, 284-290 | none |
| C4 | Disabled | ar_mode == 0 | - | gate 1 | AR:538-542 | debug (dead on the real-time path) |
| C5 | Warm-up | `++epoch_count_ <= min_epochs` (10) | continue | gate 2 | AR:550-556 | debug |
| C6 | Basis | `state_ambiguity_basis` | per_band continues | nl_wl to N rows | AR:564-581 | debug |
| C7 | Candidate filter | x == 0; P == 0 (frozen SD pivot); elevation < 20; unresolvable; not in `ar_active_systems` | kept | dropped | AR:419-445 | none |
| C8 | Group and reference | per (constellation, signal) >= 2 members; reference = highest elevation, strict `>`, no memory | DD pairs | group dropped | AR:450-467 | none |
| C9 | No pairs | pairs empty | - | gate 4 | AR:642-646 | debug |
| C10 | Position sigma | sqrt(Pxx+Pyy+Pzz) > 0.05 m | continue | gate 3 | AR:655-664 | debug |
| C11 | Wide-lane only | `wl_only` | to O rows | - | AR:671-680 | debug |
| C12 | WL-constrained | `wl_from_float` or MW map non-empty | to W rows, no fallback | to B rows | AR:686-717 | - |
| W1 | Pair usable | sat_i observed; finite positive frequency and wavelength; group "sat_i\|sat_ref" | grouped | dropped | AR:1712-1724 | none |
| W2 | Primary split | exactly two signals (or first per primary band with extra_signals) | usable | group skipped | AR:1615-1649, 1731-1737, 1774, 1851 | none |
| W3 | Hold test | `hold_release_cyc` > 0 (0.25) and memory non-empty; key "sat_i\|sat_ref\|sig_hi\|sig_lo"; \|d_wl\| or \|d_if\| or any \|d_ewl\| > 0.25 | ok | release (blame by `chooseReleaseSatellites_`), erase memory | AR:1768-1838, 309-335 | HOLDREL info, `n_hold_releases_`, `extra_counters_.releases_caused`; lost on fail (P3) |
| W3a | Silent hold skips | group unusable; key miss; index out of range | - | entry not tested | AR:1774, 1781, 1782-1784 | none |
| W4 | Released | satellite in released set (C2 or W3) | - | pair skipped | AR:1857-1858 | none |
| W5a | Wide lane, branch check | `decideWlBranch`: NotFinite; code admits if \|d\| + 3 sigma_w <= 0.40; Reach if 1 + \|d\| <= 3 sigma_w; phase decisive if \|r\| + 3 sigma_n <= gap/2; persistence 120 s | dd_wl decided | pair skipped or Undecided | AR:1399-1448, 1450-1466, 1529-1574, 1864-1880 | WLBRANCH, WLFLOAT, `wl_branch_counters_` |
| W5b | Wide lane, no branch check | \|frac\| + 3 sigma <= 0.40 | dd_wl = round | skipped | AR:1881-1895 | WLFLOAT |
| W5c | Wide lane from MW | both per-satellite keys present | dd_wl = difference | skipped | AR:1896-1908 | WLDUMP; missing key silent |
| W6 | Maturity | min arc info >= `min_arc_info` (0: off) | kept | skipped | AR:1916-1925 | `n_skipped_immature`, one line |
| W7 | IF-row amplification | (f1+f2)/(f1-f2) < 20 (with nl_iono_free_row) | kept | dropped | AR:1955-1969 | warn, no counter |
| W8 | Extra lanes | \|e - round(e)\| + 3 sigma_e <= 0.40 | admitted | extra dropped | AR:1651-1677, 1980-1987 | EWLFLOAT, `extra_counters_` |
| W9 | Contradiction response | branch contradictions | - | `bias_reset_satellites` | AR:2002-2010 | WLBRANCH, `satellites_returned`; lost on fail (P3) |
| W10 | No candidates | NL set empty | - | return float | AR:2019-2022 | debug |
| W11 | Search problem | T rows from `nlRowCoefficients`; Q = T Qdd T' + shared-reference floor 0.04 cycle | built | - | AR:1351-1393, 2044-2117 | ARCAGE |
| W12 | Search | mlambda ok and s1, s2 > 0 | continue | return float | AR:2192-2214; IC:216-249 | debug only |
| W13 | Ratio low | s2/s1 >= 5 | continue | full_ok false | AR:2232-2234 | info |
| W14 | Ratio high | ratio <= 1e4 | continue | full_ok false | AR:2236-2240 | warn "degenerate covariance" |
| W15 | Success rate | bootstrap P_s >= 0.999 | continue | full_ok false | AR:2243-2248 | info |
| W16 | Per-pair cap | max \|y - a\| <= 0.15 cycle | full fix | full_ok false | AR:2250-2265 | info |
| W17 | Partial | `enable_partial_ar`; greedy drop, each subset re-tested by the full battery, down to one entry | subset fix | return float | AR:2269-2286; IC:294-358 | QBCOND path=reject; info on outcome |
| W18 | Write-back | Joseph pseudo-update, two rows per pair (+1 sat_i, -1 sat_ref) per primary signal plus fixed extras, R = hold_variance I | fixed_state, fixed_covariance, held_pairs | - | AR:2288-2338, 2402-2450 | QBCOND, summary info |
| C13 | Upper bound again | wl_result.ratio > 1e4 | - | [corrected 2026-11-19: see Corrections, item 4] success false; wl_result returned with its lists | AR:688-699 | warn (duplicates W14) |
| C14 | Failure exit | W attempt failed | - | outer empty `result` returned; W3 and W9 lists dropped | AR:715-717 | debug |
| C15 | Memory commit | success and hold_release_cyc > 0 | held_pairs_ overwritten per key | - | AR:509-513 | none |
| C16 | Partner append | `partner_release_` | appended to release list | - | AR:514-518 | - |
| K3 | Install | success | state and covariance overwritten | - | RTE:279-281; PppEpochUpdate.cpp:626-627 | info "AR: fixed X/Y" |
| K4 | Hold pass | ar_mode == 3 | second update on same rows | no-op | AR:3506, 3523-3567 | info |
| K5 | Refloat | release list non-empty | `refloatSatellites` at initial ambiguity sigma | - | RTE:287-289; AR:336-351 | info, run totals |
| K6 | Code-bias reset | `bias_reset_satellites`, `estimateRxSatCodeBias` | reset to prior | - | RTE:298-306 | info |
| B1-B7 | Per-band LAMBDA | per-band lambda from sat_i (AR:767-777); search, s > 0 (AR:798-806); ratio >= threshold (AR:822); ratio <= upper (AR:825); P_s (AR:833); per-DD cap (AR:848); partial (AR:861-879); write-back R = hold_variance (AR:898, 965-967) | fix | float; no held_pairs, so no release test | AR:720-992 | info / warn; no gate code after search failures |
| O1-O9 | Wide-lane only | pair admitted (AR:2566-2571); exactly two signals (AR:2591); both MW keys (AR:2605-2609); frac <= 0.40, missing frac reads 0 (AR:2620-2626); none -> gate 0 (AR:2643-2646); FFR P_s >= 0.999 drop weakest arc (AR:2676-2709); \|innovation\| <= 0.5 cycle, absolute (AR:2746-2785); integer change -> 100 m2 reset (AR:2798-2827); Joseph R = 1e-4 cyc2 (AR:2831-2873) | WL rows applied | gate 0 or 5 | AR:2540-2875 | debug, warn "FRAME conflict" |
| N1-N17 | NlWl | position sigma (AR:564-576); MW arc admission (AR:3012-3095); pivot by elevation per constellation and band pair (AR:3129-3152); FFR (AR:3233-3261); screen (AR:3264-3306); three-band parity (AR:3328-3416); change -> 144 cyc2 (AR:3426-3447); Joseph R = 1e-4 cyc2 (AR:3464-3494); `holdAmbiguities` returns (AR:3513) | WL rows applied | gate 0, 3 or 5 | AR:2940-3494 | debug / warn / info |
| D1-D23 | Baseline DD | [corrected 2026-11-19: see Corrections, item 7] mode, warm-up, position sigma (AR:1015-1039); key form, exclusion, constellation, observation, elevation of sat_i only, system, wavelength, index, seed (AR:1067-1122); >= 2 (AR:1131); search, s > 0 (AR:1166-1174); ratio, upper, P_s, per-DD (AR:1187-1221); partial (AR:1226-1262); full (AR:1265-1306); caller installs always, then `holdAmbiguitiesDd` in mode 3 (main `src/core/app/ApplicationBaseline.cpp:1233-1236`) | fix | float; caller drops ratio and reason | AR:1007-1307 | `n_skipped_*`, info; no gate code, no run-end counter |

## 3. What a hold is in each mode and what releases it

| Mode and path | What the hold is | What releases it |
|---|---|---|
| per_band, mode 1 or 2, WL-constrained (adopted) | A fresh Joseph pseudo-observation of the re-searched integers on every accepted epoch, R = hold_variance (1e-7 m2 in the recipe, loader default 1e-3, main GnssConfigLoad.cpp:1989-1990), written into the state (RTE:279-281). No persistent row exists. The memory `held_pairs_` (n1, wl, extras per key) serves only the release test and is overwritten by each accepted fix (AR:509-513). | W3 on a key match (misfit > 0.25 on WL, IF-NL or extra-lane scale); C3 arc restart or key removal (memory only); C2 partner refloat; K5 refloat. A refloat resets the satellite's ambiguities to the initial sigma. Nothing releases the information of an epoch whose re-fix was rejected (W14) or whose attempt failed (C14). |
| per_band, unconstrained LAMBDA | The same per-epoch write-back (AR:898, 965). | No held_pairs are stored, so no release test runs; only generic slip resets and C2. |
| per_band, mode 3 | The same write-back, then `holdAmbiguities` repeats the update on `fixed_dd_rows` at the same R against a state already equal to the fix, so v = 0 and the covariance is contracted a second time with no new information (AR:3523-3567, after RTE:280). | As mode 1. |
| wide-lane only | WL pseudo-observations at `wl_only_pseudo_var` 1e-4 cyc2, written back every epoch; no held_pairs (AR:2831-2873; `holdAmbiguities` ignores it, AR:3523-3526). | None in this path. `previous_dd_wl_` is never erased (only writes AR:2867, 3480). The integer-change reset (O8) runs after the screen and cannot fire on a one-cycle change (P13). |
| NlWl | WL pseudo-observations at 1e-4 cyc2; no NL fix; no held_pairs. | Only the integer-change reset of the non-pivot WL columns (N16). |
| baseline DD | [corrected 2026-11-19: see Corrections, item 7] The resolver's update at R = hold_variance, installed always (ApplicationBaseline.cpp:1233-1235); in mode 3 `holdAmbiguitiesDd` constrains every "DD\|" key, including keys never fixed, with v = 0 (AR:3589-3623). | No release rule. Generic responses only: `handleBaselinePhaseOutlier` re-seeds one key (main `src/core/filter/KalmanOutlierResponse.cpp:273-347`); the complete reset re-inflates diagonals only (KalmanOutlierResponse.cpp:54-59, 417-426); a pivot handover S-transforms (main `src/core/filter/DdPivotManager.cpp:170-247`) or prunes and reseeds (DdPivotManager.cpp:140-149). |

Life of a held pair on the adopted path:
- Reference change: the key holds sat_ref (AR:512, 1779), and the reference is re-elected every call by elevation with no hysteresis (AR:461-467). Under a new reference the entry is never found (W3a), so the pair goes untested until a fix under the new reference. The old entry stays until an arc of either satellite restarts, and is tested again if the old reference returns. No counter.
- Slip flag: `resetAmbiguity` -> `markAmbiguityPendingReseed` -> generation bump (main `src/core/filter/StateManager.cpp:461-505, 1883-1886`); C3 forgets every hold of the satellite at the next resolve, C2 refloats its other signals.
- Correction discontinuity counter or freshness flip: `resetAmbiguity` (main `src/core/realtime/RealTimeEngineSsrBreaks.cpp:305-307, 351`), same route. The carry variants use `shiftAmbiguity`, which bumps the generation when delta != 0 (StateManager.cpp:546) and keeps the covariance: the memory is dropped while the state stays tight; with partner refloat armed a shift on one signal refloats the whole satellite (inferred from AR:270-281). Keys claimed by the hold seam skip the counter reaction (SsrBreaks.cpp:297-302).
- Gap: a static-short gap keeps keys, generations and holds and zeroes cross-covariances only (main `src/core/app/PppEpochSteps.cpp:303-310`); static-long and kinematic gaps prune all ambiguities (PppEpochSteps.cpp:317-319). Real-time gap handling was not read.

## 4. Defects by priority

### P1 (high). Nothing independent watches an installed hold. REAL (skeptic, new)
Every test after a fix reads the written-back state: `holdMisfit_` (AR:309-319) and `floatWidelane_` (AR:1313-1332) read x, which RTE:279-281 replaced with fixed_state at R = 1e-7 m2; the narrow-lane battery re-searches a y already pulled to the held integer. A wrong hold therefore confirms itself. Scenario: G10-G02 fixed one NL cycle off; the next epoch the DD sits within millimetres of the wrong integer, d_if is near 0 and HOLDREL cannot fire. Invariant 1 of CLAUDE.md: the response to a moved truth is wired but blinded. The quantity a release needs must be one the write-back does not pull (decision 2).

### P2 (high). Galileo E1/E5a: the wide lane is admitted by the code test alone, and the narrow-lane battery cannot see a one-cycle wide-lane error. REAL (skeptic upgraded the wl-constrained part's "nearly blind")
[corrected 2026-11-19: see Corrections, item 3] On the iono-free row shift = -f2/(f1-f2) (AR:1380): -2.949 for E1/E5a (and GPS L1/L5), -3.529 for L1/L2. The gap is 0.051 cycle (AR:1523), half gap 0.026. With the floor, sigma_n = sqrt(var + 0.04^2) >= 0.04 (AR:1521-1522), so 3 sigma_n >= 0.12 and neither candidate can be decisive (AR:1419-1420). The branch check on Galileo is exactly the code test (AR:1407): no contradiction, no persistence, no code-bias reset can occur there. A one-cycle wide-lane error moves the NL float by 2.949 cycles, which leaves it 0.051 cycle from an integer, inside the 0.15 cap (W16) and well within the ratio test. On L1/L2 the same error leaves 0.471 cycle and is caught. Scenario: E18-E06 with code multipath of 0.3 WL cycle and sigma_w 0.03 passes |d| + 3 sigma_w = 0.39 <= 0.40 on the wrong integer; the fix is accepted and then held by P1.

### P3 (high). A failed WL-constrained attempt discards its release and code-bias reset lists. REAL (call-path F3 = wl-constrained D1)
[corrected 2026-11-19: see Corrections, item 4] AR:715-717 returns the outer empty `result`; AR:688-699 returns `wl_result`. Before failing, `resolveWlConstrained` has already logged HOLDREL, incremented `n_hold_releases_` and erased the memory (AR:1825-1836), and on contradictions logged the reset request and counted `satellites_returned` (AR:1556-1559, 2003-2009). Scenario 1: one held pair drifts past 0.25, its satellite leaves the search (AR:1857), the remaining set fails the ratio: the satellite is not refloated, its memory is gone, and next epoch it re-enters the search on its still-pinned state. Scenario 2: a lone contradicted pair empties the candidate set (AR:2019-2022), same loss. The same failure behaves two ways: the upper-bound reject keeps the lists. Counters report refloats and resets that never happened (invariant 4). `tests/test_ar_wl_branch.cpp:607-625` passes because it calls `resolveWlConstrained` directly (tests/test_ar_wl_branch.cpp:47-50). Partner releases survive (AR:514-518).

### P4 (high). The hold is re-applied every accepted epoch, so the same integer information accumulates. Mechanism REAL, size UNCONFIRMED
Each accepted epoch adds a row at R = hold_variance on the same DD; with ambiguity q at the loader default 1e-12 (main GnssConfigLoad.cpp:2227) the DD variance falls roughly as R/k after k epochs (inferred). Scenario: one hour fixed, then a one-cycle slip on G05 L1 missed by the slip layer: the stiff DD pushes the step into ionosphere, clock and position, and P1 hides it. The header's BAKE00CAN account (AmbiguityResolver.hpp:429-434) shows a float can still drift 0.7-0.9 WL cycle, so the size needs a simulator injection. The users' q was not read.

### P5 (medium). A reference change orphans held pairs with no event. REAL (wl-constrained D3 = mw-hold D4)
W3a: a key miss after a reference change is a bare `continue` (AR:1781), as are an unusable group (AR:1774) and an index out of range (AR:1782-1784). Scenario: G05-G10 held one WL cycle wrong; G12 overtakes G10; the search under G12 sees an inconsistent float, fails and stays float; the wrong tight state remains and no HOLDREL is possible. Invariant 7 asks for an event at both edges; the baseline path re-expresses holds on a pivot change (DdPivotManager.cpp:170-247), the single-difference path does not.

### P6 (medium). References are elected per signal, so the bands can split and the constellation silently drops out. REAL (call-path D8 = wl-constrained D2 = wl-only D6, band part)
C8 picks a reference per (constellation, signal). When the highest satellite has no resolvable ambiguity on one band (masked by C7 or seeded at 0), the bands get different references, no "sat_i|sat_ref" group holds two signals, and every group fails W2 (AR:1712-1724, 1774, 1851) with no counter. Under BSSD the frozen pivot (P == 0, AR:421) never takes part (inferred). Frequency on observed data needs a trace.

### P7 (medium). The ratio upper bound rejects the best floats and the re-fix of an installed hold. Reasoning REAL, frequency UNCONFIRMED (call-path F4 extended by the skeptic)
The ratio s2/s1 is unbounded as s1 -> 0 (AR:2216). Scenario: one NL ambiguity, sigma 0.05 cycle, offset 0.005 cycle: s1 = 0.01, s2 = 396, ratio 3.96e4 > 1e4, warn "degenerate covariance" (AR:2236, 825). Degeneracy is a property of Q (`qb_pre.sig_lo` is computed for it, AR:792, 2093). After a write-back the re-search sees y near a, so the bound can reject the re-fix of a held pair: success false, state still pulled, memory kept but not refreshed.

### P8 (medium). The integer settings change the float solution, including the float reference user's. REAL (call-path F2)
With ar_mode > 0 the resolver exists, so C1-C3 run every applied epoch and the partner rule refloats every signal of a satellite when one restarts (AR:270-281), appended at AR:514-518, applied at RTE:287-289. The `obs` user (ratio 1e9) runs the same rule, so the fixing and float users share it and neither equals a resolver-free float run. The comment at AR:533-535 assumes an ar_mode 0 resolver exists for comparison; no builder constructs one (RealTimeEngineInit.cpp:298, main `src/core/app/PppSetup.cpp:417`, ApplicationBaseline.cpp:658), so AR:538-542 is dead. Whether the grader's held pair-epoch count includes rejected re-fixes with a pulled state was not read.

### P9 (medium). One release threshold on three scales. REAL (mw-hold D6)
0.25 cycle is applied to d_wl (WL cycles, about 21 cm on L1/L2), d_if (NL cycles, about 2.7 cm) and d_ewl (AR:1789-1790, 1810).

### P10 (medium). The warm-up counts resolver calls since construction and is never reset. REAL (call-path D6)
`epoch_count_` (AR:550) survives refloats, partner releases, gaps, hold releases and a complete filter reset (KalmanOutlierResponse.cpp:413-426 does not touch it). After hour 1 a restarted set has no warm-up; only the position-sigma gate guards it, and it looks at position, not ambiguities.

### P11 (low). An exactly integer float is rejected. REAL (call-path D10)
s1 <= 0 returns float with no partial on the full set (AR:804, 2211); the partial loop drops entries on the same condition (IC:323). Reachable on noise-free simulator data, where integer questions are settled first.

### P12 (low). Modes. REAL (call-path F5 = dd D3; mw-hold D2)
Mode 1 ("continuous") and mode 2 ("instantaneous") are the same code: no test of `ar_mode == 2` exists in src (grep); every accepted fix is persisted (RTE:280-281; ApplicationBaseline.cpp:1233-1235). Mode 3 repeats the rows with v = 0 (section 3). Off the adopted path.

### P13 (low, off path). Wide-lane-only path. REAL (wl-only D1, D4, D5)
D1: the innovation screen (AR:2746-2771) runs before the integer-change detection (AR:2806-2826); after a re-lock from N to N+1 the innovation is about 1 cycle > 0.5, the row is dropped and the reset never fires, with a "FRAME conflict" warn every epoch. D4: an empty constraint set returns with gate_reason 0 (AR:2645), which `ConsumerTrace.cpp:29-30` labels "mlambda_fail". D5: a missing fraction reads 0 and passes the 0.40 test (AR:2620-2623), the same fail-open as NlWl (AR:3048). The unnormalised screen (wl-only D2) is a design weakness rather than a defect: a code-seeded arc 1.3 cycles off is rejected until it converges unaided.

### P14 (low, off path). Baseline DD. REAL (dd D1, D2, D4, D5, D8)
D1: `holdAmbiguitiesDd` pins every "DD|" key, including non-integer-capable ones (C2W against C2C under signal aliasing, main `src/core/measurement/BssdTransform.cpp:434-436`), partial-fix rejects and keys below the mask (AR:3589-3597): a stiff prior with no truth under it. D2: double update per epoch. D4: elevation and observation are tested on sat_i only (AR:1097-1102), never on the pivot, which `DdPivotManager` selects by SNR (DdPivotManager.cpp:54-61). D5: four causes share "malformed/unknown"; no counter for the constellation filter; the "staying float" warn is followed by a partial fix (AR:1192-1195, 1225-1262). D8: hand key parsing (AR:1073-1085) instead of `StateKeys::parseDdAmbiguityKey`; `RealTimeEngineArObserve.cpp:73-104` re-implements and has drifted from C7.

### P15 (low). Telemetry and dead code. REAL
Uncounted drops: C7, C8, W1, W2, W4, W5c, W7, W12 (debug only). gate_reason stays 0 after search failures (AR:769-806, 717). `num_candidates` on WL paths is the per-band DD count while `num_fixed` counts NL or WL rows (AR:679, 698), so "fixed X/Y" (RTE:283) mixes units (inferred). Dead: `consumed_dd` (AR:1996-1997), `Qab` (AR:747-752), the duplicated upper-bound test (AR:2236 and 692), AR:538-542 on the real-time path. Duplicated: the full and partial write-backs (about 40 lines, AR:2297-2331 against 2411-2442); the acceptance battery three times (AR:822-858, 1187-1223, 2232-2267). Computed and discarded: the runner-up integer vector (IC:234-249, only s(1) kept), P_s and max fraction on rejection (log only), `qb_pre` on a full failure with partial off, the full-set ratio after a partial fix (AR:1257).

### Not a defect
- [corrected 2026-11-19: see Corrections, item 5] call-path F1 ("the user cannot fix any integer"): ratio 1e9 is the float variant by design (`scripts/irdu_users.py:24, 46`). The first-stage part read the `obs` user's configuration; its "partial off" does not describe the fixing users (partial on in the recipe). Refuted.
- mw-hold D5: blaming the reference needs two contradicted pairs and a majority (AR:330). By design, but it widens P1: a reference slip with one held pair refloats the innocent sat_i.
- wl-constrained D4 as worded ("not a bug"): superseded by P2.

### Unconfirmed
call-path D7 (arc age counts calls with an applied update, not epochs), call-path D9 and dd D7 (per-band lambda from sat_i for FDMA GLONASS; GLONASS inactive in the reference configurations), dd D9-D11 (no test calls `resolveDd` was not re-checked), wl-only D3 (pinned wrong WL from understated sigma), wl-only D7 (stale `previous_dd_wl_` after a re-rise), wl-only D9 (MW sigma understated about 4 times at n = 3600 beyond the 100-sample Hatch window, inferred).

### The three wrong holds of section 118.1 (7fb0737 NRF part16 line 142)

The record lists three short runs one cycle off among 114 211 to 126 470 held pair-epochs of the combined-mode users outside the network.

| Run | Error | Rule that let it through | Rule that should have released it | Status |
|---|---|---|---|---|
| UTQI E18-E06, DOY 146, 33 epochs; the same run on the current stream, 34 epochs (so a user-side cause, inferred) | one WL cycle (float user one WL cycle away) | W5a reduced to the code test (AR:1407) by P2; W13-W16 blind to it (0.051 cycle residue) | none can: W3 reads the pulled state (P1) | mapped (P1, P2) |
| QUIN G10-G02, DOY 146, 14 epochs | one NL cycle, WL right | [corrected 2026-11-19: see Corrections, item 6] W13-W16 on the floored matrix, or a W17 subset (a one-entry subset needs only \|frac\| <= 0.15 and sigma below about 0.15 cycle); which one cannot be told | W3 d_if at 0.25, self-confirming (P1); a HOLDREL in an epoch whose set then failed is lost (P3) | partly mapped (P1, P3 possible) |
| ASCG G20-G23, DOY 147, 16 epochs | one NL cycle, WL right | [corrected 2026-11-19: see Corrections, item 6] as QUIN | as QUIN | partly mapped |

The two GPS runs most likely ended by an arc restart, a partner refloat, a reference change or a re-search that chose other integers (inferred). Not decidable without a per-decision trace: full set or subset at onset, ratio, P_s and pair fraction at onset, a reference change before onset (P5), a correction step on either satellite, how each run ended, and whether a HOLDREL fired and was discarded at AR:717. The users ran with `ambiguity_resolver` at debug (`scripts/irdu_users.py:21`), so the existing logs may settle part of this, including the P3 case: the HOLDREL line survives while the refloat does not.

## 5. Split plan

Principle: structure first, identical output; behaviour changes after, each behind a default-off key. A step is "compile-only" when it moves no executed code (declarations, a header, a test, a struct introduced but not yet used); "identical-output" when it moves executed code and must reproduce the solution, the AR trace and the counters byte for byte. Log text is kept verbatim in identical-output steps; a step that changes a log line declares it.

Proof by kind:
- compile-only: Debug build of `gnss_core` and `gnss_tests`; the unit suite passes.
- identical-output: the above, plus a same-build comparison of a pre-step and post-step Release executable on (a) one simulator user replay (fixing and float variants) and (b) one observed-data user of section 118.1, 2 h each, comparing the solution file, the AR summary lines and the run-end counters byte for byte. These are runs listed for later; none is requested now.

### 5.1 resolveWlConstrained (AR:1680-2466, first)

A plain struct `WlcEpoch` carries what crosses stages; each stage is a private member function with a short linear body; a declared table `kWlcStages` lists them in order with inputs, outputs and the rules (section 2) each owns.

| Stage | Function | Inputs | Outputs | Rules | Replaces |
|---|---|---|---|---|---|
| S1 | `groupSatellitePairs` | pairs, observations | groups (key, infos, PrimaryExtraSplit) | W1, W2 | AR:1696-1737 |
| S2 | `testHeldPairs` | groups, x; reads and writes `held_pairs_` | released, release_satellites, one `HoldCheck` per held entry {ok, contradicted, unusable_group, key_miss, bad_index} | W3, W3a | AR:1765-1838 |
| S3 | `decideWideLane` (per group) | group, x, P, released, branch time | optional dd_wl with source and reason; branch epoch | W4, W5a-c | AR:1840-1908 |
| S4 | `gateCandidate` (per group) | group, dd_wl | optional NlCandidate | W6, W7, W8 | AR:1910-1998 |
| S5 | `settleContradictions` | branch epoch | bias_reset_satellites | W9 | AR:2000-2010 |
| S6 | `buildSearchProblem` | candidates, pairs, x, P | T, y, nl_shift, Q, qb_pre; ARCAGE | W10, W11 | AR:2019-2117, 2192-2197 |
| S7 | `searchAndAccept` | search problem | integers, s, ratio, P_s, worst pair, verdict {full, partial(kept), reject} | W12-W17 | AR:2199-2286 |
| S8 | `writeBack` (one for both verdicts) | candidates, kept indices, integers | fixed_state, fixed_covariance, fixed_dd_rows, held_pairs | W18 | AR:2288-2338, 2402-2450 |
| S9 | `reportFix` | qb_pre, P_s, max fraction, fix | QBCOND and summary lines | - | AR:2336-2351, 2448-2464 |

Steps:
1. compile-only: declare `WlcEpoch`, the `HoldCheck` outcome enum and `kWlcStages` (unused); add unit tests pinning the existing pure functions (section 6).
2. identical-output: extract S1 and S9 (no decisions move).
3. identical-output: extract S2 with the outcome record but no new counter written to any output; S3 and S5.
4. identical-output: extract S4 and S6; delete `consumed_dd` (dead, so still identical).
5. identical-output: extract S7, keeping the order of the battery and its log lines; then S8 merging the two write-backs (the partial path is the full path with a kept-index subset; the proof must cover a run that takes a partial fix).
6. identical-output with declared log addition: counters for W3a outcomes, W1/W2/W4/W5c/W7/W12 drops and the per-band reference split (P6), reported in the resolver's run-end line. Numbers unchanged; the run-end text grows (declared).
7. behaviour change, default off (new key, ledger row in the same commit): `resolveCore_` returns `wl_result` with success false on every failed exit, so release and reset lists reach the caller (P3). Off: identical. On: a simulator injection run (listed later).

### 5.2 resolveCore_ (AR:523-992)

A declared gate table `kArGates` (rule, gate code, test) replaces the inline sequence C4-C12; the per-band body becomes `resolvePerBand_` with stages `buildPerBandProblem` (B1), `searchAndAccept` shared with S7 (B2-B6), `writeBack` shared with S8 (B7).
1. compile-only: gate table declared; the duplicate upper-bound test at AR:692 documented as redundant.
2. identical-output: move C4-C12 into the table walker, preserving gate codes and log text.
3. identical-output: per-band body onto the shared S7 and S8 (the battery returns a verdict; each caller keeps its own log text). Remove `Qab`. Proof needs one per-band LAMBDA run (`config/test_suite/_pppar_algo_static_gps_l1l2.json`, listed later).
4. identical-output: delete the AR:692 test only after step 3 shows W14 makes it unreachable (W14 rejects every ratio above the bound before success is set; inferred, to be proven by the comparison).

### 5.3 resolveWlOnly (AR:2540-2875) and applyMwWlConstraintsNlWl_ (AR:2940-3494)

Stages: `buildWlOnlyConstraints` (O1-O4), `eliminateWeakArcs` (O6), `screenWlInnovations` (O7), `detectWlIntegerChanges` (O8), `applyWlPseudoObs` (O9); one `wlDdPrediction` helper replaces the four copies (AR:2734, 2754, 2843, 2878). NlWl reuses the screen and FFR stages and adds `checkThreeBandParity` (N15). Steps 1-3 identical-output; no current configuration takes either path, so the proof is the unit suite plus one `_nlwl_*` test-suite run (listed later). Reordering O8 before O7 (P13 D1) and a normalised screen are behaviour changes, default off, or the paths are retired (decision 9).

### 5.4 resolveDd (AR:1007-1307)

Stages: `filterDdCandidates` (D4-D12, with `StateKeys::parseDdAmbiguityKey` replacing the hand parse: identical-output if the parse accepts the same keys, proven by a unit test over the key forms), the shared acceptance stage, the shared write-back. Proof: one archived `_usgs_n141_m141` configuration run (listed later). The D1 scope fix of `holdAmbiguitiesDd` is a behaviour change.

### 5.5 refreshArcAges_ (AR:200-290)

Extract the generation-restart detector as a pure function (keys, generations, previous generations) -> restarted satellites, and the partner rule as a pure function over arc ages: identical-output.

## 6. Pure functions for unit tests (plan package 15)

Existing, testable now (friend shims, AmbiguityResolver.hpp:889-906):
- `ils::mlambda`, `ils::search` (IC:216-249): 1-D; 2-D strongly correlated (reduction matters); a float exactly on an integer (s1 = 0: pin the current rejection of P11); found < 2.
- `ils::computeBootstrapSuccessRate`: d = 0 gives 1; monotone decreasing in each d; one large d dominates.
- `ils::selectFixableSubset` and `selectFixableSubset_` (IC:294-358, AR:96-122): all pass; shrink to one entry; ratio failure drops the largest variance, per-pair failure drops the largest fraction; nothing passes.
- `holdMisfit_` (AR:309-319): zero misfit; a common NL error e gives d_if = e and d_wl = 0; one cycle on band 1 alone gives d_wl = 1 and the matching d_if, on L1/L2 and on E1/E5a.
- `chooseReleaseSatellites_` (AR:321-335): one contradicted pair blames sat_i; reference bad in 2 of 3 held pairs blamed; 2 of 4 not blamed (needs 2*bad > held).
- `decideWlBranch` (AR:1399-1448): NotFinite; Reach; code admits W0; Contradiction; PhaseW1; Undecided; E1/E5a with floor 0.04 never phase-decisive (pins P2).
- `stepWlBranchPersistence` (AR:1450-1466): decision after 120 s of the same integer; restart on contradiction; a change of integer restarts.
- `floatWidelane_` (AR:1313-1332): known DDs and covariance give w and sigma_w.
- `nlRowCoefficients` (AR:1351-1393): shift -3.529 (L1/L2) and -2.949 (E1/E5a, L1/L5); narrow-lane row against IF row.
- `addSharedReferenceFloor_` (AR:1334-1345): the floor enters only entries sharing the higher signal's reference.
- `wlArcSuccessRate_` (AR:2491-2536): empty set gives 0; missing key gives 0; count below `wl_only_min_arc_count` gives 0; sigma 0 gives 1; negative sigma gives 0; exact pivot gives 1.
- `KalmanFilter::josephPseudoUpdate` (main `include/filter/KalmanFilter.hpp:225`): zero innovation contracts the covariance only; k repeated applications give variance near R/k (pins P4 and P12).

Testable after a compile-only move out of the anonymous namespace: `qbCondition`, `rebuildQbCycles` (AR:154-189): identity Q, rank-deficient Q, condition number.

Extractable by the split (identical-output steps above), then testable:
- Candidate predicate (C7, AR:419-445): each exclusion alone, including P == 0 for the frozen pivot.
- Reference election (C8, AR:450-467): strict `>` tie goes to map order; a masked band on the top satellite produces split references (pins P6).
- Generation-restart detector and partner rule (AR:244-281): one signal restarts with another at age >= 3; both restart; key removed.
- Acceptance battery verdict (W13-W16): each failure alone, in order; ratio above the bound with s1 near 0 (pins P7).
- `splitPrimaryAndExtra` (AR:1615-1649): two signals; three without primary bands (unusable); extra_signals.
- Wide-lane-only stages: constraint formation with a missing fraction (pins P13 D5); screen; change detection after the screen (pins P13 D1).
- Three-band parity (AR:3328-3411): zero and non-zero telescoping sums.
- DD key parse through `StateKeys::parseDdAmbiguityKey`: every key form `resolveDd` accepts today.

## Decisions for the author

1. Release and reset lists on a failed attempt (P3): return them on every exit behind a default-off key, or make it the only behaviour once the simulator injection shows it. It changes refloats.
2. The quantity an independent release test reads (P1): the float state saved before the write-back (a shadow float, owned by `StateManager`), a code-only wide lane from the correction stream's code biases, or the pair's phase residual history.
3. Galileo E1/E5a wide-lane admission (P2): accept the code test alone, require persistence on code admission, or lower the floor for the branch decision only. Simulator first.
4. Re-application of the hold (P4): keep the per-epoch re-fix, or hold one persistent constraint per fixed DD and skip re-applying an unchanged integer.
5. Reference election (P5, P6): hysteresis, one reference per satellite pair across bands, and an event that re-keys or re-expresses held pairs on a reference change, as the baseline handover does.
6. Ratio upper bound (P7): replace by a test on Q (`qb_pre.sig_lo`), or keep and stop it rejecting the re-fix of an installed hold.
7. The float reference user (P8): keep it with the resolver on (partner refloat shared with the fixing user), or build it with `refloat_partner_signal` off or with an ar_mode 0 resolver for a clean comparison; and whether the held pair-epoch grade counts rejected re-fixes.
8. Modes (P12): remove mode 2 or implement it as instantaneous; drop the second update of mode 3.
9. Off-path code: repair or retire the wide-lane-only, NlWl and baseline DD resolvers (no current configuration takes them); and whether `closed_loop.ar_wl_only` keeps its default true.
10. Warm-up (P10): reset `epoch_count_` per restarted set, or replace it by a per-arc maturity bar.

## Not checked

- `GnssConfigLoadNetwork.cpp` line numbers were read on both trees by different parts; the path keys agree, but no line-by-line comparison of the two trees was made.
- The users' ambiguity process noise and the effective hold variance in the run folders (taken from `scripts/irdu_users.py`, not from the observed-data users' arm files); the size of P4.
- The partial and full accept blocks of `resolveWlConstrained` (AR:2270-2466), `decideWlBranchOfPair_`, `selectFixableSubset_` internals beyond the summary, and `resolveDd` beyond its rule list were read in part only.
- Whether `reactToPbHold_` bumps the ambiguity generation; real-time gap handling.
- Whether any run log shows PARTNERREL lines, discarded HOLDREL lines or reference changes before the three wrong holds; no run folder was opened.
- How the grader `user_fix_vs_float_free.py` counts held pair-epochs.
- Whether the MW pair order always matches the resolver's high/low frequency order; the MW frame of a closed-loop BSSD user.
- That no test calls `resolveDd` or `holdAmbiguitiesDd` (dd D11) was not re-checked by the skeptic.

Runs listed for later, none requested: a simulator user with a one-cycle wide-lane error injected on a Galileo pair (P1, P2); a simulator user with a one-cycle narrow-lane error on a held GPS pair and an injected reference change (P1, P3, P5); a missed one-cycle slip on a held pair after one hour (P4); the same-build identical-output comparisons of each split step (section 5); one per-band LAMBDA, one NlWl and one archived baseline run for the off-path steps. Before any build: read the existing debug logs of the six user runs for how each wrong hold began and ended.

## 2026-11-19: corrections from the check against the code

Trees: main = `C:/GnssProcessor` at 5ddc422 (user-engine code); 7fb0737 = the frozen checkout (network code, record). User files read by the check: QUIN = `runs_nom/_rap_nw_obsadx_143_quin/arm.json` (fixing user), BADG = `runs_nom/_rap_nw_obsadf_144_badg/arm.json` (float user). AR and RTE as in the body. Items 1 to 7 carry inline markers in the body; items 8 to 12 are refinements without a marker.

1. **Partial fixing of the observed-data users** (Outcome; section 1.1, values paragraph). The document says the adopted users run `enable_partial_ar true`. Both user files set `estimation.ambiguity_resolution.enable_partial_ar: false` (QUIN line 138, BADG); the loader default is false (main `src/core/config/GnssConfigLoad.cpp:2026-2027`). True holds only in the simulator recipe (main `scripts/irdu_users.py:44`). Consequence for section 2, row W17: not taken by the observed-data users (partial off: AR:2269-2270 returns float).
2. **Source of the recipe** (section 1.1, values paragraph). The document takes the values from `scripts/irdu_users.py:43-46` and marks the link to the observed-data users as inferred. `irdu_users.py` builds the simulator users `_rap_irdu_u_*` (main `scripts/irdu_users.py:1-30`, USERS line 42). QUIN is a copy of `_rap_nw_obsa24x_147_quin` (its `config_diff_vs_base.txt` line 1), and the record says "partial fixing off" for these users (7fb0737 `docs/refound/NRF_network_ring_fixing_2026-11-14_part8.md:298`). The other values (ratio 5, branch check on, hold 1e-7 m2, release 0.25, upper bound 1e4, mask 20, min_epochs 10, sigma 0.05 m, floor 0.04, IF row, partner refloat) are confirmed at QUIN lines 135-150. Both files also set `network.require_signal_integer_for_ar` true, a value the document does not give.
3. **P2 scenario** (section 4, P2). "Code multipath of 0.3 WL cycle" admits the wrong integer: wrong as worded. A 0.3-cycle error rounds to the right integer; the wrong integer needs a float wide lane about 0.7 cycle from the truth (0.3 from the wrong integer), AR:1406-1407. The mechanism of P2 stands.
4. **Upper-bound reject and its lists** (section 2, row C13; section 4, P3). The document says the upper-bound reject keeps the release and reset lists (AR:688-699). That block cannot run: both success exits already bound the ratio (full set AR:2236; subset IC.cpp:326 via AR:2277). An upper-bound reject is W14 and leaves through C14 (AR:717), so it drops the lists too. With partial off every battery failure of the full set does the same. Row C13 reads correctly as "unreachable: W14 (AR:2236) and the subset gate (IC:326) bound the ratio before success".
5. **Not a defect, F1** (section 4). The document calls the first-stage "partial off" refuted. The first-stage reading was right (QUIN line 138): the ratio part is refuted (1e9 is the float variant), the "partial off" is correct for both users.
6. **The two GPS wrong holds, QUIN and ASCG** (section 4, table of the three wrong holds). "Or a W17 subset" is wrong: with partial off AR:2269-2270 returns float. Each onset was a full-set fix that passed W13-W16 on the floored matrix.
7. **Baseline install** (section 2, row D1-D23; section 3, row baseline DD). "Installed always" is wrong: main `src/core/app/ApplicationBaseline.cpp:1233` reads `if (ar_result.success)`; the fix is installed on success.
8. **The ratio of a failed attempt** (P3, P15; refinement). The failed attempt also loses its ratio (outer result ratio 0, gate 0), so `ConsumerTrace.cpp:29-30` labels every failed WL-constrained attempt "mlambda_fail", ratio failures included.
9. **Process noise and hold variance of the users** (P4; Not checked, second item; refinement). The users set `filter.process_noise.ambiguity_q` 1e-12 and hold_variance 1e-7 m2 (QUIN), so "the users' q was not read" and the second item of Not checked are resolved. The size of P4 remains unconfirmed.
10. **Merged write-back** (section 5.1, step 5; refinement, UNCONFIRMED). The arithmetic is the same (AR:2302-2304 against AR:2356-2360) but the log text differs ("[PAR]"). Only a simulator user of `irdu_users.py` (partial on) can take a partial fix; the observed-data proof run (b) never executes that branch.
11. **Per-band body onto the shared write-back** (section 5.2, step 3; refinement, UNCONFIRMED). The rows differ: one per DD with v = afix - y_dd (AR:957-967) against two per pair (AR:2415-2442). Identical output is not shown.
12. **The AR:692 test** (section 5.2, step 4; refinement). Unreachability is shown by AR:2236 and IC:326 (item 4), not inferred.

### Line numbers that moved

The check cites some ranges a few lines off the body's citations (main unless marked): the MW map RTE:262-265 (body RTE:262-268); the install RTE:279-280 (body RTE:279-281); main `src/core/measurement/MeasurementModelRow.cpp:117-131` (body :117-130); `floatWidelane_` AR:1313-1330 (body AR:1313-1332); the release test's read of the state at AR:1785-1787; the write-back AR:2402-2442 (body AR:2402-2450); `Qab` AR:747-750 (body AR:747-752); `consumed_dd` declared at AR:1760; the narrow-lane `num_fixed` count at AR:2445 (body AR:679, 698 for the per-band count); `epoch_count_` increments only at AR:550 and AR:1021; the partial-off exit AR:2269-2270; the defaults at main `include/filter/AmbiguityResolver.hpp:207, 212, 213, 265, 404`.

### What stands

- P1, P2, P3, P5, P9 and P10 stand as written, line for line. P3 is stronger than stated: with partial off, every full-set battery failure (ratio, upper bound, P_s, per-pair cap) drops that epoch's HOLDREL refloats and code-bias resets, so the case "HOLDREL in the log, refloat lost" is likelier for the QUIN and ASCG runs.
- The subset path is gone from the explanation of the two GPS wrong holds. Each onset was a full-set fix whose pair passed the 0.15 cap, so the fixing user's float sat near the wrong integer while the float user's float sat one cycle away. The open question is why the two users' states diverged before onset (inferred: earlier holds on correlated pairs, P1 and P4), not how a subset was chosen.
- P4 and P7 interact, which the document does not examine: when every candidate is already tightly held, the floored search gives ratios above 1e4 and the re-fix is rejected (P7). That limits the accumulation of P4 to epochs that bring in a fresh float candidate (inferred).
- The claim that the three wrong holds fit these rules stands for UTQI E18-E06 (P2, then P1; QUIN `wl_branch_check` true, signals L1X/L5X, 7fb0737 NRF part15:216; occurrence inferred). For the GPS runs it stays "partly mapped", without the subset branch.
- Decisions put to the author, after correction:
  1. Release and reset lists on a failed attempt (P3): return them on every exit behind a default-off key, or make it the only behaviour once a simulator injection shows it. W14 rejections are covered too.
  2. The quantity an independent release test reads (P1): a shadow float saved before the write-back (owned by `StateManager`), a code-only wide lane from the correction stream's code biases, or the pair's phase residual history.
  3. Galileo E1/E5a wide-lane admission (P2): accept the code test alone, require persistence on code admission, or lower the floor for the branch decision only. Simulator first.
  4. Re-application of the hold (P4): keep the per-epoch re-fix, or hold one persistent constraint per fixed DD and skip re-applying an unchanged integer.
  5. Reference election (P5, P6): hysteresis, one reference per satellite pair across bands, and an event that re-keys or re-expresses held pairs on a reference change.
  6. Ratio upper bound (P7): replace by a test on Q (`qb_pre.sig_lo`), or keep it and stop it rejecting the re-fix of an installed hold. A rejection by the bound also discards the epoch's release lists.
  7. The float reference user (P8): keep it with the resolver on, or build it with `refloat_partner_signal` off or with an ar_mode 0 resolver; and whether the held pair-epoch grade counts rejected re-fixes.
  8. Modes (P12): remove mode 2 or implement it as instantaneous; drop the second update of mode 3.
  9. Off-path code: repair or retire the wide-lane-only, NlWl and baseline DD resolvers; and whether `closed_loop.ar_wl_only` keeps its default true.
  10. Warm-up (P10): reset `epoch_count_` per restarted set, or replace it by a per-arc maturity bar.

### Left unconfirmed

- Whether the users' build (engine rscb6, per the record) has the same resolver code as main 5ddc422; the document reads main.
- The configurations of the users of section 118.1 themselves (the DOY 146 and 147 run folders were not opened). Partial off for them rests on the record (part8:298) and on the lineage of the two files read.
- P6, P13, P14 and sections 1.2, 3 (except the baseline install) and 6 in detail; the `DdPivotManager`, `StateManager` generation and `PppEpochSteps` gap lines.
- The size of P4 and the frequency of P7 on observed data; items 10 and 11 (identical output of the merged write-backs).

The full errata is at `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/UFA_errata.md`.

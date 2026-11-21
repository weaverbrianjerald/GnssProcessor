# UFQ: user integer fixing, quality of the search, the held set and the noise model

**STATUS**

- **id:** UFQ
- **topic:** user-ar
- **status:** closed
- **summary:** Pre-registered 2026-11-20. Three changes to the user's integer fixing, each behind a key that is off by default and identical when off: (3) the search's true ending, its ratio and the held integers reported in the trace and at run end; (2) a search over the not-yet-held ambiguities only, conditioned on the held set; (1) a measurement noise model taken from the user's own data, with the tail handled apart. Judged on the simulator user against truth and on WTZZ00DEU day 129, UTQI day 146 and CHTI day 143. Results 2026-11-20 and 2026-11-21: item 3 built (trace and run end); item 2 built, not recommended alone; item 1 measured harmful on observed data and kept off; items 2 and 1 also tested on the poster's 85 warm starts.
- **dates:** 2026-11-20 (pre-registered, sections 1 to 7; results, sections 8 to 11); 2026-11-21 (results, sections 12 and 13; closed with the managing session's confirmation)
- **knobs / posture:** replay users of the reference streams (see section 2); keys per item, each default off and identical off: item 2 `estimation.ambiguity_resolution.search_unheld_only` (ledger KEEP_OFF), item 1 `filter.measurement_noise.phase_scale_measure` (TRACE) and `filter.measurement_noise.float_phase_scale` (KEEP_OFF); item 3 is a declared change of the trace and the run-end line, without a key.
- **code:** branch `user/fix-quality-2026-11-20` (worktree `D:\GnssProcessorSupport\worktrees\user_fix`, cut from main `8109a64`); files `include/filter/AmbiguityResolver*.hpp`, `src/core/filter/AmbiguityResolver.cpp`, `src/core/realtime/ConsumerTrace.cpp`, `include/realtime/ConsumerTrace.hpp`, the user's row weighting in the real-time engine (item 1: `include/realtime/FloatPhaseScale.hpp`, `src/core/realtime/FloatPhaseScale.cpp`, `MeasurementModel::setPhaseVarianceScale`, the seams in `src/core/realtime/RealTimeEngineEstimate.cpp`).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_ufq_<bed>_<arm>`; tools and tables in `D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_fix\`.
- **source:** the user noise-scaling test of 2026-11-20 (`D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_noise_test\`); card [UFA](../../../refound/UFA_user_fixing_audit_2026-11-18.md) (rules W1 to W18, defects P1 to P15); the user-holds evidence package of 2026-11-19 (decisions 1 to 6); the addendum of 2026-11-19 on a statistic in the data's own scale.

**Outcome.** Three items built; the keys of items 2 and 1 are off by default and identical off. Item 3 puts the search's ending, ratio and held census in the trace: integers are held on 82 to 96 % of epochs, and searches ending on the ratio bound re-confirm held sets. Item 2 searches only the unheld ambiguities: satellites join 0.3 to 0.5 min sooner, but on the poster's 85 starts the error after the first hour is worse in 22, better in 12. Item 1, the receiver's own phase factor on float rows, speeds the simulator's first fix (139 to 71 min) but fails on observed data (wrong holds, overconfident covariance). Nothing is armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

The noise test of 2026-11-20 found that the user's search fails for reasons the trace hides, that the integers an
accepted fix installs stay in the state (held) on most epochs, and that a tighter phase sigma speeds the first fix but
makes the covariance overconfident and raises wrong holds. Three questions follow:

1. What ends each search, with which ratio, and how many pairs are held? (item 3, measure only)
2. Do new satellites join the held set sooner when only the not-yet-held ambiguities are searched? (item 2)
3. Does a noise model taken from the user's own data shorten the first fix and lower the second-hour error without
   more wrong fixes and without an overconfident covariance? (item 1)

## 2. Beds and engines

| Bed | User | Stream (corrections replayed) | Truth |
|---|---|---|---|
| s129 | SZZ000SIM, simulator, day 129 | `D:\GnssProcessorSupport\runs_irm\_rap_irm_day24_s\products.txt` | fixture integers `runs/simnet60_quiet_col_24h/SZZ000SIM/truth_ambiguities.csv` |
| w129 | WTZZ00DEU, observed, day 129 | `runs_irm\_rap_irm_day24_f\products.txt` | float twin of the same stream |
| utqi146 | UTQI, observed, day 146 | `runs_nom\_rap_ad_n146\products.txt` | float twin `_rap_nw_obsadf_146_utqi` |
| chti143 | CHTI, observed, day 143 | `runs_nom\_rap_ad_n143\products.txt` | float twin `_rap_nw_obsadf_143_chti` |

The users are built as in the noise test (`make_arms.py`: base user configuration, resolver at debug level, replayed
stream, run paths). Every comparison is same-engine (the branch build with the key off against the key on) or
same-build (the control engine `gnss_processor_ufq0.exe`, main `8109a64` unmodified, sha256 27ffcd76bac64bca, against
the branch build with every key off). Release only. A 10-epoch smoke precedes every longer run. Before any
recommendation to arm, the 16 test sites as a paired per-site replay.

## 3. Item 3: the ending, the ratio and the held set (trace and run end)

**Change.** No key: an announced change of the trace format, products and every other output identical.

- The WL-constrained search (the user path, `resolveWlConstrained`) records its ending as an enumerated value and the
  ratio it computed, and `resolve` carries both to the caller on a failed search. The release and reset lists of a
  failed search are NOT carried (that is decision 1 of the user-holds package, not ruled): the failed result keeps
  only the diagnostic fields.
- Endings: `no_nl_candidate` (no candidate after the wide-lane and maturity rules), `search_failed` (MLAMBDA failed
  or the distances are invalid), `ratio_low`, `ratio_high` (the upper bound), `success_rate`, `pair_frac` (one pair
  over the fraction limit), with `fixed` on success. The early gates keep their labels (`min_epochs`,
  `sigma_gate(...)`, `no_candidates` for no DD pairs, `disabled`, `wl_guard`). Other search paths keep
  `ratio_battery` and `mlambda_fail`.
- The held census: a satellite pair is held when the DD variance of at least two of its signals is at most ten times
  the hold variance. Evidence for the rule (w129 control of the noise test, 23,794 pair-epochs): the pairs held by the
  noise test's value rule have DD variance at most 1.5e-7 m2 at the 99th percentile (hold variance 1e-7 m2); the
  pairs not held have at least 2.4e-4 m2 at the 0.1st percentile. Any factor from 4 to 1000 classifies every
  not-held pair the same way. A satellite is held when one of its pairs is held; the census keeps the held
  satellites of the previous search epoch and counts the edges: a satellite joins the held set, or leaves it while
  still observed (refloated or released) or by its absence (set, or no pair).
- Trace `logs/closed_loop_trace.csv`: `ar_gate` carries the ending on a failed WL-constrained search (it read
  `mlambda_fail`); five columns are appended: `search_ratio` (the ratio the search computed, also on failure; 0 when
  no search ran), `n_pairs` (satellite pairs of the epoch), `n_held` (held pairs), `held_joined`, `held_left`
  (satellites).
- Run end: one new line with the ending tally, the epochs with any held pair and their share, the held pair-epochs,
  and the joins and leaves by cause.
- Proof: same-build comparison on the four beds (2 h each): `products.txt` and every log identical except
  `closed_loop_trace.csv` (the 18 old columns identical except `ar_gate` on the rows that read `mlambda_fail` or
  `ratio_battery`) and the new run-end line; engine equals offline on w129 (24 h): the engine's ending tally equals
  the tally of the noise test's grader (`grade.py`) on the same run's debug log.
- The loose end of the noise test: on the simulator the float check flags 11 held samples (G22-G24, G13-G24) that
  the truth check calls right; which check is wrong is settled offline (section 8).

## 4. Item 2: the search over the not-yet-held ambiguities

**Key** `estimation.ambiguity_resolution.search_unheld_only`, default false.

- **Why.** Today the held pairs enter every search. At w129, 1,751 of 2,880 searches end on the ratio upper bound
  (1e4), and 1,704 of them have no candidate that is not already held (`held_free_census.py` on the noise test's
  control): the search re-confirms the held set and the bound rejects it. In the 47 others a new candidate sat in the
  search; its own residual was then tiny, so the ratio passed the bound.
- **Design.** After the candidates are formed (stage S4) and the search problem is built (S6, floored covariance),
  the candidates split into the held set H (the census rule of item 3 on both primary signals) and the free set F.
  - F empty: no search, ending `all_held`, nothing is written: the held integers stay as the state holds them.
  - F not empty: the search, the ratio test, the success rate and the per-pair cap run on F conditioned on H:
    z_H the nearest integers of the held entries (they sit within 0.01 cycle), y_F|H = y_F - Q_FH Q_HH^-1 (y_H - z_H),
    Q_F|H = Q_FF - Q_FH Q_HH^-1 Q_HF.
  - The degenerate-covariance guard is kept for the case it was written for (a fix written back with no variance,
    after which the next search sees a near-singular matrix): the ratio upper bound applies to the conditioned search
    only when its smallest eigenvalue is at hold scale (at most ten times the hold variance carried into the search's
    cycles through the candidate's row coefficients); the ending is then `degenerate`. Otherwise the bound does not
    apply, and the searches where it would have fired are counted.
  - An accepted fix writes rows for the free pairs only. Held pairs are not re-applied.
- **Events (invariant 7).** A pair joins the held set when an accepted free search installs it (counter, pair
  identities at debug level); the census of item 3 counts every satellite's join and leave with its cause. Run-end
  line: searches with F empty, searches of F, fixes, pairs installed, the bound's would-have-fired count, degenerate
  refusals.
- **What releases a wrong pair, today and under item 2.** Today nothing tests a held pair against an independent
  quantity: the hold-release test reads the state the fix wrote back and never fired in 105 observed fixing user-days
  (user-holds package, P1); a wrong pair leaves the held set when its arc is refloated (cycle slip, partner refloat,
  a code-bias contradiction), when a satellite sets, or never. Under item 2 the same events release it; item 2 adds
  no release. It does not make a wrong hold more durable: it never re-applies a held pair (today a re-confirmed held
  set is written again at the hold variance on 69 of the 72 accepted searches at w129), and it never re-searches a
  held integer. A wrong integer installed for a new pair is held as long as a wrong integer installed today.
- **Measures**, key on against key off, same engine, 24 h, four beds: time from a satellite's rise (first epoch in
  the user's DD pairs) to its join (median, 90th percentile), held pairs per epoch, held share, wrong fixes (truth on
  s129: fix rows and held samples; float check on the observed beds, with the convergence repair of section 8), 3D
  error by hour band (h1, h2-4, h4-8, h8-16, h16-24).
- **Bar.** Adopted as recommended (not armed) when the rise-to-join median falls on at least three of four beds, no
  bed shows more wrong fixes than its control, and no hour band from hour 2 is worse than the control by more than
  the larger of 2 mm and 10 %.

## 5. Item 1: the noise model taken from the data

A one-page design and one experiment come before any build; both are appended below as their own dated section. The
judging set is fixed now: against the present settings and against the plain 0.5 phase scaling of the noise test,
on the four beds, together: time to first fix, second-hour error, wrong fixes (truth on the simulator, the float check
on observed data), calibration (median of actual 3D error over formal 3D sigma, about 0.9 expected for a consistent
3D covariance), held share. Cautions carried: a scale estimated from the tested residuals grows with the fault it
should detect (scale from other observations, robust or long-memory, frozen during an event); adaptive process noise
was tried and falsified; formal sigmas collapse on the simulator; a fixed floor under a scaled test sets the bar most
of the time (the 0.04 cycle narrow-lane floor is examined).

## 6. Standing invariants, checked at specification

1. **Stiff quantities.** The held ambiguities are the stiff quantity (variance 1e-7 m2, process noise 1e-12 m2/s).
   Their responses today: cycle-slip refloat, partner refloat, code-bias reset; the hold release never fires (P1).
   Item 3 adds none and changes none. Item 2 stops re-applying held rows and adds no stiff quantity. Item 1 changes
   row weights; its tail rule has both edges as events.
2. **Release only.** Every data run uses a Release engine.
3. **Deferrals.** The release lists of a failed search (decision 1) and an independent release test (decision 2)
   are not built here; both are field-inert for items 3 and 2 by construction (item 3 is measure only; item 2 releases
   nothing and re-applies nothing).
4. **Counters.** Each item has a run-end line; "0 searches of F" or "0 rows reweighted" with the key on reads as
   inert wiring.
5. **Datum-invariant grades.** User position error against the truth coordinate, DD integers against truth through
   the per-pair datum constant, held share and rise-to-join time.
6. **Default off, identical when off.** Item 3 is an announced format change (section 3); items 2 and 1 are keyed,
   proven by same-build comparison with the key off.
7. **Events at both edges.** The held set's joins and leaves (item 3 census, item 2 installs) and item 1's tail rule
   (row down-weighted, row restored) are counted with identities.

## 7. Order and checkpoints

Item 3, then item 2, then item 1 (design and one experiment, then the build). A short checkpoint to the managing
session after each. Each item ends in one commit on the branch with its unit cases in a new test file, its key and
ledger row where one is added, the configuration dump regenerated, the off identity shown, the table of the beds
below a dated heading here, and `python scripts/docs_record.py verify` passing. Nothing is merged, pushed or armed.

---

## Results, 2026-11-20

### 8. The loose end of the noise test: the float check is the wrong check

On the simulator (variants 0.5 x 0.5 and phase 0.5) the float check flagged 11 held samples of G22-G24 and G13-G24
at 02:28 to 02:32 that the truth check called right. Tools `g22_g24_check.py`, `g24_float_history.py`,
`pair_constant_via_ref.py` (folder of section 2).

- The two checks grade different objects: the truth check grades the integers of accepted fixes (NL pair lines)
  against a per-pair datum constant taken as the mode of the five runs' fix rows; the float check grades every held
  sample against the float twin, and calls the twin converged when both lanes sit within 0.2 cycle of an integer.
- The held integer of G22-G24 is the same on all 25 held samples (fixed minus truth -109 on L1C, 171 on L2W), and
  every fix row of the five variants carries it.
- The float twin was not converged on the pair: it sat at (-108.0, +172.0) from 01:50 to 02:02 (both signals within
  0.05 cycle), then walked steadily to (-110.0, +170.0) by 02:22 and turned back (-109.86 at 02:26). The walk keeps
  N1 - N2 constant: the wide lane stays integer and the narrow lane passes an integer every cycle of walk, so a
  walking float meets the "both lanes within 0.2 cycle" rule on a large part of its path. The flagged window is the
  twin's pass through (-110, +170). G13-G24 shows the same walk.
- Verdict: the float check is wrong on these samples. Repair (used by this card's grader, `grade_ufq.py`): a held
  sample is graded only where both signals of the twin's DD moved less than 0.1 cycle over the previous 20 epochs.
- Limit: the truth check's datum constant is a mode of the users' own fix rows, so it cannot see a wrong integer that
  every variant shares; nothing here suggests one.

### 9. Item 3: endings, ratio and held census

Commit `bf6f391` (branch `user/fix-quality-2026-11-20`). Engines: control `gnss_processor_ufq0.exe` (main `8109a64`,
sha256[:16] 27ffcd76bac64bca), item 3 `gnss_processor_ufq3.exe` (sha256[:16] 305fef009da29ece). Debug unit run
(filter `Ar*:Ambiguity*:Kalman*:Mlambda*:NlSearchRow*:WlFfr*:WlOnly*:ConsumerTrace*:FixAndHold*`): 338 of 338
passed, 7 new cases.

**Same-build comparison** (24 h replays at the base configurations' logging, `_rap_ufq_<bed>_id0` against
`_id3`, tool `cmp_item3.py`): every file identical after the three masks of the resolver split, except the declared
differences.

| Bed | Verdict | ar_gate transitions (control mlambda_fail to) | Other declared lines |
|---|---|---|---|
| w129 | only declared | ratio_high 1,751, pair_frac 520, ratio_low 294, no_nl_candidate 5 | run-end line |
| s129 | only declared | ratio_high 2,031, pair_frac 464, ratio_low 46 | run-end line |
| chti143 | only declared | ratio_high 2,608, pair_frac 37 | run-end line; 287 HELDSET lines (the base logs the resolver at debug); the rotation cut of the size-rotated resolver logs moves, their common span (92,077 lines) identical |
| utqi146 | only declared | ratio_high 1,842, pair_frac 577, ratio_low 186 | run-end line; 390 HELDSET lines (resolver at debug in the base); rotation cut moves, common span (115,690 lines) identical |

The 18 old columns of `closed_loop_trace.csv` are identical on every row except `ar_gate` on failed searches; the
products (none written by a replay) and every other log are identical.

**Engine equals offline** (debug runs `_rap_ufq_{w129,s129}_dbg3`, tool `engine_vs_offline.py`): the engine's
`ar_gate` equals the noise test's grader on the same run's log on 2,880 of 2,880 and 2,757 of 2,757 search epochs.

| Bed | Endings (run-end line) | Census epochs with a held pair | Held pair-epochs | Joins | Leaves observed / absent |
|---|---|---|---|---|---|
| w129 | min_epochs 10, sigma_gate 127, no_candidates 101, no_nl_candidate 5, ratio_low 294, ratio_high 1,751, pair_frac 520, fixed 72 | 2,364 of 2,870 (82.4 %) | 16,454 of 23,794 | 133 | 50 / 73 |
| s129 | min_epochs 10, sigma_gate 137, ratio_low 46, ratio_high 2,031, pair_frac 464, fixed 69 | 2,444 of 2,747 (89.0 %) | 19,089 of 23,139 | 106 | 24 / 72 |
| chti143 | min_epochs 10, sigma_gate 67, no_candidates 56, ratio_high 2,608, pair_frac 37, fixed 102 | 2,746 of 2,870 (95.7 %) | 22,962 of 25,433 | 148 | 40 / 99 |
| utqi146 | min_epochs 10, sigma_gate 55, no_candidates 72, ratio_low 186, ratio_high 1,842, pair_frac 577, fixed 138 | 2,446 of 2,870 (85.2 %) | 22,678 of 32,593 | 202 | 78 / 110 |

The census (variance rule) counts 2,364 and 2,444 held epochs where the noise test's value rule counted 2,370 and
2,446: the value rule also takes a float that sits by chance within 0.01 cycle of an integer on both signals.

Reading: the held share and the endings the noise test reconstructed offline are now in the trace and the run-end
line. Every search that ends on the ratio upper bound carries held integers (0 without a held pair on each of the
four beds: 1,751, 2,031, 2,608 and 1,842 such searches), and on every bed the bound is the most frequent ending.

### 10. Item 2: the search over the not-yet-held ambiguities

Key `estimation.ambiguity_resolution.search_unheld_only` (default false, ledger row UNVALIDATED). Engine
`gnss_processor_ufq2b.exe` (sha256[:16] 611a50d899fda331). Debug unit run (the item 3 filter plus `ConfigDump*`):
347 of 347 passed, 5 new cases (`tests/test_ar_unheld_search.cpp`).

**A defect of the first build, repaired before grading.** The resolver applies the ratio upper bound a second time
to every accepted WL-constrained fix (`resolveCore_`). Under the key that second check refused the searches the
design exists for (a new pair whose float sits on its integer, ratio above 1e4, covariance not degenerate): 54, 42,
62 and 110 accepted searches on w129, s129, utqi146 and chti143 (runs `_rap_ufq_<bed>_u1`, engine
`gnss_processor_ufq2.exe`). The second check now reads the key (`ratioBoundRejects_`, unchanged with the key off,
unit case `TheResolversOuterBoundLeavesTheUnheldSearchsVerdictStanding`). The graded runs are `_u0b` and `_u1b`.

**Off identity.** Key off (`_u0b`) against the item-3 engine on the same configuration (`_dbg3`), resolver at debug
level: IDENTICAL on all four beds (33 to 35 files, the three masks of the resolver split only).

**Key off against key on** (24 h replays, resolver at debug level; grader `grade_ufq.py` with the float check's
convergence repair of section 8 and, on the simulator, the datum constant pooled over every simulator run of the
day and called wrong only against a clear mode).

| Bed | Key | Rise to join, median / p90 (min) | Arcs never joined | Held pairs per epoch | Held share | 3D error h1 / h2-4 / h4-8 / h8-16 / h16-24 (mm) | Calibration | Wrong |
|---|---|---|---|---|---|---|---|---|
| w129 | off | 12.5 / 52.5 | 9 of 107 | 6.14 | 0.821 | 92 / 21 / 11 / 9 / 24 | 0.74 | 0 (float check) |
| w129 | on | 12.2 / 52.5 | 9 of 107 | 6.19 | 0.822 | 92 / 21 / 11 / 9 / 24 | 0.73 | 0 |
| s129 | off | 8.5 / 68.0 | 10 of 98 | 7.70 | 0.886 | 64 / 25 / 19 / 24 / 27 | 0.97 | 0 of 578 fix rows (truth) |
| s129 | on | 8.2 / 65.5 | 10 of 98 | 7.72 | 0.888 | 64 / 25 / 19 / 24 / 27 | 0.97 | 0 of 78 |
| utqi146 | off | 16.5 / 62.0 | 13 of 152 | 8.78 | 0.849 | 62 / 34 / 35 / 30 / 27 | (20.1) | 47 samples, E18-E06 |
| utqi146 | on | 16.0 / 62.0 | 13 of 152 | 8.96 | 0.849 | 62 / 34 / 35 / 30 / 26 | (20.3) | 0 |
| chti143 | off | 7.8 / 24.5 | 3 of 111 | 8.36 | 0.953 | 11 / 6 / 3 / 4 / 4 | 3.07 | 0 |
| chti143 | on | 7.5 / 24.5 | 3 of 111 | 8.39 | 0.953 | 11 / 6 / 3 / 4 / 4 | 2.89 | 0 |

First fix unchanged on every bed (100.5, 139.0, 148.0 and 66.5 min: before the first fix nothing is held, so the
two searches are the same). The simulator's key-on fix rows are fewer because a held pair is no longer searched and
printed again. UTQI and CHTI calibrations measure the standing error of the reference coordinate (section 2 of the
noise test), not the covariance.

Run-end counters, key on (searches; with every candidate held; of the free set; fixed; pairs installed; the bound
would have refused; degenerate): w129 2,637; 1,750; 887; 71; 111; 12; 0. s129 2,610; 2,036; 574; 69; 78; 6; 0.
utqi146 2,743; 1,923; 820; 114; 172; 10; 0. chti143 2,747; 2,620; 127; 94; 117; 15; 0.

**Reading.** As the census of item 3 predicted (47 of the 1,751 bound endings at w129 had a candidate not yet held),
the gain is small. Rise to join falls by 0.3 to 0.5 min on the four beds (one epoch is 0.5 min), held pairs per
epoch rise by 0.02 to 0.18, the error by hour and the calibration do not move. What does change is structural:
1,750 to 2,620 searches a day that only re-confirmed the held set are not run, a held pair is never written again
(the re-application of a hold, defect P4 of card UFA, is gone under the key), and the bound no longer refuses a new
pair for sitting on its integer (6 to 15 such searches a day, none degenerate). At UTQI the day's wrong E18-E06 hold
(the code-only wide-lane admission of the user-holds package) is not installed: the free search refused E18's set on
the per-pair cap (worst fraction 0.154 to 0.159 against 0.15) from 09:33 to 09:38, and E18 joined at 09:39 once its
wide lane was decided by the phase (sigma 0.002 cycle against 0.11 at 09:33). That is one event decided by a
marginal cap, not a rule.

**Against the bar of section 4.** Met on its letter (the median falls on four beds, no bed has more wrong fixes,
no hour band is worse), but the size is within one epoch. Recommended state: UNVALIDATED, not armed alone; it
removes the re-confirmation searches and the hold re-application, which makes it a natural part of the hold-honesty
work (decisions 1 and 2 of the user-holds package) rather than a fix-speed change. What releases a wrong pair is
unchanged (section 4).

### 11. Item 1: the user's own residuals, one experiment, and the design

**Measured** (control engine, measure only: `estimation.enable_variance_factor` and `logging.enable_residual_trace`
on, `closed_loop_trace.csv` byte-identical to the control on all four beds; tool `noise_classes.py`, runs
`_rap_ufq_<bed>_vf`). A row's class is (code or phase, constellation, band). The bulk factor k is the square root of
the median over epochs of the class's sum of v^2/R over its sum of redundancy numbers (E[v_i^2/R_ii] = r_i under
the model, also with the user's between-satellite R); rho is the lag-one autocorrelation (30 s) of each row's
standardized residuals, median over rows; the tail is the share of |w| beyond 3.29.

| Bed | phase k | phase rho | code k (per band) | code rho | tail beyond 3.29 |
|---|---|---|---|---|---|
| WTZZ, day 129 | 0.40 to 0.43 | 0.62 (GPS), 0.73 (Galileo) | G1 0.48, G2 0.36, E1 0.35, E5a 0.19 | 0.08 to 0.41 | at most 0.03 % |
| simulator | 0.55 to 0.62 | 0.80 to 0.82 | 0.90 to 0.92 | 0.00 | at most 0.14 % |
| UTQI, day 146 | 1.04 to 1.05 | 0.74 (Galileo), 0.79 (GPS) | 0.98 to 1.15 | 0.39 to 0.66 | 0.45 to 1.1 % |
| CHTI, day 143 | 0.53 (GPS) to 0.60 (Galileo) | 0.73 to 0.79 | G1 0.48, G2 0.31, E1 0.29, E5a 0.30 | 0.18 to 0.69 | at most 0.16 % |

1. The scale of the white part of the noise belongs to the receiver, not to the network: phase 0.40 to 1.05, code
   0.19 to 1.15. At UTQI the borrowed 0.5 halves a sigma the data put at 1.05.
2. The phase residuals are serially correlated (correlation times of one to two and a half minutes). Counted as
   white, N epochs carry about N (1 - rho)/(1 + rho) independent samples: the formal ambiguity sigma is too small by
   sqrt((1 + rho)/(1 - rho)) = 2.1 to 3.2, the record's finding from the other side (card ARU: the formal
   narrow-lane sigma a half to a third of the actual miss after 40 min).
3. The present phase sigma equals k sqrt((1 + rho)/(1 - rho)) at WTZZ (0.95), which is why its covariance reads
   calibrated there.
4. The user's tail is light (pooled over median variance factor 1.2 to 1.5; the network's is 15).

**The experiment** (config only, static values per receiver from the table, item-3 engine, 24 h, the four beds,
resolver at debug level; arms `_rap_ufq_<bed>_<arm>`): the control (`dbg3`); the noise test's phase x 0.5 (`n1h`);
the receiver's own bulk phase factor, code unchanged (`n1p`: phase 0.83, 1.75, 2.09, 1.13 mm for WTZZ, simulator,
UTQI, CHTI); the bulk factors of code and phase (`n1b`); the effective factors k sqrt((1 + rho)/(1 - rho)) of code
and phase (`n1d`: code 0.125, 0.272, 0.590, 0.176 m; phase 1.90, 5.47, 5.77, 3.07 mm).

| Bed | Arm | First fix (min) | h1 (mm) | h2-4 / h4-8 / h8-16 / h16-24 (mm) | Calibration | Held share | Wrong |
|---|---|---|---|---|---|---|---|
| w129 | control | 100.5 | 92 | 21 / 11 / 9 / 24 | 0.74 | 0.821 | 0 |
| w129 | phase x 0.5 | 91.5 | 35 | 26 / 14 / 10 / 22 | 1.59 | 0.828 | 0 |
| w129 | own phase k | 90.5 | 35 | 27 / 15 / 11 / 22 | 1.98 | 0.829 | 0 |
| w129 | bulk code and phase | 102.0 | 104 | 25 / 14 / 11 / 22 | 1.94 | 0.784 | 0 |
| w129 | effective | 115.0 | 167 | 24 / 9 / 10 / 22 | 0.78 | 0.813 | 0 |
| s129 | control | 139.0 | 64 | 25 / 19 / 24 / 27 | 0.97 | 0.886 | 0 of 578 |
| s129 | phase x 0.5 | 71.5 | 42 | 19 / 24 / 23 / 29 | 1.90 | 0.918 | 0 of 822 |
| s129 | own phase k | 71.5 | 40 | 19 / 22 / 23 / 29 | 1.61 | 0.918 | 0 of 739 |
| s129 | bulk code and phase | 71.5 | 40 | 19 / 22 / 23 / 29 | 1.62 | 0.916 | 0 of 722 |
| s129 | effective | 203.5 | 66 | 26 / 19 / 24 / 26 | 0.53 | 0.852 | 2 of 529, 6 undecided |
| utqi146 | control | 148.0 | 62 | 34 / 35 / 30 / 27 | (20.1) | 0.849 | 47 samples |
| utqi146 | phase x 0.5 | 102.5 | 63 | 35 / 39 / 29 / 23 | (34.7) | 0.876 | 47 |
| utqi146 | own phase k | 148.0 | 61 | 34 / 35 / 30 / 27 | (19.8) | 0.849 | 47 |
| utqi146 | bulk code and phase | 148.0 | 62 | 34 / 35 / 30 / 27 | (19.8) | 0.849 | 47 |
| utqi146 | effective | 98.0 | 55 | 40 / 33 / 28 / 26 | (7.2) | 0.878 | 0 |
| chti143 | control | 66.5 | 11 | 6 / 3 / 4 / 4 | (3.1) | 0.953 | 0 |
| chti143 | phase x 0.5 | 68.0 | 12 | 8 / 4 / 4 / 4 | (6.8) | 0.952 | 0 |
| chti143 | own phase k | 68.0 | 12 | 8 / 4 / 4 / 4 | (5.8) | 0.952 | 0 |
| chti143 | bulk code and phase | 107.5 | 70 | 10 / 5 / 4 / 4 | (5.8) | 0.925 | 0 |
| chti143 | effective | 119.0 | 94 | 7 / 4 / 3 / 3 | (1.7) | 0.917 | 0 |

Wrong: simulator fix rows against truth (section 10's grader: a mode of one or two rows had called five right fixes
of the effective arm wrong); observed data, held samples against the float twin where it is still. Bracketed
calibrations measure the standing error of the reference coordinate as much as the covariance.

**What the experiment settles.**
- The receiver's own phase factor gives the borrowed 0.5's gains where the receiver is quiet (first fix 100.5 to
  90.5 min and second hour 92 to 35 mm at WTZZ; 139.0 to 71.5 min and 64 to 40 mm on the simulator; no wrong fix)
  and leaves UTQI as it is, which a borrowed factor cannot. Its cost is the borrowed factor's: hours 2 to 8 a few
  mm worse on the observed beds and a covariance overconfident by 1.6 to 2.0 from hour 2.
- A code factor from post-fit residuals is refused: it worsens the first two hours at WTZZ and CHTI (h1 104 and
  70 mm). The code errors that matter to the float are absorbed by the bias and clock states and do not show in the
  code's post-fit residuals.
- The correlation factor in the filter is refused: calibrated or pessimistic (0.78, 0.53), but the first fix comes
  later on three beds (115, 204 and 119 min) and the second hour is worse there. It does remove UTQI's wrong hold
  and halves its first hour, so the correlation is real; the filter is the wrong place to charge it everywhere.

**Design** (keys `filter.measurement_noise.phase_scale_measure`, measure only, and
`filter.measurement_noise.float_phase_scale`, both default off and identical off):
- **The factor, from the receiver's own data.** Per epoch and phase class, sum v^2/R_nominal over sum r of the rows
  with redundancy at least 0.1 and not flagged by the tail rule; k_c^2 is the median of the per-epoch values over a
  trailing hour, formed from at least 40 epochs (factor 1 until then); frozen while the tail rule flags more than
  a stated share of the class's rows. No fixed floor under k. The measure-only key logs k_c, the epochs behind it,
  and the frozen epochs, and changes nothing.
- **Applied to the phase rows of a satellite whose ambiguities are float.** A float ambiguity absorbs the slow part
  of its own satellite's phase error, so the white part (k_c^2 sigma^2) is the weight that fits that row; a held
  ambiguity cannot, so once the satellite joins the held set (the census of item 3) its rows return to the present
  weight, which equals the effective one at WTZZ and is the one that keeps the covariance calibrated. Each
  satellite's switch is an event at both edges (to the bulk weight when it leaves the held set or first appears, to
  the present weight when it joins), counted, with the satellite at debug level.
- **The tail.** The factor never tightens a row whose standardized innovation exceeds z k_c (z = 3.29, 0.1 %
  two-sided): that row keeps its present weight for the epoch (an event, counted). No row is excluded.
- **Unchanged:** the code weights, the 0.04 cycle floor of the narrow-lane search (it is the slow-error allowance of
  the integer decision during the float phase; with the factor applied only to float rows it keeps that role), the
  process noise.
- **Expected,** to be measured: first fix and second hour as the receiver's own phase factor; hours 2 to 24 and the
  calibration as the control, because the held rows, which carry the position from the first fix on, keep the
  present weight; UTQI unchanged.

## Results, 2026-11-21

### 12. The poster's experiment: items 2 and 1 on 85 warm starts

At the managing session's request, before building the design of section 11: the AGU refresh experiment (17
receivers on 23 to 27 May 2026, moving, joining at 03:00 a network 3 h old, 3 h each, fixing and never-fixing users;
reference `refresh_arm1120`; driver `presentation/agu26/figures/v3_integer/tools/refresh_agu.py`).

- **Engine.** The branch with main `019ed34` merged (`86af1c6`), `gnss_processor_ufqp.exe`, sha256[:16]
  3db51558cf096e7a. With no user key it reproduces four `arm1120` user runs (`_rap_agu3_ufqrep*`, fixing and
  never-fixing, days 143, 145 and 147): every file identical except item 3's declared trace columns, `ar_gate`
  endings, run-end line and census debug lines (`cmp_item3.py`).
- **Networks.** Both variants replay `arm1120`'s network products (copied into `_rap_agu3_rufq{1,2}_n<doy>`, a note
  in each folder): the branch changes nothing on the network path, so the comparison isolates the user keys. The
  `arm1120` re-grade (`regrade_arm1120`, this card's folder; the reference folder has no `summary.json` and was not
  written to) reproduces its numbers: 19.75 and 39.5 min, 2.215 cm, 0 wrong of 112,754 held.
- **Variants.** `ufq2`: `estimation.ambiguity_resolution.search_unheld_only=true` (key file
  `ufq_user_keys_unheld.txt`). `ufq1`: the same plus each receiver's phase factor in the tested form (one phase sigma
  per run, 2 mm x k), with k the median of the same receiver's factors on the other four days (85 measure-only runs
  `_rap_agu3_ufqm_<doy>_<site>`; `poster_factors.md`). The factors are a property of the receiver: WTZZ 0.38 to 0.49
  over the five days, STPM 0.60 to 0.63, UTQI 0.86 to 1.01, ARHT 0.31 to 0.39; POHN (0.70 to 1.61) and CIBG (0.67 to
  1.18) each have one outlying day.

| Fixing users, 85 starts | arm1120 | ufq2 (unheld search) | ufq1 (unheld search + own phase factor) |
|---|---|---|---|
| Time to 10 cm, median (min) | 19.75 | 19.75 | 20.5 |
| Time to 5 cm, median (min) | 39.5 | 39.5 | 37.25 |
| 3D error, minutes 60 to 180, median of starts (cm) | 2.215 | 2.275 | 2.475 |
| Paired, minutes 60 to 180: better / worse / equal starts | - | 12 / 22 / 51 | 34 / 49 / 2 |
| Held share, median (whole run / after minute 60, %) | 65.8 / 95.0 | 65.8 / 95.0 | 66.7 / 98.3 |
| Wrong holds, G L1C/L2W and E L1X/L5X (float check) | 0 | 0 | 0 |
| Wrong holds, E L1C/L5Q (held samples) | 0 of 22,832 | 0 of 22,934 | 0 of 24,088 |
| Calibration from minute 60, median of starts (quartiles) | 1.23 (0.95 to 1.71) | 1.24 (0.96 to 1.72) | 2.18 (1.64 to 2.99) |

Ten-minute medians of the fixing users (cm, minutes 0 to 180): arm1120 25.01, 10.46, 6.68, 4.88, 3.77, 3.38, 2.71,
2.39, 2.34, 2.31, 2.40, 2.35, 2.06, 2.21, 2.21, 2.10, 2.28, 2.23; ufq2 identical to within 0.07 in every bin; ufq1
24.83, 10.62, 6.50, 5.38, 3.40, 3.16, 2.70, 2.25, 2.40, 2.41, 2.40, 2.40, 2.38, 2.26, 2.19, 2.24, 2.27, 2.24. The
never-fixing users and standard PPP do not move beyond 0.75 min and 0.01 cm.

**Against the managing session's rule** (times to 10 and 5 cm not worse, the error after the first hour better in
more starts than worse, no wrong integer): `ufq2` fails on the paired error (12 better, 22 worse); `ufq1` fails on
time to 10 cm (+0.75 min) and on the paired error (34 better, 49 worse), and its covariance is overconfident in 83 of
85 starts. Neither is recommended for the poster's users. Reading: the static factor on every row makes the error
after the first hour worse here as on the four beds (section 11), while its gain is in the window when the user's
own ambiguities are still float (time to 5 cm 2.25 min sooner, minutes 40 to 60 better); the design of section 11
applies the factor exactly there and keeps the configured weight on held rows, so it is built next and tested on the
same 85 starts.

### 13. Item 1: the design of section 11 on the four beds and the poster's 85 starts

Keys `filter.measurement_noise.phase_scale_measure` (measure only) and `filter.measurement_noise.float_phase_scale`
(the factor applied), default false. Code: `include/realtime/FloatPhaseScale.hpp` and its `.cpp` (the estimator and
the per-satellite map), the engine's seams `applyFloatPhaseScale_` (before the innovation screen) and
`observeFloatPhaseScale_` (after the committed update), `MeasurementModel::setPhaseVarianceScale` (the undifferenced
phase variances scaled before the between-satellite transform).

- **Engine.** The branch at `2081e0a` (the combined branch with items 3 and 2) plus item 1,
  `gnss_processor_ufqf.exe`, sha256[:16] 0e30163ad7ec15cc; this commit's tree differs from the built one in code
  comments only. Smoke (300 s) exit 0. Debug unit run 389 of 389 passed (suites `Ar*`, `Ambiguity*`, `Kalman*`,
  `Mlambda*`, `NlSearchRow*`, `WlFfr*`, `WlOnly*`, `ConsumerTrace*`, `FixAndHold*`, `ConfigDump*`, `*ConfigMember*`,
  `FloatPhaseScale*`, `Measurement*`, `Bssd*`), with the new file `tests/test_float_phase_scale.cpp`.
- **Off identity.** Keys off against the merged engine without item 1 (`gnss_processor_ufqp.exe`, 3db51558cf096e7a),
  four beds, 24 h: identical (35, 33, 35 and 35 files, none differ; `identity_item1b.txt`). Measure only against keys
  off: the trace, the per-satellite trace and the resolver logs identical on the four beds; the configuration line
  and the run-end line are the only additions (wall-clock lines aside).

**The factor the key measures** (run-end line `Float phase scale`, runs `_rap_ufq_<bed>_fm` and `_f1`; first applied:
the first row of the per-satellite trace that differs from keys off):

| Bed | Measure only, Galileo / GPS | Applied run, Galileo / GPS | Rows used / mixed (applied) | First applied (min) | Satellites to the factor / back |
|---|---|---|---|---|---|
| w129 | 0.41 / 0.44 | 0.32 / 0.35 | 20,921 / 40,988 | 77.5 | 386 / 309 |
| s129 | 0.55 / 0.60 | 0.60 / 0.33 | 13,247 / 50,081 | 38.0 | 274 / 207 |
| utqi146 | 1.08 / 1.14 | 1.04 / 0.85 | 31,638 / 56,744 | 60.5 | 438 / 324 |
| chti143 | 0.54 / 0.46 | 0.37 / 0.51 | 15,281 / 56,068 | 55.0 | 349 / 272 |

The measure-only factors agree with the offline values of section 11 within 0.09.

**Four beds** (24 h, grader of sections 10 and 11; runs `_rap_ufq_<bed>_f0` and `_f1`):

| Bed | Arm | First fix (min) | h1 (mm) | h2-4 / h4-8 / h8-16 / h16-24 (mm) | Calibration | Held share | Wrong |
|---|---|---|---|---|---|---|---|
| w129 | keys off | 100.5 | 92 | 21 / 11 / 9 / 24 | 0.74 | 0.821 | 0 |
| w129 | design | 154.0 | 99 | 24 / 17 / 14 / 26 | 1.89 | 0.830 | 0 |
| s129 | keys off | 139.0 | 64 | 25 / 19 / 24 / 27 | 0.97 | 0.886 | 0 of 578 |
| s129 | design | 71.0 | 31 | 17 / 21 / 25 / 26 | 1.36 | 0.942 | 0 of 698 |
| utqi146 | keys off | 148.0 | 62 | 34 / 35 / 30 / 27 | (20.1) | 0.849 | 47 samples |
| utqi146 | design | 102.5 | 61 | 39 / 36 / 29 / 24 | (20.4) | 0.877 | 47 |
| chti143 | keys off | 66.5 | 11 | 6 / 3 / 4 / 4 | (3.1) | 0.953 | 0 |
| chti143 | design | 66.5 | 16 | 9 / 3 / 3 / 4 | (3.6) | 0.953 | 0 |

**The poster's 85 starts** (the design alone, key file `ufq_user_keys_floatphase.txt`; `arm1120`'s network products
replayed as in section 12; `refresh_ufq3`):

| Fixing users, 85 starts | arm1120 | ufq3 (the design) |
|---|---|---|
| Time to 10 cm, median (min) | 19.75 | 21.0 |
| Time to 5 cm, median (min) | 39.5 | 35.0 |
| 3D error, minutes 60 to 180, median of starts (cm) | 2.215 | 2.13 |
| Paired, minutes 60 to 180: better / worse / equal starts | - | 40 / 44 / 1 |
| Held share, median (whole run / after minute 60, %) | 65.8 / 95.0 | 75.6 / 100.0 |
| Wrong holds, G L1C/L2W and E L1X/L5X (float check) | 0 | 59 |
| Wrong holds, E L1C/L5Q (held samples) | 0 of 22,832 | 34 of 25,848 |
| Calibration from minute 60, median of starts (quartiles) | 1.23 (0.95 to 1.71) | 1.74 (1.40 to 2.63) |

The wrong holds sit at three receivers on two days: BAKE day 145, G04-G31 one narrow-lane cycle off for 48 epochs and
G04-G03 for 1, and on the third pair E30-E28 two cycles off for 21 epochs and five pairs with E19 one cycle off for 1
to 3 epochs; CHTI day 147, E36-E30 (6 epochs), and WTZZ day 147, E13-E31 (4), one cycle off. BAKE day 145 is also the
largest paired loss (+54.5 mm). The calibration is higher in 77 of 85 starts. Ten-minute medians of the fixing users
(cm, minutes 0 to 180): 25.01, 10.46, 6.52, 4.50, 3.47, 3.16, 2.85, 2.43, 2.31, 2.54, 2.56, 2.38, 2.15, 2.14, 2.27,
2.10, 2.14, 2.25 (arm1120's in section 12). The never-fixing users carry the factor on every phase row once it
forms: time to 10 and 5 cm 21.25 and 41.5 min (arm1120 19.75 and 40.0), paired after the first hour 44 better, 38
worse, 3 equal. Standard PPP does not move.

**Against the managing session's rule:** the design fails on time to 10 cm (+1.25 min), on the paired error (40
better, 44 worse) and on the wrong integers (59 on the two main pairs, 34 on the third; arm1120 none), and its
covariance is more overconfident. Not recommended: `float_phase_scale` stays off (ledger KEEP_OFF);
`phase_scale_measure` is kept as an instrument (ledger TRACE).

**What the runs show.**
1. **The factor forms late.** It needs 40 epochs with at least three phase rows of redundancy 0.1 or more, and a
   float ambiguity leaves its rows that redundancy only once its variance is near the phase noise: the applied run
   equals keys off, value for value, until minute 77.5 at WTZZ, 38 on the simulator, 60.5 at UTQI and 55 at CHTI.
   The static factor's gain of section 11 lies before that (WTZZ, minutes 60 to 70: 220 mm with the receiver's own
   k against 893 mm).
2. **Switched on during convergence, it costs WTZZ the next hour.** Minutes 90 to 140: 90, 80, 64, 57 and 40 mm
   against 41, 18, 16, 20 and 9 with keys off; in minutes 0 to 180 the searches end on `ratio_low` 73 times and on
   `pair_frac` 55 times (keys off 0 and 2), and the first fix moves from 100.5 to 154 min.
3. **The factor read from the rows it tightens runs low.** In the applied runs only rows whose two satellites carry
   the same factor enter the estimate (40,988 rows mixed against 20,921 used at WTZZ, where the measure-only run used
   65,908), and the end-of-day factor is below the measure-only one in six of the eight (bed, constellation) cases,
   by up to 0.27 (simulator GPS, 0.33 against 0.60). A float ambiguity follows the slow error more closely the more
   its rows are tightened, so the residuals that set the factor shrink with it: a feedback toward overconfidence,
   consistent with the correlated residuals of section 11.
4. **Held rows at the configured weight do not keep the covariance calibrated.** Float satellites are present at
   every epoch (risers and refloated arcs: 19,294 scaled satellite-epochs at WTZZ, 6.7 per epoch), and their
   tightened rows carry the position with the held ones: calibration from hour 2 1.89 against 0.74 at WTZZ, 1.36
   against 0.97 on the simulator, 1.74 against 1.23 on the poster.

**Reading.** On the simulator, whose corrections are exact and whose receiver is quiet, the factor on float rows
gives the static factor's first fix (71 min against 139) without its cost in hours 2 to 8, and no wrong fix against
truth; on the observed beds and the poster's starts it does not. Every form tried that tightens the phase rows (the
borrowed 0.5, the receiver's own k on every row, the float-only design) speeds the float where the receiver is quiet
and leaves the covariance overconfident; the form that charges the time correlation (section 11's effective factor)
is calibrated and slower. The correlation (rho 0.62 to 0.82 at 30 s) is the quantity item 1 leaves open: a filter
that models it (a first-order Gauss-Markov phase error per satellite, or decorrelated epoch differences) is a new
design, outside this card's three items.

## 2026-11-21: the user noise-scaling test of 2026-11-20 (filed after the fact)

Filed on 2026-11-21 from `D:/GnssProcessorSupport/gates/day_program_2026-11-20/user_noise_test/user_noise_test_note.md` (the test that preceded this card; tables `table.txt`, grader `grade.py` with the first truth check of the user's own integers, in the same folder). The text below is the note, unchanged apart from heading levels.

### User noise-scaling test (2026-11-20)

Question: is the user's low fix rate a consequence of a pessimistic measurement noise, and would scaling the user's
noise towards a unit weight of 1 raise it without raising wrong holds?

Engine: `gnss_processor_unoise.exe` (copy of `gnss_processor_int1120.exe`, sha256 de17cb83802a20c2), Release. All
users are replay users (24 h, 30 s) on a fixed corrections file; only `filter.measurement_noise.{code,phase}_std_m`
changes between variants (`make_arms.py`); the resolver logs at debug level so `stdout.txt` holds every resolver
line. Grader: `grade.py`; full per-run records `grade_<bed>.txt`; table `table.txt`.

Beds: w129 = WTZZ00DEU on `_rap_irm_day24_f`; s129 = SZZ000SIM on `_rap_irm_day24_s`; utqi146 = UTQI on
`_rap_ad_n146`; chti143 = CHTI on `_rap_ad_n143`. Runs `D:/GnssProcessorSupport/runs_nom/_rap_unoise_<bed>_<var>`.

#### 1. What ends a search (code, int1119)

The user path is `resolve()` then `resolveWlConstrained()` (ar_mode 1, wl_from_float). When the WL-constrained call
fails, `resolve()` returns its own empty result (AmbiguityResolver.cpp:683), not the WL-constrained one, so the
ratio is 0 and the trace labels the epoch `mlambda_fail`. Every ending inside the WL-constrained resolver is folded
into that label, ratio failures included. The endings, with the numbers they compare against:

| ending | test | threshold |
|---|---|---|
| min_epochs | epoch count | 10 |
| sigma_gate | formal 3D position sigma | 0.05 m |
| no_dd_pairs | no DD pair passes the AR elevation mask (20 deg) and pairing | - |
| wlc_no_candidate | no NL candidate after the wide-lane and maturity rules | - |
| mlambda_failed / invalid distances | search failure | - |
| ratio_below_5 | s2/s1 | 5 |
| ratio_above_1e4 | s2/s1 ("degenerate covariance") | 1e4 |
| p_s_below_min | bootstrapped success rate | min_success_rate |
| per_pair_frac | worst pair's distance to its integer | nl_max_per_pair_frac |

Partial fixing is off. Fix-and-hold proper (`holdAmbiguities`) only runs at ar_mode 3, but an accepted fix is a
pseudo-observation update with R = hold_variance = 1e-7 m2 on zero-noise ambiguities, so the integers stay in the
state (held) until a hold release. A held epoch is counted when a satellite pair has both signals' single difference
within 0.01 cycle of a whole number before the search.

w129 control (reproduces the reference: 72 fixed, 101 no pairs, 10 min_epochs): ratio_above_1e4 1751,
per_pair_frac 520, ratio_below_5 294, sigma_gate 127, no_dd_pairs 101, fixed 72, min_epochs 10,
wlc_no_candidate 5. All 1751 ratio_above_1e4 endings occur while integers are held: the held pairs, collapsed to
0.3 mm, enter the next search and push the ratio past 1e4.

#### 2. Held against new fixes (controls)

| bed | held epochs | new fix accepted | first fix (min) | hold releases |
|---|---|---|---|---|
| w129 | 2370 of 2880 (82 %) | 72 | 100 | 0 |
| s129 | 2446 of 2757 (89 %) | 69 | 139 | 0 |
| utqi146 | 2452 of 2880 (85 %) | 138 | 148 | 0 |
| chti143 | 95 % of 2880 | 102 | 67 | 0 |

Nearly every accepted fix lands in an epoch that already holds integers (w129: 69 of 72): a new fix is mostly a
re-confirmation plus newly risen satellites. Before the first fix, the epochs end on ratio_below_5, per_pair_frac,
sigma_gate and no_dd_pairs. The 2.5 % "fix rate" counts accepted searches, not epochs with fixed integers. The user
holds integers in 82 to 95 % of epochs.

#### 3. Scaling (code x phase scale; error medians in mm by hour band; cal = median of 3D error / formal 3D sigma, hours 2 to 24)

| bed | var | accepted | held % | first fix min | h1 | h2-4 | h4-8 | h8-16 | h16-24 | cal | wrong held samples (float check) | truth (fix rows wrong) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w129 | 1 x 1 | 72 | 82 | 100 | 92 | 22 | 12 | 9 | 23 | 0.74 | 0 | - |
| w129 | .5 x .5 | 154 | 81 | 100 | 90 | 24 | 15 | 10 | 21 | 1.60 | 0 | - |
| w129 | .25 x .25 | 393 | 75 | 152 | 93 | 29 | 17 | 12 | 23 | 3.22 | 0 | - |
| w129 | 1 x .5 | 148 | 83 | 92 | 35 | 24 | 15 | 10 | 22 | 1.59 | 0 | - |
| w129 | 1 x .25 | 368 | 81 | 79 | 26 | 27 | 18 | 14 | 24 | 3.35 | 42 | - |
| s129 | 1 x 1 | 69 | 89 | 139 | 64 | 25 | 18 | 22 | 28 | 0.97 | 0 | 0 of 578 |
| s129 | .5 x .5 | 92 | 89 | 72 | 35 | 17 | 21 | 23 | 32 | 1.90 | 11 | 0 of 830 |
| s129 | .25 x .25 | 251 | 94 | 72 | 49 | 29 | 32 | 22 | 31 | 3.71 | 0 (8299 unverified) | 4 of 2284 |
| s129 | 1 x .5 | 95 | 92 | 72 | 42 | 20 | 21 | 23 | 31 | 1.90 | 11 | 0 of 822 |
| s129 | 1 x .25 | 252 | 93 | 72 | 51 | 23 | 27 | 23 | 32 | 3.68 | 10 | 0 of 2160 |
| utqi146 | 1 x 1 | 138 | 85 | 148 | 62 | 32 | 37 | 30 | 27 | (20) | 58 | - |
| utqi146 | .5 x .5 | 275 | 93 | 110 | 62 | 32 | 35 | 26 | 22 | (33) | 302 | - |
| utqi146 | .25 x .25 | 138 | 76 | 208 | 98 | 63 | 46 | 31 | 26 | (44) | 114 | - |
| utqi146 | 1 x .5 | 262 | 88 | 103 | 63 | 34 | 38 | 28 | 23 | (35) | 58 | - |
| utqi146 | 1 x .25 | 156 | 84 | 64 | 128 | 54 | 38 | 27 | 21 | (36) | 267 | - |
| chti143 | 1 x 1 | 102 | 95 | 67 | 11 | 6 | 3 | 4 | 4 | 3.1 | 14 | - |
| chti143 | .5 x .5 | 208 | 95 | 66 | 15 | 9 | 5 | 4 | 5 | 6.7 | 15 | - |
| chti143 | .25 x .25 | 686 | 93 | 102 | 65 | 11 | 7 | 5 | 6 | 12.8 | 0 | - |
| chti143 | 1 x .5 | 207 | 95 | 68 | 12 | 9 | 4 | 4 | 5 | 6.8 | 14 | - |
| chti143 | 1 x .25 | 681 | 95 | 68 | 18 | 10 | 6 | 5 | 6 | 12.7 | 14 | - |

Wrong held samples: (epoch, satellite pair) samples of a held pair whose fix-minus-float difference rounds to a
nonzero integer on the wide or narrow lane with both within 0.2 cycle (float twin of the same stream; utqi and chti
use the existing float users `_rap_nw_obsadf_*`). Truth (simulator): each accepted fix's N1 and N2 = N1 - dd_wl minus
the fixture's truth double differences (`truth_ambiguities.csv`); the network's integer datum adds a constant per
pair, so the pooled mode over the five runs is the reference (318 pairs, 4 with a second offset, all from the
0.25 x 0.25 run). utqi146 and chti143 calibration: the reference coordinates leave a standing 2 to 3 cm (utqi) and
a few mm (chti) error, so the ratio there measures the coordinate, not the covariance (bracketed).

Reading: scaling multiplies accepted searches (2 to 6 times) but not held epochs (held share within a few points,
and lower at 0.25 x 0.25 on w129 and utqi). Time to first fix shortens on the simulator (139 to 72 min) and with
phase scaling on w129 (100 to 79 min); hour 1 error falls there accordingly (92 to 26 mm on w129). Hours 2 to 24
are not better in any variant and are slightly worse at 0.25. The covariance becomes overconfident in direct
proportion (calibration 0.74 / 0.97 at 1 x 1, about 1.6 to 1.9 at 0.5, 3.2 to 3.7 at 0.25). Wrong holds rise with
scaling: w129 0 to 42 samples (phase 0.25), utqi146 58 to 302 (0.5 x 0.5) and 267 (phase 0.25), simulator truth
0 to 4 wrong fix rows (0.25 x 0.25); chti143 is flat.

#### 4. The statistic

The network's post-fit sigma0 (`network.trace_postfit_sigma0`, src/core/network/diag/FrameDiag.cpp:255-360) is
1.4826 x median(|v_i| sqrt(w_i)) over the rows of each class: it is NOT divided by the redundancy. Post-fit residuals
have variance sigma^2 r_i with r_i < 1, and each phase row's redundancy is small (per-LOS slant ionosphere, ambiguity,
clocks), so it is biased low by about sqrt(mean r). The redundancy-normalised factor is in `network_vcf.csv`
(f = sum w v^2 / sum r, a VARIANCE factor). Its medians over the day: observed day 129 phase 0.21 (sigma factor
0.46), code 0.20 to 0.29 (sigma 0.45 to 0.54); simulator phase 0.17 to 0.19 (sigma 0.42 to 0.44), code 0.99. The
observed phase pooled mean is 3.2 (a heavy tail), so the median and the mean disagree by a factor of 15. The user
engine has no unit-weight statistic of its own; its sat trace (`closed_loop_sat_trace.csv`) carries innovations and
post-fit residuals but no redundancy, so a properly normalised user unit weight was not computed. The user's own
position covariance is calibrated at 1 x 1 (0.74 observed, 0.97 simulator), i.e. at the network's noise the user is
not pessimistic in the position domain; the 0.5 phase variant corresponds to the network's sigma factor.

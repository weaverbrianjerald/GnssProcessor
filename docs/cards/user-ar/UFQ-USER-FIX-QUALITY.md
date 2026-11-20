# UFQ: user integer fixing, quality of the search, the held set and the noise model

**STATUS**

- **id:** UFQ
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-20. Three changes to the user's integer fixing, each behind a key that is off by default and identical when off: (3) the search's true ending, its ratio and the held integers reported in the trace and at run end; (2) a search over the not-yet-held ambiguities only, conditioned on the held set; (1) a measurement noise model taken from the user's own data, with the tail handled apart. Judged on the simulator user against truth and on WTZZ00DEU day 129, UTQI day 146 and CHTI day 143.
- **dates:** 2026-11-20 (pre-registered, sections 1 to 7)
- **knobs / posture:** replay users of the reference streams (see section 2); new keys listed per item, every one default off with a ledger row UNVALIDATED.
- **code:** branch `user/fix-quality-2026-11-20` (worktree `D:\GnssProcessorSupport\worktrees\user_fix`, cut from main `8109a64`); files `include/filter/AmbiguityResolver*.hpp`, `src/core/filter/AmbiguityResolver.cpp`, `src/core/realtime/ConsumerTrace.cpp`, `include/realtime/ConsumerTrace.hpp`, the user's row weighting in the real-time engine.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_ufq_<bed>_<arm>`; tools and tables in `D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_fix\`.
- **source:** the user noise-scaling test of 2026-11-20 (`D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_noise_test\`); card [UFA](../../refound/UFA_user_fixing_audit_2026-11-18.md) (rules W1 to W18, defects P1 to P15); the user-holds evidence package of 2026-11-19 (decisions 1 to 6); the addendum of 2026-11-19 on a statistic in the data's own scale.

**Outcome.** Open; pre-registered. The user holds integers on 82 to 95 % of epochs, yet the trace reports every failed search as `mlambda_fail` with ratio 0, and 1,751 of 2,880 searches at WTZZ00DEU end on the ratio upper bound while integers are held. Item 3 makes the trace report the ending, the ratio and the held set; item 2 searches only what is not yet held; item 1 replaces the borrowed phase sigma by one estimated from the user's own residuals, with a separate tail rule. Nothing is armed in a reference file.

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

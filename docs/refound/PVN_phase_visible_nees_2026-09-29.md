# PVN: the phase-visible consistency instrument (the estimator's phase-visible NEES)

**STATUS**

- **id:** PVN
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-09-29, pre-registered, built and read the same day, measure-only, default off. The instrument (`network.phase_visible_monitor`) measures the estimator's ionosphere-free phase-visible clock D = c - sum_j w_j p_j per satellite: the truth-free normalized update e^2 / (P- - P+) on both beds and the within-arc state NEES on the sim. Identity with the knob on, both beds. Readings at 2 h: sim NIS 0.87 (control) and 1.03 (exact update), within-arc NEES 0.2 and 0.3 (wander 2 to 5 cm inside a stated 3 to 28 cm); field NIS 0.40 and 0.10, NEES 0.6 and 0.2. The covariance along D is consistent to conservative, not overconfident, which corrects the premise of card LAH's rejection of the smoothed hold (it took D as stated to millimetres): with the actual stated sd the smoothed member's regression on the generation start stays near 1 for hours, so it is owed a build.
- **source:** [LAH](LAH_level_aware_hold_2026-09-29.md) sections 2, 8 and 9; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 4 and 5 (the level instrument and the phase-space guard statistic this card reuses); [CTU](CTU_coupling_time_update_2026-09-28.md) section 13.4 (h); [PBH](PBH_phase_bias_hold_2026-09-19.md) read 3
- **knobs / posture:** `network.phase_visible_monitor` (measure-only, default off; requires `network.raim_within_epoch`, whose fold capture supplies P-), `network.phase_visible_window_ep` (default 60); no golden, ledger or posture change
- **dates in the text:** 2026-09-29
- **hashes / run dirs:** exe `gnss_processor_pvn2.exe` sha256 prefix `fdcb57464e77e7e8`; arms `runs/_rap_pv_{ctl,ex}_{s,f}`; section 7

## 1. The quantity

Per satellite with a clock and phase biases on two bands in the folded dense block, the functional
of card CTU2 section 5: D = c - sum_j w_j p_j with one phase bias per band (the best-determined),
sum_j w_j = 1 (blind to the level) and sum_j w_j lambda_j^2 = 0 (blind to the ionosphere
direction); for GPS L1 / L2, w = 2.5457 / -1.5457. D is the only direction of (c, p_1, p_2) that the
clock walk moves and the phase data see directly. Every statistic is taken on the constellation
contrast (the clock gauge), as the level instrument does.

## 2. Two measures

**(a) Truth-free, both beds: the normalized update.** For a consistent filter the update of any
functional, g'(x+ - x-), has variance g'(P- - P+)g. Per satellite and epoch,
NIS_u = e^2 / (S- - S+), with e the contrast update of D (the median over the constellation
removed), S- and S+ the contrast-projected prior and posterior variances of D (the fold's capture
and the solve's covariance), no floor. Mean over the settled epochs toward 1; the run-end line also
reports it per window of `phase_visible_window_ep` epochs and the share above 6.63. This is the
guard statistic of card CTU2 section 5 without its 2 cm floor and outside the guard.

**(b) Sim only: the state NEES.** On the sim the a priori clock is the simulator's and the true
satellite phase biases are constant, so the true D of a satellite is constant within a phase-datum
era. The within-arc scatter of the posterior contrast D about its arc mean, over the posterior
contrast variance S+, is therefore a NEES that needs no datum: NEES = mean over arcs of
(D+ - mean_arc D+)^2 / S+ times n / (n - 1). Arcs break at a gap and at a phase-bias frame event of
the satellite. De-meaning removes the slow part of the error, so this reads low for a drift; it is
a lower bound, and an NEES far above 1 is conclusive. Computed offline from the trace.

The trace `logs/network_phase_visible.csv` carries per satellite and epoch: the contrast D+, sqrt S+,
the contrast update e, sqrt(S- - S+) and NIS_u, so both measures and any regrading come from one
file.

## 3. Gates

- U0: unit gates for the statistic's new fields against a brute-force computation on a synthetic
  network (two bands per satellite), and for the window accumulator; the full suite.
- G0: with the knob on, products, consumer trace and epoch status byte-identical to the knob off,
  both beds.
- Readings (not bars): measure (a) on both beds and (b) on the sim, on the golden control and on the
  exact update of card CTU2 (2 h, the sim from 00:30:15), with sigma0 from the existing post-fit
  trace. These readings are the baseline the cross-term, F3 and augmented-border arms are graded
  against.

## 4. The build (2026-09-29)

`network.phase_visible_monitor` (seam `src/core/filter/NetworkPhaseVisibleDiag.cpp`, called after
the clock QC block, when the epoch's last re-solve has run and the dense fold's capture still holds
the prior). The anchor guard's phase statistic (`clockAnchorPhaseStatistic`) gained the fields the
instrument reads and the guard does not: the contrast-projected prior and posterior variances
unfloored (`Sm`, `Sp`), the posterior value and its constellation median removal (`d_post`,
`d_post_hat`) and the signals the functional took (`sigs`). The trace
`logs/network_phase_visible.csv` carries `sigs`, `d_hat_m`, `sqrt_sp_m`, `e_hat_m`, `sqrt_su_m` and
`nis`; the run-end line `card PVN / phase-visible consistency` the NIS mean after the 15th epoch, the
share above 6.63, the medians of |update|, predicted update sd and stated posterior sd, and per
window the mean NIS and the rms update over the rms predicted update sd. The loader refuses the knob
without `network.raim_within_epoch` and a window below 2. Offline grader: the session scratchpad's
`pvnees.py` (the per-hour NIS and the within-arc state NEES of section 2 (b)).

Two traps in reading the trace, both paid for on the first grade:

- `d_hat_m` is the level less the constellation median of the LEVELS, and each satellite's level
  carries its own phase-bias datum (metres), so that median jumps whenever the satellite set
  changes. The state series is rebuilt from the steps: each satellite's step less the median step
  over the satellites present at both epochs (the construction of card LAH's phase-clock hold).
- On the field, band 1 carries two phase-bias families (L1C and L1W, datums hundreds of metres
  apart) and the functional takes the better-determined one each epoch, so it can change definition
  between epochs (G24 alternated by 142 m). The update of one epoch is unaffected (prior and
  posterior use the same keys); the state series breaks where `sigs` changes.

## 5. Gates and readings (2026-09-29)

U0: `tests/test_phase_visible_monitor.cpp` (suite `PhaseVisibleMonitorTest`, 4 cases: the NIS and
its refusals, the new fields against a brute-force contrast projection on a synthetic two-band
network with a dense prior, the seam's rows, settling and windows, the loader's requirements); the
full Debug suite 2 708 tests, 2 707 passed, 1 fixture-gated skip.

G0: with the knob on, the 2 h arms' `products.txt` and `closed_loop_trace.csv` are exact prefixes of
the knob-off 6 h runs, both beds, on the control (`_rap_pv_ctl_{s,f}` against `_rap_s6_ctl_{s,f}`)
and on the exact update (`_rap_pv_ex_{s,f}` against `_rap_s7_ex_{s,f}`).

Readings, 2 h, the sim from 00:30:15 (exe `_pvn2`):

| arm | NIS mean (h1, h2) | within-arc NEES (h1, h2) | within-arc scatter h1 / h2 | stated sd h1 / h2 | sigma0 global |
| --- | --- | --- | --- | --- | --- |
| sim control `_rap_pv_ctl_s` | 0.87 (0.96, 0.79) | 0.2 (0.2, 0.1) | 4.5 / 2.5 cm | 21 / 21 cm | 0.55 |
| sim exact `_rap_pv_ex_s` | 1.03 (0.99, 1.07) | 0.3 (0.3, 0.1) | 5.6 / 0.7 cm | 28 / 2.9 cm | 0.50 |
| field control `_rap_pv_ctl_f` | 0.40 (0.56, 0.25) | 0.6 (0.7, 0.3) | 4.6 / 1.3 cm | 9.6 / 4.7 cm | 0.81 |
| field exact `_rap_pv_ex_f` | 0.10 (0.15, 0.06) | 0.2 (0.2, 0.0) | 11.5 / 1.6 cm | 73 / 11 cm | 3.90 |

The stated sd and scatter are rms over the arcs' rows; the medians of the run-end line are smaller
(stated posterior sd p50 5.4, 4.4, 3.7 and 14 cm; |update| p50 2.9, 2.5, 1.1 and 1.5 mm against a
predicted update sd p50 5.2, 4.0, 4.3 and 12.5 mm). The field sigma0 of the exact update is the
prior side's (prior tension 41: the prior rows' residuals outweigh the data rows'), not the data's.
On the field the within-arc scatter also carries the real motion of D beyond the a priori, so it
reads as scatter over stated sd rather than a NEES; it is below 1 regardless.

## 6. Reading

The estimator's covariance of the ionosphere-free phase-visible clock is not overconfident. On the
sim its per-epoch update is consistent (NIS 0.87 to 1.03) and its state is stated conservatively
(the wander of 2 to 5 cm that card PBH read 3 measured against truth sits inside a stated 3 to 28 cm,
NEES 0.2 to 0.3, a lower bound). On the field both measures read below 1 (NIS 0.10 to 0.40, NEES 0.2
to 0.6): conservative, most so with the exact update. The consistency target for the critical path
is therefore the other side of 1: a covariance that is too loose along D, not too tight.

This corrects the premise of card LAH's rejection of the smoothed hold (LAH sections 2 and 8
item 3). That rejection took the phase-visible clock as stated to a few millimetres, from a
numerical toy of the posture and from the per-epoch phase-clock update of card CTU2 section 5
(2.2 mm p50), which is the size of an update, not the state's uncertainty. With the estimator's
actual stated sd (centimetres to decimetres) against the modelled walk (0.93 mm per epoch), the
smoothed hold's regression of D(t0) on D(t) stays near 1 for hours (a half-life of order
P_D / q epochs: about 2 900 epochs at 5 cm, 970 at 2.9 cm), so the smoothed member behaves like the
phase-clock hold on these beds while passing the modelled walk, and it adapts on its own to an a
priori whose walk is larger. The offline proxy that failed (rho = 1 / (1 + n / 60)) did not use the
estimator's covariance. The smoothed member is owed a build before the question is closed either
way.

## 7. Hashes and run dirs

Exe `gnss_processor_pvn2.exe` sha256 prefix `fdcb57464e77e7e8` (`_pvn1` `f28f756b78c4d38b`, before
the `sigs` column); arms `runs/_rap_pv_{ctl,ex}_{s,f}`.

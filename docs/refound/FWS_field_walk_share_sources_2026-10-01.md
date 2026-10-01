# FWS: what at the field stations makes the per-satellite walk share larger

**STATUS**

- **id:** FWS
- **topic:** network-vehicle
- **status:** closed
- **summary:** ANSWERED 2026-10-01 (sections 2.1, 4 and 5). The field's per-satellite walk share is larger because its steps are persistent, not larger, and the persistence has two sources at the field stations. Short scales (2.5 to 10 minutes): the stations' own phase errors are time-correlated (lag-1 autocorrelation 0.65 at every station, at least 0.37; the sim's white noise 0.15), and they reach a satellite's phase-visible clock in proportion to how thinly it is observed (the sparse side of the roster carries 88 % of the persistent walk; European-visible satellites are white); a coloured phase error on the sim at the field's colour-to-white ratio reproduces it. Long scales (30 minutes to an hour): the field posture's fractional phase wind-up, whose one-cycle staircase is MW-blind and half unflagged; putting it into the sim creates long-lag persistence, and accumulating it on the 6 h field with its restart response removes 45 % of the excess at an hour, consumer-neutral under the phase-clock hold. Not the cause: the loss-of-lock stations, low elevations, the European cluster, the second code channel, L1W, the pivot, the orbits. Nothing armed. CORRECTED 2026-10-01 (section 6): the wind-up restart response refloated no arc (3 carries bridged a restart unanswered; card WUP fixes it); the nine sim arms with a RINEX operation ran 1.53 h and were re-run at 2 h with every verdict unchanged; the census consumers ran fractional wind-up.
- **source:** [WSL](WSL_walk_share_stations_link_2026-10-01.md); [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) section 5; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) (the martingale test, the structure of the motion); [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) (the common part, excluded here); [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the trace columns); card SCC (the second code channel); card CSL (the band-1 signal latch)
- **knobs / posture:** arms only, from the 2 h controls `runs/_rap_wsx_ctl_{f,s}` (card REV's instrument, card SMD's exact-update posture): `--drop-station`, `processing.elevation_mask_deg`, `network.second_code_channel`, `processing.signal_priorities`, and the sim's RINEX ops; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exes `gnss_processor_wsx1.exe` `e3d6c4955054e1f9` (2 h arms), `gnss_processor_rev1.exe` `a59231c8802a14f8` (6 h); arms `runs/_rap_fws_*`; rebuilt wire `runs/_rpl_fws_wuacc6_lah_f`; census key `fwswu`; scripts `scripts/clock_level/wsf_persist.py`

## 1. The decomposition (card REV's 6 h traces)

Each epoch the smoother splits the frame-free step of D into a revision of the generation-start
value and a walk increment dPhi; card SMD's member passes the walk. Over block-clone rows of age at
least 30 minutes (each satellite's L2W key, the common step of each epoch removed):

| | field | sim |
| --- | --- | --- |
| \|frame-free step of D\| p50 / p90 per epoch, ages 1 to 2 h | 0.48 / 2.07 mm | 0.99 / 3.47 mm |
| \|dPhi\| p50 / p90 per epoch, ages 1 to 2 h | 0.30 / 0.91 mm | 0.52 / 1.49 mm |
| realized walk fraction dPhi / step, p50, ages 1 to 2 h | 0.075 | 0.220 |
| rms change of Phi_D over 5 / 20 / 60 / 120 epochs, against sqrt(k) times the rms step | 1.54 / 1.83 / 1.98 / 1.84 | 1.05 / 0.93 / 0.68 / 0.54 |

The field's steps are smaller and a smaller fraction of each is booked as walk, yet its walk share
grows faster: the steps keep their sign over minutes to an hour. The sim's walk is white at short
lags and reverts at long ones. The 2 h controls show the same contrast (ratio at 5 / 20 / 60 epochs:
field 1.47 / 1.66 / 1.83, sim 1.10 / 1.09 / 0.85), and pinning a well-tracked clock pivot (card PAC's
arm) does not move it (1.49 / 1.69 / 1.70). The statistic of this card is that persistence ratio
(`scripts/clock_level/wsf_persist.py`).

## 2. The trace reads (in flight when this section was written)

Four independent reads of card REV's 6 h traces, each on both beds with the sim as the null: the
station side (time correlation of each station's post-fit residuals by elevation, and whether a
satellite seen through time-correlated or low-elevation stations drifts more); the satellite side
and geometry (block type, health, and whether the drift tracks the observing set's geometry, as an
orbit error absorbed through an unbalanced network would); events and the band-1 signal mix (the
walk energy in post-event windows against the base rate; L1W against L1C trackers); and the code
side against the phase side of the same satellite.

## 3. The bisection arms, predicted before they ran

One change each, 2 h, `gnss_processor_wsx1.exe`; field arms subtract a field ingredient, sim arms
add one. An arm explains the persistence when it moves the ratio at 20 and 60 epochs at least half
way toward the other bed's control (field toward 1.1 / 0.85, sim toward 1.66 / 1.83).

| arm | change | tests |
| --- | --- | --- |
| `fws_eur_f` | drop the 11 European cluster stations | clustering, the cluster's own errors |
| `fws_el25_f` | `processing.elevation_mask_deg` 15 -> 25 | low-elevation multipath and troposphere |
| `fws_noscc_f` | `network.second_code_channel` off | the second code channel (field only) |
| `fws_l1c_f` | band-1 priority C before W (code and phase) | the L1W signal family |
| `fws_mpph_s` | per-line-of-sight phase sinusoid, 5 mm on L1 and L2, period 10 min | time-correlated phase errors |
| `fws_mpcd_s` | per-line-of-sight code sinusoid, 0.3 m on C1 and C2, period 10 min | time-correlated code errors |
| `fws_slip_s` | loss-of-lock slips on L1 / L2 at 0.15 per arc-hour | slips and re-levels |

**Predictions.** The station side is the leading candidate: `fws_el25_f` lowers the field's ratio
half way, and `fws_mpph_s` or `fws_mpcd_s` raises the sim's half way. `fws_eur_f`, `fws_noscc_f`,
`fws_l1c_f` and `fws_slip_s` move it by less than a quarter of the gap.

### 3.1 The first batch and the field-side arms it calls for (2026-10-01)

The first batch read (`wsf_persist.py`, ratio at 5 / 20 / 60 epochs): the field subtractions do not
lower the persistence (European cluster dropped 1.54 / 1.81 / 2.00; elevation mask 25 degrees 1.64 /
2.16 / 2.68, worse, with the walk share at 1 h doubled to 10.3 mm; second code channel off 1.47 /
1.66 / 1.85; L1C first 1.53 / 1.86 / 1.93; control 1.47 / 1.66 / 1.83). On the sim, time-correlated
phase or code errors barely move it (1.18 / 1.12 / 0.96 and 1.16 / 1.12 / 0.97; control 1.10 / 1.09 /
0.85) and losses of lock reproduce it (0.15 per arc-hour: 1.39 / 1.98 / 1.70). A second sim batch:
0.05 per arc-hour 1.13 / 1.13 / 0.84 (no effect), 0.45 per arc-hour 1.77 / 3.37 at 5 / 20 epochs (too
few clones reach 30 minutes for 60), cycle steps with no gap at 0.15 per arc-hour 1.13 / 1.14 / 0.96
(no effect); the arm with `network.cycle_slip.relevel_free_epoch` off left too few clone rows to read.
The prediction of section 3 failed on the station side (time-correlated errors) and held nowhere it
was placed; the losses of lock carry it on the sim.

On the field the loss-of-lock and resume events (lli_slip, if_slip, mw_wl_slip, reentry_relevel,
arc_resume, sig_resume, attrib_arc) number 364 over 875 arc-hours in the first 2 h (0.42 per
arc-hour), concentrated: MAC100AUS 137 (8.9 per arc-hour), then BSHM00ISR, MAJU00MHL, YKRO00CIV,
BOGT00COL, SVTL00RUS, CPVG00CPV, WARK00NZL, DARW00AUS and LMMF00MTQ at 0.8 to 1.7 per arc-hour.

**Two field arms, predicted before they ran.** `fws_nomac_f` drops MAC100AUS alone (38 % of the
events); `fws_noslip10_f` drops the ten stations above (the size of the European arm, whose drop did
not lower the persistence). If the losses of lock drive the field's persistence: `fws_noslip10_f`
lowers the ratio at 20 and 60 epochs at least half way toward the sim's control (at most 1.38 and
1.34), and `fws_nomac_f` by at least a quarter of the gap (at most 1.52 and 1.59).

### 2.1 The trace reads (2026-10-01)

Four read-only investigations of card REV's 6 h traces, each on both beds; the persistence ratio
PR = rms(W10) / (sqrt(20) rms(dPhi)) over 10-minute windows of one clone's walk increments (field
1.73, sim 0.94; the 2 h controls 1.62 and 1.05).

- **The station side (time-correlated errors as the residuals show them) does not carry it.**
  Each field station's ionosphere-free phase residual, de-meaned per arc, decorrelates within about
  10 minutes (station-median autocorrelation 0.65 / 0.19 / 0.01 / -0.04 at lags 1 / 5 / 20 / 60
  epochs; sim 0.15 / 0.04 / 0.00 / -0.02), and errors independent between stations can move D by at
  most 1.27 mm per 10 minutes against an excess of 3.76 mm (at most 11 % of the excess variance).
  What organizes the persistence is which stations see the satellite: satellites with the European
  cluster in view are white (PR 0.97, 12 % of the energy), the rest carry 88 % (PR 2.08); the
  strongest single predictor is the share of observations from 15 sparse-side stations whose
  estimated ZWD wanders on 10 to 60 minute scales (share above 0.4: PR 2.24, 62 % of the energy; at
  most 0.2: PR 1.04), and it follows the stations' identity and region, not their current
  troposphere.
- **The satellite side and geometry do not.** An orbit error projected through the network's
  unbalanced geometry is about ten times too small (a CODE-final error moves D by at most 0.5 mm per
  10 minutes; a signed fit sits at its null); no satellite property (block, clock roughness, beta,
  health) ranks the satellites. The observer count does (rank correlation -0.54), and at matched
  counts (12 to 16) the field stays at about 1.9 against the sim's 0.92: a persistent error per
  observer that averages down as observers are added.
- **The phase, not the code, carries the walk, and the field's phase is coloured.** On both beds the
  walk step follows the same epoch's weighted satellite-mean ionosphere-free phase residual (r
  -0.785 field, -0.845 sim; the code's r about 0). The field's residuals are coloured where the sim's
  are white: every field station's lag-1 autocorrelation is at least 0.34 (sim p90 0.19), the colour
  is the same at every elevation, strongest at low-latitude Asian and African stations in daylight
  (ULAB00MNG, LCK400IND, SEYG00SYC, HKSL00HKG, MAJU00MHL, MBAR00UGA, URUM00CHN), and not spatially
  coherent (the share of stations with the mean's sign matches the sim's). This explains the
  short-lag excess (5 to 20 epochs). It does not explain the long lags (1.92 at 60 epochs, 1.82 at
  120), where a minutes-scale colour would revert.
- **One field-only processing feature carries part of the long lags: the fractional phase wind-up.**
  The field posture runs wind-up in fractional (wrapping) mode (`estimation.accumulate_phase_windup`
  off; the sim pins it on), so each crossing of half a cycle steps the applied correction by one
  cycle on both bands, 107 mm in the ionosphere-free combination, MW-blind; the IF-TD threshold's
  floor equals that step, so about half of them (49 of 90 in 6 h) are never flagged. Windows touched
  by a wrap are 6.9 % of the windows and carry 51.5 % of the excess (PR 2.53 against 1.55 untouched);
  giving them the untouched persistence takes the field from 1.80 to 1.55. None fall in the first
  2 h. The logged slip, resume, datum and channel families carry about 4 % or less.

### 3.2 The arms the reads call for, predicted before they ran

| arm | change | prediction |
| --- | --- | --- |
| `fws_los30_s` | sim: per-line-of-sight phase sinusoid, 30 mm on L1 and L2, period 15 min (the field's colour-to-white ratio; the sim's white phase noise is about 2.5 times the field's, which is why the 5 mm arm moved nothing) | the short lags rise (ratio at 5 / 20 epochs at least 1.4 / 1.4), the 60-epoch ratio does not rise above the control's 0.85 by more than 0.15 |
| `fws_wufrac_s` | sim: `estimation.accumulate_phase_windup` off (the field's wrapping staircase) | the 20-epoch ratio at least 1.25 and the 60-epoch ratio above 1.1 |
| `fws_both_s` | the two together | the 20- and 60-epoch ratios at least half way to the field's (1.38 and 1.34) |
| `fws_wuacc6_f` | field, 6 h, `card REV`'s exe: wind-up accumulated with its restart response (`estimation.accumulate_phase_windup`, `network.cycle_slip.windup_restart_refloat`) against `runs/_rap_rev1_blk6_f` | the 60- and 120-epoch ratios fall by at least a quarter of their excess over 1 (to at most 1.69 and 1.62) |

## 4. The arms (2026-10-01)

All 2 h on `gnss_processor_wsx1.exe` (`e3d6c4955054e1f9`) unless stated; `fws_wuacc6_f` 6 h on
`gnss_processor_rev1.exe` (`a59231c8802a14f8`) against `runs/_rap_rev1_blk6_f` (same exe). Ratio at
5 / 20 / 60 epochs (`wsf_persist.py`):

| arm | ratio | against the prediction |
| --- | --- | --- |
| field control `wsx_ctl_f` | 1.47 / 1.66 / 1.83 | |
| `fws_eur_f` (European cluster dropped) | 1.54 / 1.81 / 2.00 | no (predicted no) |
| `fws_el25_f` (mask 25 degrees) | 1.64 / 2.16 / 2.68 | the opposite of the station-side prediction |
| `fws_noscc_f` (second code channel off) | 1.47 / 1.66 / 1.85 | no (predicted no) |
| `fws_l1c_f` (L1C first) | 1.53 / 1.86 / 1.93 | no (predicted no) |
| `fws_nomac_f` (MAC100AUS dropped, 38 % of the losses of lock) | 1.47 / 1.66 / 1.82 | FAILS (bar 1.52 / 1.59) |
| `fws_noslip10_f` (the ten worst loss-of-lock stations dropped) | 1.41 / 1.51 / 1.78 | FAILS (bar 1.38 / 1.34) |
| sim control `wsx_ctl_s` | 1.10 / 1.09 / 0.85 | |
| `fws_mpph_s` (phase sinusoid 5 mm, 10 min) | 1.18 / 1.12 / 0.96 | FAILS (the sim's white noise is 2.5 times the field's) |
| `fws_mpcd_s` (code sinusoid 0.3 m, 10 min) | 1.16 / 1.12 / 0.97 | no |
| `fws_slip05_s` / `fws_slip_s` / `fws_slip45_s` (losses of lock 0.05 / 0.15 / 0.45 per arc-hour) | 1.13 / 1.13 / 0.84; 1.39 / 1.98 / 1.70; 1.77 / 3.37 / - | the sim reproduces the persistence above 0.05 |
| `fws_step_s` (cycle steps, no gap, 0.15 per arc-hour) | 1.13 / 1.14 / 0.96 | no |
| `fws_los30_s` (phase sinusoid 30 mm, 15 min: the field's colour-to-white ratio) | 1.82 / 1.67 / 0.69 | HOLDS |
| `fws_wufrac_s` (fractional wind-up) | 1.18 / 1.40 / 1.56 | HOLDS |
| `fws_both_s` (the two together) | 1.82 / 1.71 / 0.99 | FAILS at 60 epochs (bar 1.34): the coloured noise raises the one-epoch step the ratio divides by; in absolute terms its change over 60 epochs is 9.3 mm against the control's 5.1 and the fractional wind-up's 9.7 |
| field 6 h control `rev1_blk6_f` | 1.54 / 1.83 / 1.98 (120: 1.84) | |
| `fws_wuacc6_f` (wind-up accumulated with its restart response) | 1.45 / 1.51 / 1.54 (120: 1.53) | HOLDS (bar 1.69 / 1.62 at 60 / 120) |

In absolute terms the field's walk changes over an hour by 9.1 mm (control) and 6.5 mm (accumulated
wind-up); over two hours both reach 12 mm (sim 6.5). The restart response answered the 28 restarts it
owed (none needed a refloat; 41 fell beyond the carry horizon; closure 69 of 69).

## 5. Reading (2026-10-01)

**What at the field stations makes the walk share larger** comes in two parts.

The short time scales (2.5 to 10 minutes) are the field stations' own phase errors. The walk step
follows the same epoch's satellite-mean ionosphere-free phase residual on both beds; the field's
phase residuals are time-correlated at every station (lag-1 autocorrelation 0.64 against the sim's
white 0.16, strongest at low-latitude stations in daylight), where the simulator draws white noise of
2.5 times the size. Each station's coloured error is too short and too small to show on its own, but
it reaches a satellite's phase-visible clock in proportion to how thinly that satellite is observed:
the sparse side of the roster carries 88 % of the persistent walk and satellites seen from the dense
European group are white. A coloured line-of-sight error on the sim at the field's colour-to-white
ratio reproduces the field's short-lag persistence and reverts at an hour, as the field's own colour
would.

The long time scales (30 minutes to an hour) are mostly a processing choice: the field posture's
fractional phase wind-up. Its wrapping staircase steps the applied correction by one cycle (107 mm in
the ionosphere-free combination) at each half-cycle crossing, MW-blind, half of them unflagged by
the IF-TD detector whose floor equals the step. Putting it into the sim creates long-lag persistence
in 2 h; taking it out of the field (accumulated wind-up with its restart response, 6 h) removes 45 %
of the excess at an hour and a third at two hours. What remains at the two-hour scale is not
attributed.

Not the cause: the stations that lose lock most (dropping them leaves the persistence; losses of
lock do produce it on the sim above about 0.1 per arc-hour, but the field's are concentrated at a
few stations whose removal changes little), low elevations (removing them makes it worse, fewer
observations per satellite), the European cluster (it dilutes the walk), the second code channel,
the L1W family, the clock pivot (card PAC), the orbits and the satellites' properties.

**What follows (the author's decisions).** Accumulated phase wind-up for the field lineages, the open
card of CLAUDE.md's wu93 section whose blocking response now exists and answered every restart owed
here; the census read below grades its consumer effect. The sim's white phase noise under-represents
the field's coloured error, so any sim validation of a member that passes motion (the hold family)
needs a coloured component in the fixture. Coverage on the sparse side of the roster is the
structural lever. Nothing is armed.

**The census read** (not pre-registered): the phase-clock hold's wire of `fws_wuacc6_f`, rebuilt from
its block trace (`smd_ctlwire.py ... phi_m -1`), on the 16-site census plus WTZZ00DEU (key `fwswu`)
against the control's (`revlah`): the median paired change is 0.000 m in every hour (one or two sites
move by a centimetre either way), WTZZ00DEU identical. Accumulated wind-up is consumer-neutral under
the phase-clock hold, which does not pass the phase-visible motion it changes; its value is to the
members that pass motion and to the estimator's own consistency.

The re-grade of the station-side colour (this card's own computation over card REV's traces, each
station-satellite arc of at least 40 epochs de-meaned): field station-median lag-1 / lag-5
autocorrelation 0.65 / 0.19, every station at least 0.37; sim 0.15 / 0.04, p90 0.18.

## 6. Corrections (2026-10-01, after cards SOI and LSR)

Three statements of sections 4 and 5 were wrong; the conclusions they supported are re-graded here.

**The restart response did not answer the restarts.** Section 4 says the response "answered the 28
restarts it owed (none needed a refloat)". It refloated no arc: its run-end line reads 28
"RESPONDED (0 arc(s) refloated)", and all 28 event rows read `no_arc`. The drain walked the live
ambiguity map, and a restart is a return after a gap longer than the wind-up horizon, whose column
was pruned with the gap and is re-created only after the drain; a warm event with no match was still
counted as a response, so the closure read OK. Against the same-epoch gap re-entry rows
(`network_events.csv`, tier `T0_gap`): 20 of the 28 arcs were refloated by the gap re-entry drain
for its own evidence, 5 had no leveling carry to bridge (fresh arcs), and 3 had their carry bridged
across the restart with nothing answering it (`KIT300UZB` G02 at TOW 262110,
`KIT300UZB` G22 at 279690, `MAJU00MHL` G19 at 280020; each an unmeasurable return the gap drain
bridges by design). Whether those three restarts stepped by a non-zero integer is not recorded. The
fix and its gates are card [WUP](WUP_windup_accumulated_promotion_2026-10-01.md). The arm's walk-share
reading (45 % of the hour-scale excess removed) does not rest on the response: it is the staircase's
removal, with three bridged arcs in 6 h.

**The sim arms with a RINEX operation ran 1.53 h, not 2 h.** `scripts/rapid_arm.py` truncated the
rewritten RINEX at the arm's duration counted from the start of the day, not from the arm's start
offset (00:30:15 on the sim), so the nine arms of section 4 that rewrite RINEX (`fws_mpph_s`,
`fws_mpcd_s`, the four slip arms, `fws_step_s`, `fws_los30_s`, `fws_both_s`) ran 185 epochs against
the control's 241, and their ratios were read over a shorter window than the control's. The tool is
fixed (the truncation now adds the start offset) and the arms were re-run at full length
(`runs/_rap_fws_*_s2`, `gnss_processor_wsx1.exe`, 241 epochs each; `wsf_persist.py`, ratio at
5 / 20 / 60 / 120 epochs):

| arm | as run in section 4 (1.53 h) | full length (2 h) |
| --- | --- | --- |
| control `wsx_ctl_s` (always 2 h) | 1.10 / 1.09 / 0.85 | 1.10 / 1.09 / 0.85 / 0.66 |
| `fws_mpph_s` | 1.18 / 1.12 / 0.96 | 1.15 / 1.07 / 0.82 / 0.64 |
| `fws_mpcd_s` | 1.16 / 1.12 / 0.97 | 1.14 / 1.07 / 0.84 / 0.65 |
| `fws_slip05_s` | 1.13 / 1.13 / 0.84 | 1.11 / 1.08 / 0.88 / 0.48 |
| `fws_slip_s` | 1.39 / 1.98 / 1.70 | 1.37 / 1.96 / 2.62 / 1.95 |
| `fws_slip45_s` | 1.77 / 3.37 / - | 1.50 / 2.55 / 3.22 / - |
| `fws_step_s` | 1.13 / 1.14 / 0.96 | 1.11 / 1.10 / 0.86 / 0.76 |
| `fws_los30_s` | 1.82 / 1.67 / 0.69 | 1.82 / 1.64 / 0.61 / 0.54 |
| `fws_both_s` | 1.82 / 1.71 / 0.99 | 1.82 / 1.72 / 0.98 / 1.07 |

Every verdict of section 4 stands at full length: `fws_los30_s` holds (at least 1.4 at 5 and 20
epochs, not above 1.0 at 60), the 5 mm phase, the code, the 0.05 per arc-hour slip and the step arms
do nothing, the slip arms above 0.1 per arc-hour reproduce the persistence, and `fws_both_s` fails at
60 epochs (bar 1.34). `fws_slipnorl_s` still leaves too few clone rows to read.

**The census read's consumers ran fractional wind-up.** `census6h.py` builds its replays from
`runs/_rf0hv10_field6/arm.json`, which does not accumulate, so the `fwswu` key graded the wire of the
accumulated arm with fractional-mode consumers. "Consumer-neutral" in section 5 holds for that pair;
an accumulating consumer, which arming the knob in the goldens would also give the in-process
consumer (the closed-loop consumer's configuration is a copy of the producer's), was not graded. Card
WUP's census grades it.

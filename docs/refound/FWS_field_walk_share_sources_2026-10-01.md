# FWS: what at the field stations makes the per-satellite walk share larger

**STATUS**

- **id:** FWS
- **topic:** network-vehicle
- **status:** open
- **summary:** Opened 2026-10-01 (the author's question after card WSL). Card SMC found that card SMD's member passes a per-satellite walk share whose between-satellite spread late in the run is 25 mm on the field against 9 mm on the sim, and card WSL ruled out the stations-per-satellite arithmetic of the carry. The decomposition of section 1 changes the question: the field's single-epoch walk steps are smaller than the sim's, and its walk share is larger because its steps are persistent (the change over a lag grows about twice as fast as independent steps would give, from 2.5 minutes to an hour), where the sim's are white and then mean-reverting. The question becomes what at the field stations produces per-satellite phase-visible errors correlated over minutes to an hour. Four trace reads (section 2) and one-change bisection arms on both beds (section 3, predicted before they ran) attribute it.
- **source:** [WSL](WSL_walk_share_stations_link_2026-10-01.md); [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) section 5; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) (the martingale test, the structure of the motion); [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) (the common part, excluded here); [REV](REV_revision_member_replay_2026-09-30.md) section 6 (the trace columns); card SCC (the second code channel); card CSL (the band-1 signal latch)
- **knobs / posture:** arms only, from the 2 h controls `runs/_rap_wsx_ctl_{f,s}` (card REV's instrument, card SMD's exact-update posture): `--drop-station`, `processing.elevation_mask_deg`, `network.second_code_channel`, `processing.signal_priorities`, and the sim's RINEX ops; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** to be filled at the read (section 4)

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

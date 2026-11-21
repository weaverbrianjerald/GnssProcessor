# IOF: the user with the network's ionosphere (WTZZ00DEU, stage 1)

**STATUS**

- **id:** IOF
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-21, before the first graded run, at the author's request through the managing session. The user reads the network's slant ionosphere from a network run's own trace (stage 1) and applies it as between-satellite pseudo-observations before the integer search. Two variants at WTZZ00DEU on the poster's five days: ionosphere-fixed (the two receivers at the site, WTZR00DEU and WTZS00DEU) and ionosphere-weighted (a plane over the nine regional receivers, 162 to 457 km). The weights are measured on the five poster networks (section 4). The bar is fixed in section 8. Changed before the first graded run (section 10): corrections only from links at 30 degrees or more at the network receiver (the author's ruling), a measure-only closure check at the user first, tables of each day from the other four days, and the integer check also against the uncorrected never-fixing user. Result 2026-11-21 (section 11): the closure check at the user stops the work before any constrained run; most satellites agree to a few millimetres, but the converged user's own slant states carry steps of 11 to 18 cm on single satellites (G25 on four of five days) that its position does not see.
- **dates:** 2026-11-21 (pre-registered; design changed before the first graded run, section 10; the closure check, section 11).
- **knobs / posture:** `network.closed_loop.iono_file` (the switch, default off) with `iono_file_logs`, `iono_file_stations`, `iono_file_combine`, `iono_file_sigma_elev_deg`, `iono_file_sigma_m`, `iono_file_test_k`, `iono_file_return_ep`, `iono_file_min_elev_deg` (the cut at the network receiver, default 30) and `iono_file_measure_only` (formed and written, nothing applied); the poster's replay user at the 10 degree mask (`_rap_agu3_rm10x_<doy>_wtzz`, `_rap_agu3_rm10f_<doy>_wtzz`) on the products of `_rap_agu3_rm10_n<doy>`.
- **code:** branch `user/iono-file-2026-11-21` (from 52ae46e): `realtime/NetworkIonoFile` (the reader), `realtime/NetworkIonoConstraint` (selection, weights, per-satellite events, counters), the seam `RealTimeEngine::applyNetworkIonoFile_` in `RealTimeEngineEstimate.cpp`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_iof_<arm>_<doy>_wtzz`; tools and write-up in `D:\GnssProcessorSupport\gates\day_program_2026-11-21\iono_user\`.
- **source:** the author's question of 2026-11-21 through the managing session (brief `D:\GnssProcessorSupport\gates\day_program_2026-11-21\iono_user\BRIEF.md`, with `iono_pair.py` and `site_pair_all_days.txt`); card [QHO](QHO-QUIN-HEIGHT-OFFSET.md) section 6 (the 10 degree poster pair).

**Outcome.** Open. Stopped at the closure check at the user (section 11), before any constrained run. The user at WTZZ00DEU takes the network's slant ionosphere on between-satellite differences, so the receiver terms of the network receivers and of the user cancel and no state is added. Only links whose integers the network holds at that epoch are used. Ionosphere-fixed: site pair, sigma 0.2 to 0.6 cm by elevation. Ionosphere-weighted: ring plane, sigma 2.8 to 5.6 cm. Bar: no wrong integer on three pairs and sooner to 10 and 5 cm on each of five days.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

The author (2026-11-21): "I'm curious if our WTZZ user convergence will become much faster when using our network
ionosphere (iono weighted) with fixed ambiguities. In PPP-RTK literature, the iono-fixed and iono-weighted seems to give
that final push toward instant or only a few minutes to reach convergence. I chose WTZZ specifically because we could
easily show that user separated with the iono variants as a separate line in one of the poster figures to
demonstrate."

## 2. Variants and comparison

- **Ionosphere-fixed** (`iono_file_combine: pair`): corrections from WTZR00DEU (0.0 km) and WTZS00DEU (0.1 km).
- **Ionosphere-weighted** (`iono_file_combine: plane`): corrections from the nine regional receivers only (GOPE00CZE,
  GOP600CZE, LEIJ00DEU, GRAZ00AUT, FFMJ00DEU, POTS00DEU, WROC00POL, PTBB00DEU, BOR100POL; WTZR and WTZS left out).
- **Comparison line:** the poster's own user without any network ionosphere, `_rap_agu3_rm10x_<doy>_wtzz` (fixing) and
  `_rap_agu3_rm10f_<doy>_wtzz` (never fixing).

Each variant runs as a fixing and a never-fixing user; every other key as the comparison line.

## 3. Method

**Source (stage 1).** The network run's `logs/network_ionosphere.csv` (`iono_total_m`: the absolute slant delay on GPS
L1, the reference band of the one observation equation the network and the user share). A link (receiver, satellite)
is used at an epoch only when the network has its integers at that epoch: held or decided by the ring fix for the
constellation's pair (`network_ring_fix.csv`; G L1C/L2W, E L1C/L5Q) or pinned by the chain on both bands of the pair
(`network_chain_edges.csv`, a pin target). The user and the network run share the epoch grid; a correction is matched
to the epoch exactly.

**Corrections.** Per epoch and constellation:
- `pair`: d_s = I_A,s - I_B,s over the satellites integer at both receivers; the receiver term is the median of d
  (3 or more satellites); a satellite is used only if |d_s - median| <= test_k sigma(el); the correction is the mean
  of the two, aligned by that median.
- `plane`: a reference satellite seen by the user and integer at the most ring receivers; per other satellite, the
  between-satellite differences of the ring receivers where both are integer (4 or more receivers) are fitted by a
  plane in east and north, evaluated at the user's a priori position.
- `single` (the simulator's mechanics run): one receiver, no agreement test.

**Rows.** Between-satellite differences within a constellation: x_s - x_r = c_s - c_r on the user's per-satellite
slant states, with R = D diag(sigma_s^2) D^T. The reference r is the user's BSSD pivot when it has a valid correction,
otherwise the highest valid satellite. The receiver terms (about +1 m Galileo and -0.6 m GPS between WTZR and WTZS)
cancel; the frozen state of the user's pivot is never pulled, since only differences are constrained. The rows are
applied after the measurement update and before the integer search (`RealTimeEngineEstimate.cpp`). Satellites between
the user's 10 degree mask and the network's 15 degrees have no correction and stay free.

**Events, per satellite, counted at both edges.** Release when the correction is missing, a link is not integer, the
two site receivers disagree (`pair`), or the row's innovation exceeds test_k times the square root of R + HPH^T (the
jump test). On an innovation release the satellite's slant variance is reset to its initial value, since the user's
state and the correction conflict; on the other releases the state relaxes with its own process noise. A released
satellite returns only after `iono_file_return_ep` consecutive epochs passing every test, the innovation test
included. Run-end line per constellation: epochs with rows, rows applied, releases by cause, returns validated and
failed.

## 4. Measured weights (before any user run)

Tool `iono_weights.py`, `iono_tables.py`; output `weights_all_days.txt`, `tables.json` (the iono_user folder). Five
poster networks, hours 3 to 6 UT. Per bin (lower edge, degrees) the RMS of the values that pass a 4-sigma test on the
bin's robust scale, GPS and Galileo pooled, made non-increasing with elevation (cm):

| Bin | 15 | 20 | 25 | 30 | 40 | 50 | 60 |
|---|---|---|---|---|---|---|---|
| `pair`: WTZR minus WTZS, epoch median removed (the table) | 0.57 | 0.57 | 0.33 | 0.33 | 0.23 | 0.20 | 0.20 |
| `pair` without the integer filter, GPS, RMS of all values | 11.94 | 3.15 | 2.20 | 0.38 | 0.22 | 0.22 | 0.21 |
| `plane` over the ring: prediction minus WTZR (the table) | 5.63 | 4.35 | 3.49 | 3.04 | 2.93 | 2.93 | 2.83 |
| nearest pair GOPE/GOP6: prediction minus WTZR, GPS | 21.70 | 17.76 | 13.84 | 9.39 | 8.22 | 7.05 | 4.92 |

The integer filter removes the low-elevation tail of the site pair (at 15 to 20 degrees, GPS, from 11.94 to 0.87 cm
RMS of all values). The plane over the ring predicts WTZR three to four times better than the nearest co-located pair
at 162 km, so the weighted variant uses the plane. The tables enter the configuration as `iono_file_sigma_elev_deg`
[15, 20, 25, 30, 40, 50, 60] and `iono_file_sigma_m` (pair: 0.0057, 0.0057, 0.0033, 0.0033, 0.0023, 0.0020, 0.0020;
plane: 0.0563, 0.0435, 0.0349, 0.0304, 0.0293, 0.0293, 0.0283). The site pair's agreement is the role the user's
receiver plays against a correction (a second receiver with its own antenna and multipath at the site), so it is used
as the correction's sigma, not divided by the number of receivers.

## 5. Simulator

The fixture has no network receiver near its user: the nearest to SZZ000SIM is S48000SIM at 579 km, the next at 2,425
km. It cannot test the ionosphere-fixed variant. It allows a mechanics run of the `single` path (S48000SIM to
SZZ000SIM) on the existing 2 h simulator network `_rap_irm_s_2`, which writes the same three traces, with sigma from
the simulator's truth difference by elevation and the truth check of the user's held integers (the fixture's
`truth_ambiguities.csv`, mapped into the network's datum through S48000SIM's network integers).

## 6. Runs

Release engine only, gate priority 4, a smoke of a few epochs first. With the keys off, the branch's engine reproduces
`_rap_agu3_rm10x_<doy>_wtzz` on at least two days. Then WTZZ00DEU on days 143 to 147, the two variants, each fixing and
never fixing: the poster's set-up (moving user, joins at 03:00, 3 hours, mask 10 degrees, products of
`_rap_agu3_rm10_n<doy>` replayed, corrections from `_rap_agu3_rarm1120_n<doy>/logs`, the same network run).

## 7. Grading

- 3D error per epoch for the first 30 minutes; time to 10 cm and to 5 cm (the 30-epoch rule of `grade_warm.py`) and
  the error after the first hour (median 3D, minutes 60 to 180), per day and as the median of the five days; time to
  the first held integer and the held share.
- The integer check on the three pairs (G L1C/L2W, E L1X/L5X, E L1C/L5Q) against the never-fixing user of the same
  variant (`users_agu3.py both`), and the position error itself as the independent check.
- Ratio of the 3D error to the reported 3D sigma from minute 60.
- The run-end counters of released and returned corrections.

## 8. Bar (fixed before the runs)

A variant is offered for the poster only if no integer is wrong on any of the three pairs on any of the five days, and
its times to 10 cm and to 5 cm are sooner than the comparison line's on each of the five days. Otherwise the result is
reported as measured. Five starts are a demonstration at one site with network receivers 0 and 100 m away, not a
statement about users in general; the report and any caption say so.

## 9. Stage 2

What stage 1 shows that a record in the product file and an interpolation for a user anywhere need is written below
with the results.

## 10. Changes before the first graded run (2026-11-21)

The managing session's vetting of the design and the author's ruling, both received before any graded run:

- **A. Cut at the network receiver.** Corrections come only from links at 30 degrees or more at the network receiver
  they come from (`iono_file_min_elev_deg`, default 30). The author: "If the iono is much more stable at 30-degree mask,
  then the network iono product handed to the user should simply use the 30-degree mask, while the user retains its
  10-degree mask." The user keeps its 10 degree mask; its satellites without a correction keep a free ionosphere. The
  sigma table covers 30 degrees and up (an elevation at the user below the first edge takes the first bin); no weight
  by elevation below 30 degrees.
- **B. Closure at the user first.** The agreement of WTZR00DEU and WTZS00DEU shows that the network is consistent with
  itself, not that the user's slant states are in the same datum (WTZZ00DEU tracks Galileo L1X/L5X, WTZR00DEU and
  WTZS00DEU L1C/L5Q; the satellite bias convention the user applies and its own antenna pattern can each leave a
  per-satellite offset). A measure-only form (`iono_file_measure_only`: corrections, rows and innovations formed and
  written per satellite and epoch to `logs/iono_file_trace.csv`, nothing applied) runs first on the five days, and the
  innovations of the converged unconstrained user (minutes 60 to 180) give the per-satellite mean and scatter by
  constellation. Per-satellite means beyond the scatter stop the work for a report. The sigma tables of both variants
  come from this closure at the user, not from the network-side agreement of section 4.
- **C. Out of sample.** The table applied on a day and the choice between the nearest pair and the plane come from the
  other four days. The choice, network side from 30 degrees up (`iono_choice_loo.py`): the plane on every left-out set
  (2.55 to 2.92 cm against 6.59 to 7.79 cm for the nearest pair on the other four days).
- **D. A reference that does not share the constraint.** The integer check runs against the never-fixing user without
  corrections (`_rap_agu3_rm10f_<doy>_wtzz`) as well as against the variant's own never-fixing user; the position
  against the coordinates stays the third check.

The traces of section 3 exist for all five network days. The tables of section 4 (network side, all five days, from
15 degrees) are not the ones applied; they stay as the network-side record. The 10-epoch smoke of the first form
(`_rap_iof_smk_145_wtzz`, 15 degree cut, before this section) and the first launch, stopped before any run finished,
are not graded. The corrections are taken at the user's own epoch from the network run's file (no latency).

## Results, 2026-11-21

### 11. The keys-off check and the closure at the user (measure-only)

Engine `gnss_processor_iof2.exe` (sha256[:16] 08c1e36780d33e58, commit ef3bc95). Keys off (`_rap_iof_off_143_wtzz`,
`_rap_iof_off_145_wtzz`): the 18 shared columns of `closed_loop_trace.csv` equal the poster's runs on every row except
`ar_gate` on failed searches (card UFQ item 3's declared endings, which the poster's engine predates), and the
resolver lines are identical. Measure-only (`_rap_iof_mpx_<doy>_wtzz` and `_rap_iof_mpf_<doy>_wtzz` site pair,
`_rap_iof_mwx_<doy>_wtzz` ring plane; 15 runs, every run exit 0): the 3D error equals the poster's run on every epoch
of all 15.

Closure (`iof_closure.py`, `closure.txt` in the iono_user folder): the correction minus the user's slant state,
centred per epoch and constellation, minutes 60 to 180, five days pooled, links from 30 degrees up.

| Arm | Constellation | Samples | Scatter (robust), cm | RMS of the satellite means, cm | Satellites with mean above scatter |
|---|---|---|---|---|---|
| site pair, fixing user | GPS | 6,121 | 5.73 (0.48) | 3.63 | 1 of 8 (G25 -10.02, scatter 7.83) |
| site pair, fixing user | Galileo | 4,522 | 1.01 (0.18) | 0.60 | 1 of 16 (E16 +2.25, scatter 1.91) |
| site pair, never-fixing user | GPS | 6,121 | 9.25 (10.68) | 7.71 | 7 of 8 |
| site pair, never-fixing user | Galileo | 4,522 | 3.29 (1.99) | 2.99 | 10 of 16 |
| ring plane, fixing user | GPS | 6,303 | 6.03 (3.39) | 3.24 | 1 of 9 (G25 -8.10) |
| ring plane, fixing user | Galileo | 6,206 | 3.85 (2.57) | 2.16 | 3 of 21 (E06, E27, E03) |

What the offsets are (`closure_sats.txt`, `closure_pivot.txt`): G25 sits at -15 to -18 cm for stretches of 50 to 150
minutes on days 144 to 147 and at -1.2 cm otherwise (and on day 143 throughout); G29 at +7 cm for 20 minutes (day
147); G18 at -11 cm for 10 to 20 minutes (days 145 and 147); E16 at +3 to +5 cm on day 143. At each switch the user's
slant state moves (by -16.9 cm in one epoch on day 144 at minute 106.5, by -13.7 cm on day 146 at minute 145.5, and on
day 145 at the hand-over of the user's GPS pivot away from G25), while the network's correction runs smoothly. The
user's position does not change across the switches (3D error 20 minutes before and after: 0.84 and 0.86 cm, 1.16 and
1.24 cm, 1.20 and 1.11 cm). The offset therefore lies along a direction the user's position does not see: the level of
one satellite's slant state is not held by the user's own data and moves at user events (a re-fix, the pivot
hand-over). The never-fixing user's slant states carry offsets of several centimetres on most satellites (they are not
converged to the centimetre in three hours). The Galileo signal family (WTZZ00DEU on L1X/L5X, the site receivers on
L1C/L5Q) shows no offset at this level: 15 of 16 Galileo satellites agree within 0.7 cm on average (E16, from day
143, is the one above), robust scatter 0.18 cm.

Corrections per epoch at the 30 degree cut: site pair, a median of 5 GPS and 4 Galileo satellites; ring plane, 5 and 5.

Reading: the stop criterion of section 10 (B) is met (per-satellite means beyond the scatter, in the fixing user's
converged states); the work stops here for the managing session. No constrained run was made.

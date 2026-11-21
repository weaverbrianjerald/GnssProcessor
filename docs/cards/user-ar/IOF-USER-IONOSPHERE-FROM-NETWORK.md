# IOF: the user with the network's ionosphere (WTZZ00DEU, stage 1)

**STATUS**

- **id:** IOF
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-21, before the first graded run, at the author's request through the managing session. The user reads the network's slant ionosphere from a network run's own trace (stage 1) and applies it as between-satellite pseudo-observations before the integer search. Two variants at WTZZ00DEU on the poster's five days: ionosphere-fixed (the two receivers at the site, WTZR00DEU and WTZS00DEU) and ionosphere-weighted (a plane over the nine regional receivers, 162 to 457 km). The weights are measured on the five poster networks (section 4). The bar is fixed in section 8. Changed before the first graded run (section 10): corrections only from links at 30 degrees or more at the network receiver (the author's ruling), a measure-only closure check at the user first, tables of each day from the other four days, and the integer check also against the uncorrected never-fixing user. Result 2026-11-21 (section 11): the closure check at the user stops the work before any constrained run; most satellites agree to a few millimetres, but the converged user's own slant states carry steps of 11 to 18 cm on single satellites (G25 on four of five days) that its position does not see. Explained 2026-11-21 (section 12): the user's pivot and satellites not yet held; the rows now leave the pivot out and the weights are the untrimmed closure RMS per constellation, out of sample. Results 2026-11-21 (section 12): both cuts (30 and 15 degrees, the author asked for the second), zero wrong integers on the simulator and on the five days, no variant sooner on every day; the site pair at 15 degrees reaches 10 cm sooner on all five days.
- **dates:** 2026-11-21 (pre-registered; design changed before the first graded run, section 10; the closure check, section 11; the constrained runs at two cuts, section 12).
- **knobs / posture:** `network.closed_loop.iono_file` (the switch, default off) with `iono_file_logs`, `iono_file_stations`, `iono_file_combine`, `iono_file_sigma_elev_deg`, `iono_file_sigma_m`, `iono_file_test_k`, `iono_file_return_ep`, `iono_file_min_elev_deg` (the cut at the network receiver, default 30) and `iono_file_measure_only` (formed and written, nothing applied); the poster's replay user at the 10 degree mask (`_rap_agu3_rm10x_<doy>_wtzz`, `_rap_agu3_rm10f_<doy>_wtzz`) on the products of `_rap_agu3_rm10_n<doy>`.
- **code:** branch `user/iono-file-2026-11-21` (from 52ae46e): `realtime/NetworkIonoFile` (the reader), `realtime/NetworkIonoConstraint` (selection, weights, per-satellite events, counters), the seam `RealTimeEngine::applyNetworkIonoFile_` in `RealTimeEngineEstimate.cpp`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_iof_<arm>_<doy>_wtzz`; tools and write-up in `D:\GnssProcessorSupport\gates\day_program_2026-11-21\iono_user\`.
- **source:** the author's question of 2026-11-21 through the managing session (brief `D:\GnssProcessorSupport\gates\day_program_2026-11-21\iono_user\BRIEF.md`, with `iono_pair.py` and `site_pair_all_days.txt`); card [QHO](QHO-QUIN-HEIGHT-OFFSET.md) section 6 (the 10 degree poster pair).

**Outcome.** Open. No variant meets the pre-registered bar (sooner to 10 and 5 cm on each of five days); all hold zero wrong integers on three pairs against both references, on the simulator too. The site pair at the 15 degree cut is the best: sooner to 10 cm on all five days (median 6.5 against 14.0 min; 4.2 against 12.6 cm at minute 10), to 5 cm on three. The user's resolver still first fixes after about an hour: its pair-fraction and ratio gates hold the fix back. G25 carries a steady 1.07 cm WTZZ00DEU site term.

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

### 12. The pivot, the weights, the simulator and the five days at two cuts

**What the closure offsets were** (`step1_table.txt`, `step45_iof_mpx.txt`). Of 3,647 satellite-epochs beyond 3 cm,
1,240 belong to the user's between-satellite pivot and 2,340 to satellites whose integers were not yet held; the 67
held ones (G28, day 147) were the pivot pulling the median centre. The between-satellite form zeroes the pivot's slant
column in every row (`BssdTransform.cpp`, step 6c): a non-pivot satellite's slant state is its delay minus the pivot's
plus a constant shared by all non-pivot satellites of that pivot's stint, and the pivot's frozen state is in no row.
The rows now leave the pivot out (differences among non-pivot satellites against the highest one, so the constant
cancels); a pivot hand-over is counted; the innovation test is centred on held satellites (commit 82c417e). Across 10
hand-overs the non-pivot pair closure moved by a median 0.14 cm and at most 0.39 cm, apart from one 17.3 cm case where
G29's integers were held at that epoch.

**G25, a steady site term.** Without the pivot the held satellites close with an RMS of 0.39 cm (GPS) and 0.21 cm
(Galileo), robust 0.17 and 0.16 cm, none beyond 3 cm. G25 sits at -1.07 cm in the slant delay (0.65 cm in the
geometry-free phase; scatter 0.25 cm, 273 epochs): -0.98, -0.99, -1.10, -1.21 cm on days 143 to 146, always on its
descending arc at 30 to 54 degrees; WTZR00DEU and WTZS00DEU agree with each other there, and the plane variant, built
without them, sees it too (-1.5 to -2.8 cm). It is a term of the WTZZ00DEU site for that direction (antenna pattern or
multipath), the user-side limit of the fixed variant at this site, of the same family as QUIN's term (card QHO).

**Weights** (the managing session's rule: the untrimmed RMS of the held non-pivot closure at the user, per
constellation, per elevation bin where the bins differ beyond their own uncertainty, each day from the other four
days; `sigma_tables.json`, `sigma15_tables.json`): 30 degree cut, site pair GPS 0.42 to 0.49, 0.33 to 0.38, 0.23 to 0.30
cm at 30, 40, 50 degrees and up, Galileo 0.22 to 0.26, 0.18 to 0.22, 0.12 to 0.14, 0.14 to 0.20 cm at 30, 40, 50, 60
degrees; plane 1.9 to 7.0 cm. 15 degree cut (the author asked for it beside the 30 degree cut), site pair one value per
constellation (GPS 0.39 to 0.43, Galileo 0.25 to 0.26 cm; the 30 to 40 degree bin is carried by G25, 0.47 against 0.26
cm without it); plane 2.6 to 8.2 cm. No held satellite had a correction between 15 and 20 degrees (the satellites there
are rising and not yet held), so that bin takes the 20 to 30 degree value.

**Simulator** (`sim_truth.txt`; one receiver, S48000SIM, 579 km from SZZ000SIM, sigma 11 to 76 cm from the simulator's
own closure, in-sample, one simulator day). The resolver's holds against truth through an adapter for the repository's
grader (`sim_holds_truth.py`; 0 wrong on an existing 24 h user first): no corrections 4,922 held link-epochs, 0 wrong;
30 degree cut 5,058, 0 wrong; 15 degree cut 5,298, 0 wrong. Time to 10 cm 37.0, 21.5, 23.0 min; to 5 cm 75.5 min in all
three; the first minutes are worse with the decimetre-level correction at 579 km.

**The five days** (`iof_summary.txt`, `iof_holdcheck.txt`, `watch.txt`; engine `gnss_processor_iof4.exe`, sha256[:16]
8a41e1a7c21ea97d; fixing users, medians of the five days unless stated):

| | Comparison | Fixed, 30 deg | Weighted, 30 deg | Fixed, 15 deg | Weighted, 15 deg |
|---|---|---|---|---|---|
| Corrections per epoch, GPS / Galileo | - | 4 / 3 | 4 / 4 | 6 / 6 | 6 / 8 |
| Time to 10 cm, min (days 143 to 147) | 14.0 (10.5, 13.5, 22.5, 27.5, 14.0) | 10.0 (7.5, 14.0, 10.0, 7.0, 11.5) | 11.5 (11.5, 14.0, 10.5, 21.0, 11.5) | 6.5 (6.5, 6.0, 10.0, 3.0, 6.5) | 10.5 (19.5, 13.0, 10.5, 6.0, 10.5) |
| Time to 5 cm, min | 35.0 (41.0, 16.0, 35.0, 34.5, 37.5) | 30.5 (39.5, 30.5, 20.0, 28.5, 40.0) | 39.5 (42.5, 16.0, 41.0, 32.5, 39.5) | 34.0 (8.5, 8.5, 37.0, 34.0, 40.0) | 39.0 (44.0, 15.0, 39.0, 34.0, 39.0) |
| Days sooner, 10 cm / 5 cm | - | 4 / 3 | 3 / 1 | 5 / 3 | 4 / 2 |
| 3D error at minutes 1, 5, 10, 20, cm | 29.5, 13.7, 12.6, 7.9 | 54.9, 21.2, 9.8, 5.3 | 55.1, 31.5, 11.6, 6.4 | 38.4, 14.9, 4.2, 5.0 | 66.3, 31.7, 13.1, 6.0 |
| Error after the first hour, cm | 1.27 | 1.23 | 1.41 | 1.01 | 1.26 |
| Resolver's first fix, min | 53.5 | 57.5 | 64.5 | 61.5 | 66.5 |
| Wrong integers, 3 pairs, 5 days (own float user / float user without corrections) | 0 / - | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 3D error over its sigma from minute 60 | 0.89 | 1.02 | 0.92 | 1.03 | 0.84 |
| Releases (cause and count) | - | low elevation 36, disagree 11, too few 2, pivot 12 | low elevation 40, innovation 6, not integer 2, pivot 12 | missing 53, disagree 36, not integer 1, pivot 12 | missing 60, innovation 16, not integer 2, pivot 12 |
| Returns validated / failed; refloats | - | 22 / 2; 0 | 17 / 1; 6 | 44 / 6; 0 | 23 / 3; 16 |
| Applied beyond 2 / 3 sigma | - | 14 / 1 | 667 / 140 | 8 / 0 | 1,085 / 257 |

Applied corrections beyond 2 and 3 sigma by elevation at the user: fixed 30 degrees, 9 / 1 of 5,588 at 30 to 40 and 5 /
0 of 7,585 above; fixed 15 degrees, 0 / 0 of 2,106 at 15 to 20, 2 / 0 of 5,094 at 20 to 30, 2 / 0 of 5,587 at 30 to
40, 4 / 0 of 7,707 above; weighted 15 degrees, 230 / 76 of 2,943, 236 / 65 of 6,220, 204 / 45 of 6,277, 416 / 71 of
8,983 (the plane's own tails). Every run had 12 pivot hand-overs over the five days, counted.

**Satellites not held at their first correction** (the managing session's watch item, `watch.txt`): fixed 30 degrees
63 of 82 corrected arcs, no innovation refloat among them, time from the first correction to held integers a paired
median 1.0 min later than the same satellite without corrections (46 pairs); weighted 30 degrees 76 of 96, 2 refloats,
1.0 min later; fixed 15 degrees 107 of 116, no refloat, 1.5 min sooner (61 pairs); weighted 15 degrees 128 of 138, 5
refloats, 1.8 min later.

**Why the fix does not come sooner.** In the first hour the constrained fixing users pass the resolver's sigma gate more
often (108 to 150 of 600 epochs end there against 203 in the comparison) and then end at the per-pair fraction gate
(`nl_max_per_pair_frac`, 0.15 cycle: 172 to 240 epochs, one pair rejecting the whole search) or the ratio test (160 to
191). The constrained float itself is close to integer: the never-fixing constrained users have single differences
within 0.01 cycle of an integer on both signals in 35 % (fixed, 30 degrees) and 61 % (fixed, 15 degrees) of epochs,
against 6 % without corrections.

**Reading against the bar of section 8.** No variant is sooner to both 10 cm and 5 cm on each of the five days, so none
is offered under the pre-registered reading; the results are reported as measured. Between the cuts, the 15 degree cut
gives the site pair one or two more corrected satellites per constellation, zero wrong integers and the soonest times to
10 cm (all five days). Five starts are a demonstration at one site with network receivers 0 and 100 m away, not a
statement about users in general.

**Stage 2 (section 9).** What the runs show a product record must carry: per link (network receiver, satellite) and
epoch, the slant delay on GPS L1, whether the network holds the link's integers at that epoch (without the flag the
site pair's agreement at 15 to 20 degrees is 12 cm RMS, with it 0.9 cm), the satellite's elevation at the receiver
(the cut is taken there), the receiver's signal family (WTZZ00DEU on Galileo L1X/L5X against L1C/L5Q at the site
receivers showed no offset here, but it is a property of the pair) and the receiver's position (the plane needs it).
The corrections were taken at the user's own epoch from the network run's file (no latency); a real-time record adds
the age and, at low elevation, a rate. A user away from network receivers gets the plane's 2 to 8 cm (ionosphere-
weighted level); the site pair's 0.2 to 0.5 cm needs a receiver at the site. The user's weight also carries its own
site terms (G25 here), so the sigma belongs to the user's closure, not to the network alone. The user's resolver gates,
not the ionosphere, decide when it fixes: an earlier fix needs a resolver that drops an outlying pair instead of
rejecting the search.

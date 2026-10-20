# PIV: the clock pivot elected by predicted station coverage, health and shadow

**STATUS**

- **id:** PIV
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-18, default off, never armed: `network.pivot_election: coverage` elects each constellation's clock pivot at its first epoch and at a forced handover as the registered, healthy satellite that the orbit product predicts at the most roster stations over a look-ahead horizon, passing over one that enters the Earth's umbra inside it; an incumbent that stays registered and healthy is kept. Unit gate, ledger rows and a Release smoke are in (section 6). The check of section 5 ran 2026-10-18 (section 8): knob absent identical to main except the declared lines (G0), picks G22 and E04 on the field and G32 and E36 on the sim with no handover against two in each control (G1), field hour-2 common drift 27 % of the control's (G3), held-out consumer and 16-site census unchanged within 1 mm at the median (G4, G5); the clock product is within 2.4 mm of the control and marginally not "no worse" (G2).
- **source:** [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) sections 3 to 5 (G14 pinned cut the field hour-2 common drift from -0.180 to -0.063 m/h); [HLT](HLT_broadcast_health_2026-09-25.md) (the health latch and the pivot's health skip); `docs/status/OPEN_ITEMS.md` queued build 1
- **knobs / posture:** `network.pivot_election` (`off` | `coverage`), `network.pivot_election_horizon_s` (7200), `network.pivot_election_step_s` (300); ledger rows UNVALIDATED and PARAM; neither golden changed
- **dates in the text:** 2026-10-17, 2026-10-18
- **hashes / run dirs:** branch `mpg/pivot`; scratch `D:/GnssProcessorSupport/gates/pivot/` (`tools/offline_rank.py`, build logs, smoke output); unit gate `tests/network/test_pivot_election.cpp` (suites `CoveragePivot*`)

**Outcome.** Question: can the clock pivot be chosen by what the network can use of it (stations that track it, a
healthy broadcast record, no umbra entry in the coming hours) instead of by its number? Built as a default-off
election that runs only at ignition and at a forced handover and otherwise stays sticky. Unit gate and a Release
smoke pass (10 epochs, field and sim): the field run elects G22 (27.6 of 58 stations on average over 7200 s; runner-up
G14 27.4) and E04 (28.7), with G01 passed over as unhealthy, where the control keeps G01 and E02; with the knob absent
the output equals a main-built exe's. The 6 h check (section 8) holds on G0, G1, G3, G4 and G5, and G2 within 2.4 mm:
no handover in 6 h against two in each control, field hour-2 common drift 0.051 against 0.192 m/h, consumer and census
unchanged at the median. The forced-handover branch was not exercised. Nothing is armed.

## 1. The rule

The legacy pivot is the declared satellite, else the sticky incumbent (the pivot of the last emitted product), else
the lexically first registered satellite. `network.sat_health: pivot` (card HLT) adds a health skip to the last two.
On DOY 129 that elects G01, which has no healthy broadcast record all day, the fewest trackers of any GPS
satellite and an umbra entry at TOW 276150 (card PAC).

`network.pivot_election: coverage` replaces the lexical fallback by an election. Per constellation, in order:

1. The declared pin (`network.clock_datum_satellite`), when registered. Unchanged.
2. The incumbent (`last_pivot`), when registered and healthy. It stays: no election runs while it holds.
3. The standby, the satellite the previous solved epoch left in force (`ConstellationState::elected_pivot`), when
   registered and healthy. Epochs that are solved but not emitted would otherwise elect again at every epoch, and
   two near-tied candidates could alternate; the standby holds the pick until an emitted product adopts it or it
   leaves.
4. Otherwise a fresh election among the registered satellites that the broadcast health series (card HLT, built at
   setup from the navigation file) does not flag, or among all of them when none is healthy (counted).

The election ranks the candidates by two numbers over the look-ahead horizon (default 7200 s, samples every 300 s,
25 samples):

- **Coverage.** The count of (roster station, sample) pairs with the satellite at or above the processing elevation
  mask, from the SP3 orbit nodes interpolated to each sample (ten nodes, Earth-fixed frame of the sample) and the
  roster coordinates. Elevation is measured against the geocentric up of the station (at most 0.19 degrees from the
  geodetic one, which matters to no count). The mean station count printed is this count over the samples.
- **Umbra.** The first sample inside the cylindrical Earth shadow (`Planetary::isSatelliteEclipsing`, the test the
  geometry stage uses, with `processing.eclipse_shadow_scale`), the Sun from the geometry stage's own `Planetary`
  model.

A candidate that stays out of the umbra for the whole horizon beats one that enters it; among the former the larger
coverage wins; if every candidate enters the umbra, the one that enters it last wins. Ties go to the lexically first
satellite. All sums are integer counts, so the ranking is a pure function of its inputs and the preview before PASS B
(`networkHandoverStransformBeforeRows`) and the solve glue (`networkClockDatum`), which both elect, always agree. The
predictor memoizes the last ranking per constellation, so the second caller costs nothing.

Everything it reads is known before the epoch: the orbit product, the roster coordinates, the Sun and the broadcast
health. It reads no observation.

## 2. Where it lives

| piece | file |
| --- | --- |
| ranking (`rankPivotCandidates`), predictor (`NetworkPivotCoverage`), builder (`networkPivotCoverageArm`) | `network/solve/PivotElection.{hpp,cpp}` |
| the rule inside the datum builder (`PivotElection`, `electClockPivot`, `buildSharedDatum` and its plan counters) | `network/solve/Sbasis.{hpp,cpp}` |
| the per-epoch binding both seams share (`NetworkPivotElectionEpoch`), the standby commit, counters, event rows | `network/solve/ClockDatum.{hpp,cpp}` |
| the preview before the rows | `network/solve/HandoverStransform.cpp` |
| the standby row | `ConstellationState::elected_pivot`, `kConstellationFields` (ColdStart), `kEntityWriters` row `notePivotElections` |
| the knobs, the loader, the fingerprint token `piv:<mode>:<horizon>:<step>` | `GnssConfig`, `GnssConfigLoadNetwork.cpp`, `NetworkKnobs`, `SnapshotFingerprint.cpp` |
| the health series built also under this knob | `ApplicationNetworkSetup.cpp` (the sat_health door) |
| the predictor built once per run | `runNetwork` (one builder call), `NetworkSetupResult::pivot_coverage` |
| the run-end section | `networkPivotElectionRunEnd` in `HandoverStransform.cpp` |

The loader refuses `coverage` without `network.sticky_pivot` (checked after the service profile has had its chance
to arm it), refuses a step that is not positive or a horizon below the step, and warns that the knob is inert under
the sum-to-zero and broadcast-anchored clock datums and that a declared pin stays ahead of it.

## 3. The edges (standing invariant 7) and the triage (invariant 3)

An election that changes the pivot is a clock-pivot handover: the machinery already in place answers it (the
discontinuity bump, the handover S-transform of `network.handover_stransform`, the declared pivot on the S line).
The rule adds none. It also removes one: a pivot elected for coverage and shadow is not the satellite that sets or
enters the umbra next, so the field's G01 handover at TOW 276150 should not occur (to be shown by the check).
The decoupling edge (a pivot leaves) is the forced handover; the re-coupling edge (the old pivot returns) is
unchanged: the incumbent of the last emitted product stays.

Field-blocking deferrals: none known. Field-inert paths: a run whose orbits are broadcast records has no SP3 nodes,
so no candidate ranks and every election falls back to the first candidate (counted as `rank_failed`, one warning at
setup, one at the run end).

## 4. Telemetry (standing invariant 4)

- Setup: one line (roster size, satellites with orbit nodes, horizon, step, mask, umbra scale); a warning when no
  orbit node exists.
- Per election: one `main_log` line (pick, its mean station count, the runner-up, candidates, entries into the
  umbra) and one `network_events.csv` row, tier `PIVOT_ELECT`, event `elected` (statistic: mean stations) or
  `unranked`.
- Run end (inside the clock-pivot subsystem's report): fresh elections, held picks, unranked elections, the table of
  elections, and an INERT warning when the knob is armed and no election ran.

## 5. The check (owed; written before any arm ran)

One exe, the golden copies with only the knob under test changed (`scripts/rapid_arm.py`), the sim golden from
00:30:15, graded as the goldens are (hour 1 is ignition, settled claims come from hours 2 to 6).

| gate | arms | bar |
| --- | --- | --- |
| G0 | knob absent, field and sim, 2 h, against an exe built at main `30ca49e` | product records, `network_epoch_status.csv`, `closed_loop_trace.csv` and `network_events.csv` identical; run-end lines moved only as section 7 declares |
| G1 | field and sim, 6 h, `coverage` against the same exe without it | the elected pivots and handovers from the `PIVOT_ELECT` rows and the S lines: no unhealthy pivot (field G01), at least one election per constellation, and no more handovers than the control |
| G2 | the same pair | `scripts/ftc_step_attrib.py --r3 e_clk --systems G,E` on the clock product: no worse than the control |
| G3 | the same pair | frame-free common D slope by hour: field hour 2 at most half the control's magnitude (control -0.180 m/h on the pre-EXL posture, bar 0.10 as in card PAC); sim not worse |
| G4 | the same pair | held-out consumer by hour (field WTZZ00DEU, sim SZZ000SIM): median of hours 2 to 6 at most +0.005 m above the control, hour 1 at most +0.02 m |
| G5 | field, 6 h | the 16-site census (`gcensus.py`): median paired change of the hour-2 to 6 p50 at most +0.005 m |
| C | field, 6 h, `network.clock_datum_sum_to_zero` | the competitor, for the record, on the same instruments (G2 to G5) |

Never armed by this card: adoption is the author's decision.

## 6. Smoke and unit gate

Unit gate `tests/network/test_pivot_election.cpp`: the ranking (largest count, tie, no orbit, umbra-free first, all
shadowed), the predictor on a synthetic equatorial ring with a fixed Sun (interpolation to 1 m, the counts above the
mask checked by hand, the umbra entry time), the rule (incumbent kept, declared pin, absent or unhealthy incumbent,
no healthy satellite, standby, unusable answer), the datum builder's counters, the per-epoch binding and the loader
(defaults, refusals, fingerprint).

Offline cross-check (`D:/GnssProcessorSupport/gates/pivot/tools/offline_rank.py`, numpy, a low-precision Sun, no
health filter) at 00:00:30 on DOY 129 with the field golden's 58 stations, 15 degree mask, 7200 s and 300 s: GPS G22
27.6, G14 27.5, G30 26.8, G13 26.8, G15 25.3 stations on average, 30 of 32 umbra-free; Galileo E04 28.7, E31 28.2,
E09 27.6, E24 21.0 (inside the umbra at the start). G22 and G14 are a near tie, which is what the standby row is for.

Release smoke 2026-10-18 (exe `gnss_processor_pivot.exe` sha256 prefix `23824b462630cf41`, built from the branch;
control exe `gnss_processor_pivot_base.exe` is the Release build of main `30ca49e`, prefix `50ca2ea5fbbc540c`; arms
from `scripts/rapid_arm.py` on the goldens, `--hours 0.0834`, 11 emitted epochs, run dirs
`D:/GnssProcessorSupport/runs_nom/_rap_pivsmoke_{f,f_ctl,f_base,s}`):

| run | exit | elections (the `main_log` lines) | pivots on the S line, first and last emit |
| --- | --- | --- | --- |
| field, `coverage` | 0 | E04 28.7 of 58 stations (runner-up E31 28.1, 22 candidates, 1 entering the umbra); G22 27.6 (runner-up G14 27.4, 31 candidates, 2 entering the umbra) | E:E04 G:G22, no handover |
| field, knob absent | 0 | none | E:E02 G:G01, no handover |
| sim, `coverage` | 0 | E36 17.5 of 60 (runner-up E26 16.9); G26 16.4 (runner-up G17 16.4) | E:E36 G:G26, no handover |

The C++ ranking reproduces the offline one (G22 27.6 and G14 27.4 against 27.6 and 27.5; E04 28.7 and E31 28.1
against 28.7 and 28.2; the offline version uses a low-precision Sun and its own interpolation). The health series flags 13
satellites for the whole window, G01, E14 and E18 among them, and G01 is not a candidate. The setup line, the
per-election lines, the `PIVOT_ELECT` rows (`scripts/pivot_election_report.py`) and the run-end section print as
section 4 states. The run-end reach row `constellation.elected_pivot` shows 2 rows (the two constellations), ColdStart.

Knob absent against the main-built exe, same arm config: `products.txt`, `network_epoch_status.csv`,
`network_events.csv`, `closed_loop_trace.csv` and `network_ionosphere.csv` are byte-identical, and 31 of the 34 CSVs
are; `network_datum_graph.csv`, `network_run_summary.csv` and `network_service_health.csv` differ only in their
wall-clock columns (`ms`, `loop_ms`, `ms_per_epoch`, `ms_per_epoch_window`). This is a 10-epoch smoke, not gate G0.

## 7. Output moved when the knob is off (declared line by line)

- Run end, "entities as carried" table (`networkEntitiesRunEnd`), two lines: the header line counts one more declared
  field ("N declared field(s)" rises by one), and one new row line, `Network (entities as carried):
  constellation.elected_pivot (was elected_pivot) ColdStart rows max 0 final 0`, follows the `last_pivot` row.
- Nothing else: no counter prints, no setup line, no trace row, and the sat_health door reads the navigation file only
  when `network.sat_health` or `network.pivot_election` asks.

## 8. Check results, 2026-10-18

Separate stage from the build. No source, golden, manifest or ledger row changed; nothing is armed. Candidate: Release
exe built from `mpg/pivot` at `961f863`, sha256 prefix `23824b462630cf41` (`gnss_processor_pivotchk.exe` and
`gnss_processor_w3pivot.exe` under `D:/GnssProcessorSupport/gates/exe/x64-Release/` are byte copies of the smoke exe).
Identity control: `gnss_processor_w3base.exe`, the Release build of main `30ca49e` (prefix `50ca2ea5fbbc540c`).
Same-exe controls for G1 to G5: the candidate with the knob absent. Arms under `D:/GnssProcessorSupport/runs_nom/`
(`_rap_pivchk_{ctl,on}_{f,s}`, `_rap_pivchk_s2z_f`, instrument arms `_rap_pivchk_ins_*`, census
`_rap_pivc_{ctl,on,s2z}_*`); graders, outputs and logs under `D:/GnssProcessorSupport/gates/pivot/chk/`. The 6 h arms
are the goldens with only the knob changed (`scripts/rapid_arm.py`; the sim arms with `network.start_offset_hours`
0.504167). Every run exited 0, with 720 of 720 epochs emitted and none withheld.

### 8.1 G0, knob absent: identical except the two declared lines

- **G5 gate, 23 arms against `w3base`** (`gate_step.sh`, side `w3pivot`; detail `gates/g5/gate_w3pivot_plain/`). Run
  as specified, every arm reads DIFFERENT (`gates/g7/summary_w3pivot.txt`: 23 DIFFERENT, `snap.txt` IDENTICAL), and
  exactly two items differ in each arm: `stdout.txt` and the `main_log` rotation set. Everything else is IDENTICAL in
  all 23 arms: `arm.json`, `products.txt` and the traces in row order, the run manifest (12 records), `rt_engine.log`
  and the exit code. The differing lines are the section 7 lines and nothing else: with them reversed on the
  candidate's text outputs (the `SUBS` mechanism of `gate_step.sh`; `gates/pivot/chk/subs_piv.sed` deletes the
  `constellation.elected_pivot` row and turns "110 declared field(s)" back into "109") the comparison reads RESULT
  IDENTICAL on all 23 arms, and with the snapshot bytes of `snapw` that is 24 IDENTICAL lines
  (`gates/g7/summary_w3pivot_subs.txt`, detail `gates/g5/gate_w3pivot/`).
- **User side** (`uom_gate.py`, quick set, tag `w3pivot` against `w3base`): 16 of 16 arms IDENTICAL, every exit 0
  (`gates/pivot/chk/uom_w3pivot.log`).
- **Real time** (`w3pivot_dr` and `w3pivot_cl` against `w3base_dr` and `w3base_cl`). The three SBF replay arms: every
  CSV and log IDENTICAL; `console.log` differs only in the wall-clock figure of the "Real-time session ended" line
  (1.7 s against 10.2 s, 2.3 s against 12.3 s, 3.7 s against 5.6 s; decoded from UTF-16, one line per arm). The
  closed-loop arm (`cl`): 37 items IDENTICAL, among them `network_products.txt`, `network_solution.csv`,
  `network_events.csv` and `network_epoch_status.csv`; `console.log` and `main_log.log` differ in the known wall-clock
  lines (RINEX ingestion time, epoch-loop wall, round-trip self-check time, the arc-ledger cost line) and in the two
  section 7 lines.
- The knob-absent 6 h field arm of this stage reproduces the baseline pair's consumer to within 0.5 mm (`_rf0hv15_field6`: 58 11 18 10
  15 11 mm by hour).

G0 holds: no output moved other than the declared lines.

### 8.2 G1, elected pivots and handovers (6 h; `scripts/pivot_election_report.py`)

| bed | arm | elections (pick, mean roster stations over the horizon, runner-up) | clock-pivot handovers in 6 h |
| --- | --- | --- | --- |
| field | control | none; pivots E02, G01 | 2: E02 to E03 at 03:20:30, G01 to G02 at 04:42:30 (TOW 276150) |
| field | `coverage` | E04 28.7 of 58 (E31 28.1); G22 27.6 of 58 (G14 27.4); both at 00:00:00 | 0 |
| sim | control | none; pivots E02, G01 | 2: E02 to E03 at 03:20:30, G01 to G02 at 04:42:30 |
| sim | `coverage` | E36 17.4 of 60 (E34 17.2); G32 16.4 of 60 (G02 16.4, a tie at one decimal); both at the first epoch | 0 |

No unhealthy pivot (the health series flags 13 satellites for the whole window, G01, E14 and E18 among the GPS and
Galileo ones; none is a pick), one election per constellation, and no handover against two in each control. The run-end
line reads "2 fresh election(s) made a pick, 0 held, 0 unranked". The sum-to-zero arm has no pivot and no handover.
G1 holds.

Not exercised: the branch that elects at a forced handover (the incumbent leaves). No pick left the roster in 6 h, so
on these beds that branch is covered by the unit gate alone. The ignition picks are near ties (G22 against G14 by 0.2
station, G32 against G02 by under 0.1 station on the sim). The counts are integers and the pick is deterministic, but
another start time or day can reverse it.

### 8.3 G2, clock product (`ftc_step_attrib.py --r3 --systems G,E`; `gates/pivot/chk/r3_pivchk_*.txt`)

e_clk within-arc RMS after the arc mean is removed (mm; p50 / p90 / RMS of the 30 s step), hours 2 to 6, all wire
satellites and the same with each epoch's constellation-common step removed:

| bed, system | form | control | `coverage` | sum-to-zero |
| --- | --- | --- | --- | --- |
| field G | all satellites | 81.4 / 122.2 / 7.32 | 83.3 / 128.7 / 7.11 | 55.1 / 137.7 / 8.95 |
| field G | common step removed | 23.0 / 58.8 / 3.04 | 22.3 / 50.2 / 1.11 | 29.5 / 65.2 / 2.35 |
| field E | all satellites | 22.2 / 48.2 / 3.18 | 23.9 / 44.7 / 2.77 | 26.9 / 43.6 / 2.69 |
| field E | common step removed | 22.0 / 47.7 / 2.71 | 22.4 / 45.4 / 2.28 | 23.4 / 43.2 / 2.31 |
| sim G | all satellites | 16.2 / 25.4 / 3.06 | 18.6 / 27.0 / 2.42 | |
| sim G | common step removed | 10.2 / 22.8 / 2.58 | 10.0 / 19.2 / 1.48 | |
| sim E | all satellites | 22.2 / 29.0 / 4.34 | 22.1 / 28.8 / 4.03 | |
| sim E | common step removed | 9.0 / 18.7 / 2.23 | 9.3 / 18.2 / 1.35 | |

The all-satellites p50 is 1.7 to 2.4 mm larger with the election on three of the four cells (field G +1.9, field E
+1.7, sim G +2.4) and equal on sim E (-0.1). The step RMS is lower in all eight rows and p90 in six of eight (higher on
the all-satellites rows of field G and sim G). With the common step removed the p50 is within 0.7 mm in every cell.
By the letter of "no worse than the control" the all-satellites p50 is marginally not met, by at most 2.4 mm, with the
step RMS and the common-step-removed forms equal or better. The all-hours rows and the consumer-tracked subsets read
the same way (output files). The competitor's all-satellites p50 on field G is 26 mm lower than the control's and its
p90 15 mm higher.

### 8.4 G3, constellation-common drift by hour

The goldens do not write the smoothed hold's clone trace, which is the instrument of card PAC (the limit already
stated in card EXL section 5). Two reads are reported.

**Exact instrument on the card PAC posture, 2 h.** The golden with `emit_pb_hold_phase_clock` false and
`emit_pb_hold_smoothed` true (the hold of the pre-EXL goldens), same exe, arms `_rap_pivchk_ins_*`. The grade is the
arithmetic of `pac_arm.py` (GPS L2W clones; `gates/pivot/chk/pac_hour.py`, which reproduces the card PAC pair to the
digit: `_rap_wsx_ctl_f` +0.210 and -0.180, `_rap_pac_g14_f` +0.174 and -0.063 m/h). Frame-free common D slope, m/h
(hour sum, cm):

| bed | arm (GPS pivot) | hour 1 | hour 2 |
| --- | --- | --- | --- |
| field | control (G01) | +0.063 (+0.8) | -0.192 (-16.2) |
| field | `coverage` (G22) | -0.066 (-20.9) | -0.051 (-4.5) |
| sim | control (G01) | +0.069 (+4.7) | -0.017 (-2.4) |
| sim | `coverage` (G32) | +0.043 (+3.4) | -0.008 (-3.0) |

Field hour 2: 0.051 against 0.192 m/h, 27 % of the control's and under the 0.10 bar; the control reproduces card PAC's
-0.180 on the posture with Galileo added. Sim not worse. The held-out consumer on these 2 h arms is unchanged (field
hours 1 and 2: 75.0 and 22.6 mm against 76.1 and 21.5 mm; sim 103.1 and 25.4 against 104.0 and 25.7 mm). One
observation: the elected G22's own S_D moves +17.1 m over hour 1 (the table's "pivot own" column reads +1712 cm)
against -25 cm for the control pivot. In the control G22 moves +16.95 m over the same hour as a non-pivot satellite,
and +33 m in the archived card PAC runs, so the excursion is that satellite's ignition behaviour and not an effect of
the election. It does mean the pick is the satellite with the largest hour-1 excursion of D on this day.

**Receiver-clock read on the golden posture, 6 h** (`gates/pivot/chk/common_drift_rx.py`, from
`network_rx_states.csv`). The clocks of the three hydrogen-maser stations (PTBB00DEU, WTZR00DEU, WTZS00DEU) carry the
datum's common drift (card PAC section 2), so their mean straight-line slope by run hour is read, m/h:

| arm | hour 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| control | +0.181 | -0.130 | -0.086 | +0.060 | +2.004 | -0.016 |
| `coverage` | +0.024 | -0.000 | +0.001 | +0.037 | +0.077 | +0.007 |
| sum-to-zero | +0.281 | -0.044 | -0.058 | -0.007 | +0.047 | -0.017 |

Hour 5 of the control is the G01 to G02 handover (a clock-frame step of c0 -1.576 m) seen in the receiver clocks; the
election has no handover. Calibration of this proxy on the card PAC pair: it reads the control's hours 1 and 2 as
+0.184 and -0.140 (card D slope +0.210 and -0.180) and the G14 pin's hour 2 as -0.027 (card -0.063), but the pin's
hour 1 as -0.022 against +0.174. It follows the size of the settled drift and not every hour of ignition, so it
supplements the exact read and does not replace it. G3 holds on the exact instrument.

### 8.5 G4, held-out consumer by hour (median 3D error, mm; hour 1 is ignition)

| bed | arm | 1 | 2 | 3 | 4 | 5 | 6 | mean 2 to 6 | median 2 to 6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| field WTZZ00DEU | control | 58.1 | 11.3 | 18.3 | 10.3 | 15.5 | 10.5 | 13.2 | 11.3 |
| field WTZZ00DEU | `coverage` | 58.7 | 11.5 | 23.0 | 11.3 | 14.6 | 8.7 | 13.8 | 11.5 |
| field WTZZ00DEU | sum-to-zero | 57.3 | 12.6 | 25.5 | 13.5 | 18.1 | 15.6 | 17.1 | 15.6 |
| sim SZZ000SIM | control | 99.4 | 32.6 | 39.8 | 16.0 | 18.0 | 15.2 | 24.3 | 18.0 |
| sim SZZ000SIM | `coverage` | 100.3 | 32.4 | 39.6 | 15.4 | 17.8 | 15.3 | 24.1 | 17.8 |

The median of hours 2 to 6 moves +0.2 mm on the field and -0.2 mm on the sim (bar +5 mm); hour 1 moves +0.6 and
+0.9 mm (bar +20 mm). G4 holds. On the field the single hours move +4.7 mm (hour 3) and -1.8 mm (hour 6) and average
to +0.6 mm over hours 2 to 6.

### 8.6 G5, the 16-site census (`gcensus.py`; 6 h wires `_rap_pivchk_ctl_f` and `_rap_pivchk_on_f`, consumer-only replays)

Median site p50 by hour: control 136 54 41 36 40 37 mm, `coverage` 137 56 39 35 41 37 mm. Paired change of each
site's mean hourly p50 over hours 2 to 6: median -0.1 mm (bar +5 mm), no site better or worse by 1 cm; the largest
moves are ARHT00ATA -5.8 mm, CEDU00AUS +4.9 mm, ANMG00MYS -3.5 mm, POHN00FSM -2.4 mm and UTQI00USA +1.7 mm. The hour 1
paired median is -1.4 mm. G5 holds (`gates/pivot/chk/census_grade_ctl_on.txt`).

### 8.7 C, the sum-to-zero competitor, for the record (field, 6 h, `network.clock_datum_sum_to_zero`)

- Consumer WTZZ00DEU: mean of hours 2 to 6 17.1 mm against 13.2 mm (+3.9 mm), every settled hour above the control's
  (section 8.5).
- Census (`census_grade_ctl_s2z.txt`): median paired change 0.0 mm; DAV100ATA -14.5 mm, BAKE00CAN -5.7 mm, CEDU00AUS
  -5.0 mm, and ARHT00ATA +59.2 mm (worse by 6 cm).
- Clock product: all-satellites p50 lower, p90 higher (section 8.3). Common drift: the settled receiver-clock slope is
  small (section 8.4). No pivot, no handover.

### 8.8 Standing

- **Against the frozen bars:** G0, G1, G3, G4 and G5 hold. G2 holds within 2.4 mm and not to the letter: the
  all-satellites p50 of e_clk is 1.7 to 2.4 mm larger on three of four cells while its step RMS and the
  common-step-removed forms are equal or better. The sign of the grade decides nothing here: nothing is armed.
- **What the election does on these beds:** it replaces the unhealthy, sparsely tracked G01 by G22 (and E02 by E04),
  removes the two forced handovers of the control (among them the G01 eclipse handover at TOW 276150), cuts the field
  hour-2 common drift to 27 % of the control's on the card PAC instrument, and leaves the held-out consumer and the
  census unchanged within 1 mm at the median.
- **What the check cannot show:** one day (DOY 129) and one start; the forced-handover branch of the election (no pick
  left the roster, section 8.2); the common drift on the golden posture itself (section 8.4 reads it on the card PAC
  posture and through the receiver clocks); a near-tie pick (G22 against G14) that another start time can reverse.
- **Owed before any adoption (the author's decision):** a run long enough for a pick to set (a 24 h arm), at least one
  more day, and the clone-trace read of the common drift on the armed hold when the interim hold is retired.

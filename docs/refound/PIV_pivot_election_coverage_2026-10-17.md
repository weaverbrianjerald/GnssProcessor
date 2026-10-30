# PIV: the clock pivot elected by predicted station coverage, health and shadow

**STATUS**

- **id:** PIV
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-18, default off, never armed: `network.pivot_election: coverage` elects each constellation's clock pivot at its first epoch and at a forced handover as the registered, healthy satellite that the orbit product predicts at the most roster stations over a look-ahead horizon, passing over one that enters the Earth's umbra inside it; an incumbent that stays registered and healthy is kept. Unit gate, ledger rows and a Release smoke are in (section 6). The check of section 5 ran 2026-10-18 (section 8): knob absent identical to main except the declared lines (G0), picks G22 and E04 on the field and G32 and E36 on the sim with no handover against two in each control (G1), field hour-2 common drift 27 % of the control's (G3), held-out consumer and 16-site census unchanged within 1 mm at the median (G4, G5); the clock product is within 2.4 mm of the control and marginally not "no worse" (G2). 2026-10-28 (section 9): on a 24 h day the forced-handover branch runs and the census is unchanged; on DOY 130 ARHT00ATA is 40.6 mm worse, the one failed clause; not armed. 2026-10-30 (section 10, card NSP): under six neutral changes of the wire no census site moves by more than 0.19 mm, so ARHT00ATA's +40.6 mm is a response to the election's wire and the clause stands failed; not armed. 2026-10-30 (section 11, pre-registered): on DOY 128, 131 and 132 (6 h, same exe and posture) ARHT00ATA's change is +7.6, +3.0 and +25.9 mm, so the clause fails on two of five days (DOY 130 and 132) and the reading is "recurring", neither systematic nor day-specific; ARHT00ATA is worse on four of five days, no other site is worse on any, the census median and WTZZ00DEU are unchanged or better; under the election G01 returns from a short gap within 0.6 m where the legacy pivot path emits it about 100 m off (card MDAY's G01 departure); not armed.
- **source:** [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) sections 3 to 5 (G14 pinned cut the field hour-2 common drift from -0.180 to -0.063 m/h); [HLT](HLT_broadcast_health_2026-09-25.md) (the health latch and the pivot's health skip); `docs/status/OPEN_ITEMS.md` queued build 1
- **knobs / posture:** `network.pivot_election` (`off` | `coverage`), `network.pivot_election_horizon_s` (7200), `network.pivot_election_step_s` (300); ledger rows UNVALIDATED and PARAM; neither golden changed
- **dates in the text:** 2026-10-17, 2026-10-18, 2026-10-28, 2026-10-30
- **hashes / run dirs:** branch `mpg/pivot`; scratch `D:/GnssProcessorSupport/gates/pivot/` (`tools/offline_rank.py`, build logs, smoke output); unit gate `tests/network/test_pivot_election.cpp` (suites `CoveragePivot*`); section 11: exe `build/x64-Release/gnss_processor_pivmd.exe` (a byte copy of `gnss_processor_bof.exe`), run root `D:/GnssProcessorSupport/runs_nom`: bases `pivmd_bases/`, arms `_rap_pivmd{128,131,132}_{ctl,piv}_f6`, census replays `_rap_pivmdc<D>_{ctl,piv}_*`, reports `pivmd_reports/`; pre-registration commit `1e1a538`

**Outcome.** Question: can the clock pivot be chosen by what the network can use of it (stations that track it, a
healthy broadcast record, no umbra entry in the coming hours) instead of by its number? Built as a default-off
election that runs only at ignition and at a forced handover and otherwise stays sticky. Unit gate and a Release
smoke pass (10 epochs, field and sim): the field run elects G22 (27.6 of 58 stations on average over 7200 s; runner-up
G14 27.4) and E04 (28.7), with G01 passed over as unhealthy, where the control keeps G01 and E02; with the knob absent
the output equals a main-built exe's. The 6 h check (section 8) holds on G0, G1, G3, G4 and G5, and G2 within 2.4 mm:
no handover in 6 h against two in each control, field hour-2 common drift 0.051 against 0.192 m/h, consumer and census
unchanged at the median. The forced-handover branch was not exercised. Nothing is armed. 2026-10-28 (section 9): on a 24 h day the forced-handover branch runs and the census is unchanged; on DOY 130 ARHT00ATA is 40.6 mm worse, the one failed clause; not armed. 2026-10-30 (section 10, card NSP): under six neutral changes of the wire no census site moves by more than 0.19 mm, so ARHT00ATA's +40.6 mm is a response to the election's wire and the clause stands failed; not armed. 2026-10-30 (section 11): on DOY 128, 131 and 132 ARHT00ATA is +7.6, +3.0 and +25.9 mm, so the clause fails on two of five days: the response recurs without being systematic (the pre-registered reading "recurring"); no other site is worse on any day; not armed.

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

## 9. The 24 h arm and a second day, 2026-10-28

Exe `build/x64-Release/gnss_processor_bof.exe` (the Release build of main `ef9f75d`, sha256[:16] 2d84a326fd00fd04), run root `D:/GnssProcessorSupport/runs_nom`, field golden DOY 129, 24 h from 00:00 (2880 epochs), one change per arm against `_rap_bof_ctl_f24`. Gates written before the runs: `D:/GnssProcessorSupport/runs_nom/bof_reports/preregistration.md`; grade `.../bof_reports/grade_24h_piv_otl_nta.md` (16-site census of 24 h consumer-only replays, `gcensus.py --hours 24`; worse = a site worse by more than 1 cm). Arm `_rap_bof_piv_f24`; second day `_rap_bof_piv130_f6` against `_rap_bof_ctl130_f6` (the golden moved to DOY 130 by
`scripts/mday_golden_config.py`, 6 h; grade `.../bof_reports/grade_6h_nsd_arl_piv130.md`).

| gate | DOY 129, 24 h | DOY 130, 6 h |
|---|---|---|
| P1 forced-handover branch | runs: G22 departs at 10:28:30, the election picks G23 (27.8 stations, runner-up G18 26.8), handover c0 +1.257 m | not exercised (no pick left) |
| P2 handovers, arm / control | 1 / 2 (control: E02 to E03 at 03:20:30, G01 to G02 at 04:42:30, c0 -1.576 m) | 0 / 1 (control G01 to G02 at 04:39:00) |
| P3 WTZZ00DEU, mean hourly p50 change, hours 2 to N | -0.34 mm (worst hour +3.3 mm) | -0.04 mm (worst hour +1.0 mm) |
| P4 census paired median, sites worse | -0.13 mm, none (ARHT00ATA -6.9 mm) | 0.0 mm, ARHT00ATA +40.6 mm |
| P6 clock product, within-arc RMS p50 (G all / E all) | 93.6 to 98.4 / 27.3 to 25.3 mm | 82.6 to 83.9 / 23.0 to 24.8 mm |

The election runs at a forced handover as built and halves the day's handovers; the held-out consumer and the census
median are unchanged on both days. The pre-registered bar (P1 to P5 together) is missed on one clause: ARHT00ATA is
40.6 mm worse over hours 2 to 6 of DOY 130 (hours 4 to 6 by 45 to 73 mm), while on DOY 129 it is 6.9 mm better.
ARHT00ATA is the census site with the largest error (hourly p50 0.03 to 0.37 m) and moves by centimetres under
unrelated changes of the wire (-16.8 mm under `network.cycle_slip.gf_arc_jump` on DOY 129, card ARL section 14;
+59 mm under the sum-to-zero competitor, section 8.7); its spread under a neutral perturbation is not measured, so the
DOY 130 clause cannot be read as noise or as an effect. The GPS clock product is 4.8 mm worse at the all-satellite p50
on the day (1.3 mm on DOY 130) and Galileo 2.0 mm better (1.8 mm worse on DOY 130). Not ready to arm on the bar as
written; what would settle it is the ARHT00ATA spread under a neutral change (for example two arms that differ only in
an inert knob that perturbs the wire at the rounding level).

## 10. ARHT00ATA's spread under a neutral change, 2026-10-30

Card [NSP](NSP_census_neutral_spread_2026-10-29.md) measured the census under changes of the wire with no expected
effect, on the exe and posture of section 9 (the bytes of `gnss_processor_bof.exe`, the `ef9f75d` golden). The inert
knobs that section 9 suggests (`network.parallel_max_threads`, `network.parallel_carry`) leave the float-vehicle
golden's products byte-identical, so the neutral arms re-round the roster coordinates instead (an independent offset
within plus or minus 0.05 mm per axis, six seeds per day). Graded as section 9's census (hours 2 to 6, paired, 1 cm):

| DOY 130 | six neutral arms | the election |
|---|---|---|
| ARHT00ATA change | +0.19, 0.00, +0.01, -0.02, +0.18, -0.06 mm (s 0.11 mm) | +40.6 mm |
| largest change at another census site | 0.18 mm (HARB00ZAF) | -3.6 mm (QUIN00USA) |
| sites worse by more than 1 cm | none in any arm | ARHT00ATA |
| decision rows changed (90 to 94 % and 98.7 % of them the measure-only prior-residual census) | 513 to 729 | 11439, among them 100 clock-anchor decouple, recouple and landed-discontinuity events |

On DOY 129 no census site moves by more than 0.02 mm in any of six neutral arms.

Reading, fixed before the runs (card NSP section 5): **outside**. No neutral arm moves ARHT00ATA as far (rank 0 of 6)
and +40.6 mm is about 90 times the one-sided 99 % prediction bound of a neutral change (+0.44 mm). The failed clause
is a response of ARHT00ATA to the election's wire on DOY 130, not noise, and it stands. The caveat fixed in advance
holds as well: the neutral arms flip none of the clock-anchor, gate and re-admission decisions that the election
changes, so whether another change of that sequence would move ARHT00ATA as far is not measured. ARHT00ATA's response
to the election is -5.8 mm (DOY 129, 6 h, section 8.6), -6.9 mm (DOY 129, hours 2 to 24) and +40.6 mm (DOY 130, hours
2 to 6); whether the election harms it is a question of more days (the multi-day observations hold DOY 128, 131 and
132). Not armed.

## 11. ARHT00ATA's response to the election on three more days, 2026-10-30

### 11.1 Pre-registration (written before any of its arms ran)

**Question.** Section 10 reads ARHT00ATA's +40.6 mm on DOY 130 as a response to the election's wire, not noise. Is the
response systematic (the election makes ARHT00ATA worse on most days) or specific to DOY 130? Read on the three
remaining days of the multi-day observations, DOY 128, 131 and 132 of 2024.

**Posture.** The posture of sections 9 and 10, for continuity with their numbers: the exe bytes of
`build/x64-Release/gnss_processor_bof.exe` (sha256[:16] 2d84a326fd00fd04, the Release build of main `ef9f75d`), run as
the byte copy `build/x64-Release/gnss_processor_pivmd.exe`, and the field golden of `ef9f75d` moved to each day by the
transform of `scripts/mday_golden_config.py` applied to `git show ef9f75d:config/golden/golden_field_refound.json` (a
copy of the script whose source path names that file; the script itself reads the current golden). Applied to DOY 130
the transform reproduces card BOF's base `bof_bases/golden_field_doy130.json` byte for byte (sha256[:16]
59ed54978019cb16). The current golden is not used: its seven further keys (ocean tide loading, four keys; the noon-turn
attitude; the estimable block test; the seed mirrors) would mix a posture change into the day-to-day comparison, and
the DOY 129 and 130 numbers and the neutral spread of section 10 were all measured on the `ef9f75d` posture. Whether the
election's response is the same on the current golden is not measured here.

Bases, `D:/GnssProcessorSupport/runs_nom/pivmd_bases/golden_field_doy<D>.json`:

| day | date | roster | sha256[:16] | known before the runs (card MDAY) |
|---|---|---|---|---|
| DOY 128 | 2024-05-07 | 57 (COCO00AUS has no RINEX) | 313c640ac9aec371 | MAC100AUS logs no satellite before 10:30 UT, so it is absent from this window; from 03:36 the golden's emitted G01 clock sits about 100 m from the a priori product (section 6.2) |
| DOY 131 | 2024-05-10 | 58 | 0b42b71c4915d04f | MAC100AUS's file ends at 02:01:30; the same G01 departure from 03:22:30 |
| DOY 132 | 2024-05-11 | 56 (MAC100AUS and LCK400IND have no RINEX) | ccf649d9bdd60e23 | 00 to 06 UT lies in the geomagnetic storm of 10 and 11 May (card MDR section 2) |

G01 is the control's GPS pivot at the start of DOY 129 and 130 (the lexically first registered satellite; the golden
does not skip an unhealthy one), so a G01 event inside the window can reach the control's clock datum.

**Arms.** Per day a control and an election arm, 6 h from 00:00, made with `scripts/rapid_arm.py make --base <day base>
--tag <tag> --hours 6` (the election arm adds `--set network.pivot_election=coverage`; horizon and step at their
defaults) and run with `rapid_arm.py run --exe C:/GnssProcessor/build/x64-Release/gnss_processor_pivmd.exe`, with
`RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`: `pivmd128_ctl_f6`, `pivmd128_piv_f6`, `pivmd131_ctl_f6`,
`pivmd131_piv_f6`, `pivmd132_ctl_f6`, `pivmd132_piv_f6`.

**Census.** Per wire, `python scripts/clock_level/gcensus.py make <key> <day's control run> <wire run> <exe> --day D
--obs-root D:/GnssProcessorSupport/data_mday`: consumer-only replays at WTZZ00DEU and the 16 sites; the consumer base of
both keys of a day is that day's control (as in card NSP); keys `pivmdc<D>_ctl` and `pivmdc<D>_piv`. Graded with
`gcensus.py grade pivmdc<D>_ctl pivmdc<D>_piv --day D --obs-root D:/GnssProcessorSupport/data_mday`: x = a site's mean
hourly p50 over hours 2 to 6, d = election minus control, paired by site; a site is worse (better) when d exceeds +1 cm
(falls below -1 cm). Card NSP's spread (ARHT00ATA s 0.11 mm on DOY 130 and 0.014 mm on DOY 129, no site above 0.19 mm)
is taken as the noise of these days; it is not measured on them.

Reported beside the census, without a bar: each arm's elected pivots and handovers (`scripts/pivot_election_report.py`),
WTZZ00DEU's change, the census paired median and its hour 1, ARHT00ATA's hourly change, and the size of the election's
change of the wire (`scripts/neutral_spread.py wire --day D`: between-satellite move and decision rows in one run only).
DOY 129 over hours 2 to 6 is read from the first six hours of card BOF's 24 h replays (`bofc24_ctl`, `bofc24_piv`), so
that all five days share one window; the hours-2-to-24 value of section 9 (-6.9 mm) stands as it is.

**Reading, fixed now.** W = the number of the three new days on which ARHT00ATA is worse by more than 1 cm.

| reading | condition | meaning |
|---|---|---|
| systematic | W at least 2 | the clause fails at ARHT00ATA on at least three of the five days DOY 128 to 132: on this posture and window the election makes ARHT00ATA worse on most days |
| day-specific | W = 0 | the clause fails at ARHT00ATA on DOY 130 alone of the five days: the DOY 130 response does not recur |
| recurring | W = 1 | the clause fails on two of the five days: the response recurs but not on most days; neither reading above holds on this sample, and the next step (more days, or 24 h windows) is the author's |

- The sign and size of ARHT00ATA's change on every day are reported with the reading, and a day with ARHT00ATA better
  by more than 1 cm is named.
- A day whose election wire equals its control's (products byte-identical) has no response to read: it is named and
  left out of W.
- An arm that does not exit 0 is re-run once on the same exe; a second failure is reported and the day is left out of W.
- Another site worse by more than 1 cm on a new day is reported as its own finding; it does not enter W.
- The reading names how often ARHT00ATA's response recurs; it does not by itself make the election harmful or
  harmless. Adoption remains the author's decision.

### 11.2 Results, 2026-10-30

All six arms exited 0 (2206 to 2313 s, six at once on the shared machine); the 102 census replays (17 per key) all
exited 0. Outputs in `D:/GnssProcessorSupport/runs_nom/pivmd_reports/`: `run_arms.log`, `census_driver.py` and its
log, `pivot_report.txt`, `wire.txt`, `score.txt`, `grade_128.txt`, `grade_131.txt`, `grade_132.txt` (the
`gcensus.py grade` output) and `five_day_table.py` with its output `five_day_table.txt` (every site on the five days;
it reproduces card BOF's DOY 130 grade, ARHT00ATA +40.6 mm and its hourly row, exactly).

**The arms.**

| | DOY 128 | DOY 131 | DOY 132 |
|---|---|---|---|
| control: pivots at the start; handovers | G01, E03; G01 to G02 at 03:28:30 (c0 -1.571 m) | G01, E02; G01 to G02 at 03:16:30 (c0 -1.658 m) | G01, E02; E02 to E03 at 01:45:00 (c0 +0.136 m), G01 to G02 at 03:12:30 (c0 -1.563 m) |
| election: picks at 00:00 (mean stations; runner-up) | G14 27.5 (G22 27.5, decided by the integer count); E25 28.5 (E03 28.1) | G22 27.8 (G14 27.3); E02 28.6 (E08 28.2) | G22 26.8 (G13 26.5); E09 27.7 (E24 27.1) |
| election: handovers | 0 | 0 | 0 |
| clock-anchor event rows, control / election | 196 / 94 | 171 / 70 | 373 / 211 |
| between-satellite clock move, hours 2 to 6, p50 / p95 (mm) | 2.9 / 21.1 | 1.5 / 23.9 | 14.7 / 44.8 |
| decision rows in one run only (the first at) | 14922 (00:00:00) | 10966 (00:00:00) | 8216 (00:00:00) |

For scale, DOY 130 (section 10): 2.6 / 19.2 mm and 11439 rows. On DOY 131 the election keeps the control's Galileo
pivot E02, so only the GPS pivot differs. Every election wire differs from its control from the first epoch.

**ARHT00ATA on the five days** (x the control's mean hourly p50 over hours 2 to 6; DOY 129 from the first six hours of
card BOF's 24 h replays, DOY 130 card BOF's grade):

| day | x control (mm) | change (mm) | hourly change, hours 1 to 6 (mm) | clause |
|---|---:|---:|---|---|
| DOY 128 | 134.4 | +7.6 | -5.7, +2.5, +0.2, +4.3, +8.7, +22.0 | holds |
| DOY 129 | 225.4 | -5.8 | +35.6, -3.6, -22.1, +2.8, +8.1, -13.9 | holds (hours 2 to 24: -6.9, section 9) |
| DOY 130 | 197.6 | +40.6 | -2.8, +0.8, +45.6, +15.1, +72.9, +68.7 | fails |
| DOY 131 | 37.0 | +3.0 | 0.0, +0.1, +0.1, -2.3, +6.9, +10.3 | holds |
| DOY 132 | 100.2 | +25.9 | -40.7, -3.2, -3.9, +74.7, -14.5, +76.5 | fails |

**The census of the new days** (16 sites, paired, hours 2 to 6):

| | DOY 128 | DOY 131 | DOY 132 |
|---|---|---|---|
| sites worse / better by more than 1 cm | 0 / 0 | 0 / 0 | 1 (ARHT00ATA +25.9 mm) / 0 |
| paired median (mm) | -0.04 | +0.18 | -0.02 |
| largest changes at the other sites (mm) | DAV100ATA -4.6, POHN00FSM +2.4, UTQI00USA +1.8 | DAV100ATA +2.7, POHN00FSM -2.5, HARB00ZAF +1.6, BADG00RUS -1.6 | HARB00ZAF -8.0, DAV100ATA -2.9, KOKB00USA +2.5, BAKE00CAN +1.5 |
| WTZZ00DEU change (mm) | -1.1 | -1.9 | -0.8 |
| hour 1 paired median (mm); sites better / worse by more than 1 cm | -0.9; 1 / 1 | +0.8; 0 / 0 | -4.8; 6 / 1 |

### 11.3 The reading

**W = 1: ARHT00ATA is worse by more than 1 cm on DOY 132 (+25.9 mm) and worse below the bar on DOY 128 (+7.6 mm)
and DOY 131 (+3.0 mm). The reading fixed in section 11.1 is "recurring".** The clause fails at ARHT00ATA on two of the
five days (DOY 130 and 132) and holds on three; neither "systematic" (W at least 2) nor "day-specific" (W = 0) holds on
this sample.

- ARHT00ATA's change is positive on four of the five days (median +7.6 mm, mean +14.3 mm) and negative on DOY 129
  (-5.8 mm over hours 2 to 6, -6.9 mm over hours 2 to 24). Each change is at least 27 times card NSP's neutral SD at
  the site (0.11 mm), so each is a response, not noise. Four positive signs of five occur with probability 0.19 under
  a response with no preferred sign: the sign pattern alone does not decide the question either.
- Where the worsening sits: in the last hours of the window on every worse day (DOY 128 hours 4 to 6, DOY 131 hours 5
  and 6, DOY 132 hours 4 and 6, DOY 130 hours 3 to 6), while hours 2 and 3 of the three new days stay within 4 mm. The
  controls hand their GPS pivot from G01 to G02 in hour 4 of the new days and in hour 5 of DOY 129 and 130; on DOY 130
  the worsening starts in hour 3, before that handover, so the tie is not read as established. ARHT00ATA tracks the
  same satellites in both replays on every new day and on DOY 130 (no satellite's row count differs by more than 20 in
  any hour), so the change is not a satellite gained or lost at the site.
- Everywhere else the election is neutral or better on every day: no other census site is worse by more than 1 cm on
  any of the five days, the census paired median is within 0.2 mm of zero, WTZZ00DEU is 0.8 to 1.9 mm better on the
  new days, and every election arm runs without a pivot handover against one or two per control (43 to 59 % fewer
  clock-anchor event rows on the new days).
- Open: every 6 h number is the window 00:00 to 06:00 UT (a whole day only on DOY 129, -6.9 mm); the mechanism of
  ARHT00ATA's response is not read; the posture is the `ef9f75d` golden, not the current one. Not armed; adoption, and
  whether a site that fails on two days of five holds it, is the author's decision.

### 11.4 Side finding: G01's return on DOY 128 and 131

On both days G01 leaves both wires for a few minutes (DOY 128 from 03:28:00 to 03:36:00, DOY 131 from 03:16:00 to
03:22:30), and the control hands its GPS pivot from G01 to G02 inside that gap. When G01 returns, the control emits it
at -110.7 m (DOY 128) and -102.7 m (DOY 131) from the a priori product, then withholds it except for one record
(-88.6 m at 05:40:00, -90.1 m at 05:31:00). The election wire, whose pivot was never G01, emits G01 on its return
within 0.6 m of its level before the gap (-0.49 to -0.97 m on DOY 128, +0.88 to +0.35 m on DOY 131) and keeps
emitting it (178 and 202 clock records that the control lacks). ARHT00ATA's consumer confirms the election's level: in
the ten minutes after the return, G01's code innovation is -1.46 to +0.33 m (DOY 128) and -0.73 to +0.54 m (DOY 131)
with the election wire and no ambiguity reset, against -5.33 to +0.73 m and -9.57 to +0.43 m and three ambiguity
resets with the control wire, whose 100 m step the consumer absorbs into reset ambiguities. The 85 to 112 m G01
departure that card MDAY section 6.2 reads as "a property of the product day" is therefore a product of the legacy
pivot path (the pivot handed away from G01 while G01 is out, then G01's return), not of the product day, and it does
not occur under the election. Not read further here: the defect in the legacy path is a separate item, and
ARHT00ATA's two smallest worsenings (+3.0 and +7.6 mm) fall on these two days.

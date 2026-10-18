# PIV: the clock pivot elected by predicted station coverage, health and shadow

**STATUS**

- **id:** PIV
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-18, default off, never armed: `network.pivot_election: coverage` elects each constellation's clock pivot at its first epoch and at a forced handover as the registered, healthy satellite that the orbit product predicts at the most roster stations over a look-ahead horizon, passing over one that enters the Earth's umbra inside it; an incumbent that stays registered and healthy is kept. Unit gate, ledger rows and a Release smoke are in (section 6); the check against same-exe controls on the field and sim goldens (6 h) is owed (section 5, bars frozen).
- **source:** [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) sections 3 to 5 (G14 pinned cut the field hour-2 common drift from -0.180 to -0.063 m/h); [HLT](HLT_broadcast_health_2026-09-25.md) (the health latch and the pivot's health skip); `docs/status/OPEN_ITEMS.md` queued build 1
- **knobs / posture:** `network.pivot_election` (`off` | `coverage`), `network.pivot_election_horizon_s` (7200), `network.pivot_election_step_s` (300); ledger rows UNVALIDATED and PARAM; neither golden changed
- **dates in the text:** 2026-10-17, 2026-10-18
- **hashes / run dirs:** branch `mpg/pivot`; scratch `D:/GnssProcessorSupport/gates/pivot/` (`tools/offline_rank.py`, build logs, smoke output); unit gate `tests/network/test_pivot_election.cpp` (suites `CoveragePivot*`)

**Outcome.** Question: can the clock pivot be chosen by what the network can use of it (stations that track it, a
healthy broadcast record, no umbra entry in the coming hours) instead of by its number? Built as a default-off
election that runs only at ignition and at a forced handover and otherwise stays sticky. Unit gate and a Release
smoke pass (10 epochs, field and sim): the field run elects G22 (27.6 of 58 stations on average over 7200 s; runner-up
G14 27.4) and E04 (28.7), with G01 passed over as unhealthy, where the control keeps G01 and E02; with the knob absent
the output equals a main-built exe's. Nothing is armed; the 6 h check against same-exe controls is owed.

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

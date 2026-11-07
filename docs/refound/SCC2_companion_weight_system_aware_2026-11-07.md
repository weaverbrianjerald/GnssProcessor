# SCC2: the second-code weight, matched on the constellation as well as the signal

**STATUS**

- **id:** SCC2
- **topic:** code-bias
- **status:** closed
- **summary:** BUILT and GATED default off on branch `worktree-wf_c49deb5b-2f3-3` (nothing armed). The station split divided by 100 every code row whose observation code matched a receiver inter-signal state of its station, whatever its constellation: 8,051 Galileo C1C rows at CHUR00CAN and STJO00CAN in hours 2 to 6 of the field baseline, the whole fallback count of 9,447. `network.second_code_system_aware` matches on (constellation, signal). Off byte-identical (2 h and 6 h field, 2 h sim); on restores exactly those rows, sim identical, census median -0.2 mm; G4 FAILED as frozen at hour 3 (WTZZ00DEU 3.3 mm better, outside the two-sided 2 mm bar). Arming is the author's decision (section 6). Verification (section 7): every number reproduced exactly.
- **source:** card [SCC](SCC_second_code_channel_2026-09-21.md) (R2, R5, R7: the inter-signal state and its scale); review card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, F1) and its second-model verification `D:/GnssProcessorSupport/srn/wave2_verdicts.json`, finding D2-03 (code lens); `src/core/network/assembly/StationSplit.cpp`, `src/core/network/assembly/SecondCodeScope.cpp`, `src/core/measurement/ReceiverBias.cpp`
- **knobs / posture:** new `network.second_code_system_aware` (bool, default false, fingerprinted when it acts); read, not changed: `network.second_code_channel` (armed), `network.second_code_sigma_scale` (10 in both goldens, a census setting of card SCC), `network.second_code_signals` (default C1C, C1W). Base: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `284ec29`.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** pre-registration `8dda7f2`, code `7218470`; baseline `runs/_rf0hv17_field6` (C:); review executable `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`) with its controls `_rap_srn_ctl_{f2,s2,f6}`; changed exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_scc2.exe` (the Release build of `7218470`, sha256[:16] `1438852627f74872`); run root `D:/GnssProcessorSupport/runs_nom`: arms `_rap_scc2_{off,on}_{f2,s2,f6}`, smoke `_rap_scc2_smoke_on`, census replays `_rap_scc2c_{off,on}_<site4>` (both on the off arm's consumer configuration)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: does the station split's second-code row test take rows of a constellation that holds no
inter-signal state? Yes: at CHUR00CAN and STJO00CAN every Galileo C1C row took the GPS companion's divide-by-100,
8,051 rows in hours 2 to 6 and the whole run-end fallback count. Keyed on (constellation, signal) behind
`network.second_code_system_aware`: off byte-identical; on restores exactly those rows (fallback 9,447 to 0), the sim
is identical, and the census median moves -0.2 mm. The Galileo C1X biases and between-satellite clocks move (E36 8.9
and 21 mm). G4 failed as frozen: WTZZ00DEU hour 3 is 3.3 mm better, outside the two-sided bar. DAV1 is 7.7 mm worse.
Standing: UNVALIDATED; arming is the author's decision.

## 1. The defect, as read

The receiver bias resolver (`src/core/measurement/ReceiverBias.cpp`, the second-code block) gives a band-1 code its
own receiver state when the code is allow-listed and is not the station's latched canonical code for that
constellation and band digit. The state's key is `RxCbKey{sys, ParamForm::Isc, signal}`, text `<system>_ISC_<signal>`,
so the state is per constellation: a station whose GPS canonical is C1W holds `GPS_ISC_C1C`, and its Galileo C1C rows,
when Galileo's canonical is C1C, route through the Galileo per-band receiver bias like any canonical row.

The station split (`src/core/network/assembly/StationSplit.cpp`, block "Second-code rows", main `284ec29` lines 528 to
555) reads the states rather than re-deriving them, which is right, but it reads them through a set of observation
codes:

```cpp
isc_sigs.insert(entry.first.signal.obsCode(ObsType::Code));   // no constellation
...
if (!isc_sigs.count(sig)) continue;                            // any constellation's C1C
```

Every matched row is then tagged `DataCodeCompanion`, counted as a companion (if its (satellite, signal) cell is open)
or as a fallback row (otherwise), and divided by `second_code_sigma_scale^2` (100 on the goldens). A Galileo C1C row at
a station holding `GPS_ISC_C1C` matches: its cell is never open (no Galileo companion exists), so it is counted as a
fallback row and enters at 3 m over sin(el) instead of 0.3 m.

The other second-code readers are constellation-correct: the PASS A sweep keys its cells on
`StateKeys::makeSatBiasKey(satellite, signal)` and the resolver keys the state on the system. The defect is confined to
the split's weight, tag and counter. Under `processing.channel_mode: all` the same block runs (the predicate is
`networkSecondCodeRowsActive`) and has the same blind spot.

## 2. Confirmation on the field baseline

Instrument: `runs/_rf0hv17_field6/logs/network_pf_rows.csv` (columns week, tow, station, sat, signal, kind, resid_m, w,
elev_deg), hours 2 to 6 (tow 262800 to 280770). The elevation law of a code row is w = sin²(el) / 0.3²; a row is
classed "divided" when w lies between 0.005 and 0.02 of the law, "at the law" between 0.5 and 2. The GPS companion of
a station is the band-1 GPS code whose rows are divided. Every Galileo C1C row of the run falls in one of the two
classes (none other).

| station | GPS companion | Galileo C1C rows divided by 100 | Galileo C1C rows at the law |
|---|---|---:|---:|
| CHUR00CAN | C1C | 4,051 | 0 |
| STJO00CAN | C1C | 4,000 | 0 |
| the other 16 stations with GPS companion C1C | C1C | 0 (no Galileo C1C rows: Galileo tracked as C1X) | 0 |
| the 22 stations with GPS companion C1W | C1W | 0 | 81,255 |
| the 18 stations holding no inter-signal state | none | 0 | 14,349 |
| total, 58 stations | | **8,051** | 95,604 |

18 stations hold the GPS C1C companion role and 22 the C1W role, matching the run's "signals as planned" line (GPS
band 1 companion C1W 22, C1C 18) and the verification's count. The run-end line of the second code channel reads
"9447 row(s) took the FALLBACK path"; the contributions table counts 244,658 `data_code_companion` rows against
235,211 companion rows in the family census. The difference, 9,447, is exactly the fallback count: on this posture
the GPS fallback population is empty and the whole fallback count is the Galileo C1C rows of the two stations over the
six hours (8,051 of them in hours 2 to 6). Card SCC's reading of a nonzero fallback count, "a finding about the latch",
does not apply to them: they are canonical rows of another constellation.

## 3. Design

**Key.** `network.second_code_system_aware` (bool, default false), section `network`, parsed beside
`second_code_sigma_scale` in `loadNetworkSection_` (`src/core/config/GnssConfigLoadNetwork.cpp`); getter
`GnssConfig::networkSecondCodeSystemAware()`. A load-time info line when it acts, a warning when it is set without
second-code rows (inert, kept as configured). Fingerprinted only when it acts (`scc:sysaware` appended in
`networkSnapshotFingerprint`), as the scale is: it decides which rows were accumulated at the scale.

**The test, as one pure function.** `secondCodeIscRows(rx_cb, satellite_ids, signal_types, n_rows, system_aware,
other_system)` in `network/assembly/SecondCodeScope` returns the row indices that route through an inter-signal state.
It walks the rows exactly as the split always has (rows up to min(n_rows, signals), phase and empty signals skipped,
stop at the first matched row beyond the satellite ids, skip a matched row with no satellite id). With
`system_aware` false it matches the observation code alone (the historic row set, byte-identical); true, it also
requires the (constellation of the satellite, observation code) pair to be one of the station's `Isc` keys. The
constellation comes from `systemOfSat` (no `sat[0]` parsing). The aware form only removes rows from the unaware set.

**The split.** The block calls the function and keeps its body: tag, companion or fallback count, and the division by
`scale^2`, over the returned rows. The arithmetic per row is unchanged, so the off path is the same floating-point
operation on the same rows.

**Telemetry (standing invariant 4).** A per-station counter `scc_other_system_rows` (rows the code-alone match takes and
the pair match refuses) is counted in both forms and summed into one new run-end line of the second code channel's
report: off, it states how many rows took the scale only by sharing a code; on, how many kept their own weight. On the
field baseline the line reads 9,447 in both forms (prediction).

**Why `NetworkKnobs` carries no member.** The split runs inside the parallel station produce and reads the
configuration through `StationBuild::config` (`const GnssConfig&`), as its neighbours do
(`networkSecondCodeSigmaScale`, `networkExtraBandCodeSigmaScale`). A `NetworkKnobs` snapshot member would have no reader.

**The scale itself is not changed.** The verification measured the companion's unscaled standardized residual at 0.52
against 0.40 for the canonical row; the factor 10 is card SCC's census setting (armed 2026-09-22 at the author's
decision), not a noise statement, and this card leaves it.

**Expected move (key on).** At CHUR00CAN and STJO00CAN the Galileo C1C rows return to the elevation law (a factor 100
in weight): their information on those stations' Galileo receiver code bias and clock, and on the Galileo C1C satellite
code biases they support, rises. Two of 58 stations, one code family: the Galileo C1C B records may move at the
millimetre level, the clocks and the user should not move beyond noise. The tag of those rows changes from
`data_code_companion` to `data_code` and the fallback count falls from 9,447 to 0. The sim is inert by construction
(one code per band; no inter-signal state exists).

## 4. Gates (frozen)

- **G0 (tests).** New suite `SecondCodeSystemAware` (`tests/network/test_second_code_system_aware.cpp`): a station
  with GPS C1C companion and a Galileo C1C row (off divides both, on the GPS row only), a C1W-companion station
  (unchanged), a Galileo state of its own honoured, the walk's bounds, the loader. Full Debug suite: 0 failures.
- **G1 (off identity).** Key off on `gnss_processor_scc2.exe` against the review executable's controls
  `_rap_srn_ctl_f2` and `_rap_srn_ctl_s2`: `products.txt` with `#` lines stripped identical, every pre-existing
  `logs/*.csv` identical.
- **G2 (the trace).** Key on, field 2 h and 6 h against a same-executable control: the Galileo C1C rows of the two
  stations at the elevation law (0 rows divided), every other code row's weight unchanged, the run-end fallback count 0.
- **G3 (products).** Field 6 h, hours 2 to 6, arm minus control: the median difference per satellite of the Galileo
  B records C1C and C1X reported; the clocks (C records) median difference reported.
- **G4 (user).** WTZZ00DEU `pos_err_3d_m` p50 by hour within 2 mm of the control in every hour 2 to 6 (2 h and 6 h).
- **G5 (census).** The 16-site census on the 6 h wires: median within 0.5 mm of the control's.
- **G6 (sim).** Key on, sim 2 h: `products.txt` identical to the same-executable control.

A failure of G4 or G5 is reported as a failure; the card then does not recommend arming.

## 5. Results (2026-11-07)

Build: the Release and Debug builds of `7218470` in the card's worktree; exe `gnss_processor_scc2.exe` (section
header). Arms built by `scripts/rapid_arm.py make` from the two goldens of `284ec29`; the off arms' `arm.json` differ
from the review executable's controls in `network.product_output_path` only. The machine was shared with other
sessions (a 2 h field arm took 35 minutes).

**G0 (tests): PASS.** Suite `SecondCodeSystemAware`, 10 cases, all pass with the 29 cases of
`NetworkSecondCodeScope`. Full Debug `gnss_tests.exe`: 3,843 cases in 524 suites, 3,810 passed, 33 skipped
(fixture-gated), 0 failed. `scripts/nom_epoch_lint.py`: PASS. `scripts/make_golden_config.py --verify`: PASS (the new
row is UNVALIDATED, so no golden carries a gap). A 10-epoch smoke with the key on (`_rap_scc2_smoke_on`): exit 0, the
load line and the run-end line present, 144 rows left at their own weight, fallback 0.

**G1 (off identity): PASS.** Key off on `gnss_processor_scc2.exe` against the review executable:

| arm | control | `products.txt` (`#` lines stripped) | `logs/*.csv` |
|---|---|---|---|
| `_rap_scc2_off_f2` | `_rap_srn_ctl_f2` | identical, 113,888 lines | 31 of 34 byte-identical; the other 3 identical once their wall-clock columns are removed |
| `_rap_scc2_off_s2` | `_rap_srn_ctl_s2` | identical, 80,690 lines | the same 3 files, identical without wall-clock columns |
| `_rap_scc2_off_f6` | `_rap_srn_ctl_f6` | identical, 343,049 lines | the same 3 files, identical without wall-clock columns |

The three files are `network_datum_graph.csv` (column `ms`), `network_run_summary.csv` (`loop_ms`, `ms_per_epoch`)
and `network_service_health.csv` (`ms_per_epoch_window`): timing, which differs between any two runs. The only new
output of the off path is the run-end line of section 3 ("row test by code only: 9447 code row(s)" on the 6 h field
arm, 2,945 on the 2 h, 0 on the sim).

**G2 (the trace): PASS.** `logs/network_pf_rows.csv`, every code row of the arm matched to the control's by (epoch,
station, satellite, signal):

| window | code rows | weight unchanged (bit-equal) | weight x 100 |
|---|---:|---:|---:|
| 2 h, all epochs | 476,976 | 474,031 | 2,945 (CHUR00CAN 1,378, STJO00CAN 1,567), all Galileo C1C |
| 6 h, hours 2 to 6 | 1,199,430 | 1,191,379 | 8,051 (CHUR00CAN 4,051, STJO00CAN 4,000), all Galileo C1C |

The x 100 rows are exactly the rows of the table in section 2; no other code row's weight moved by a bit. Run-end
(6 h): fallback rows 9,447 to 0, companion rows 235,211 in both, `data_code_companion` contributions 244,658 to
235,211, the new line 9,447 in both forms.

**G3 (products), field 6 h, hours 2 to 6, arm minus control, 599 epochs.** Reported, no bar.

| record | per-satellite median of the difference |
|---|---|
| B Galileo C1C (25 satellites) | median -0.016 mm, largest 0.27 mm (E36) |
| B Galileo C1X (25) | median +0.007 mm, largest 8.93 mm (E36); E12 +4.68, E04 +4.63, E03 +4.48, E08 -3.94; 1 key above 5 mm |
| B Galileo C5Q / C5X | largest 0.33 / 0.24 mm |
| B GPS, every signal (96 keys) | largest 0.70 mm |
| C GPS (32) | common mode +0.90 mm; between satellites largest 1.27 mm (G32), median 0.10 mm |
| C Galileo (25) | common mode -17.97 mm (per epoch -21.5 to -10.8); between satellites largest 21.4 mm (E36), median 4.5 mm |

The 2 h arm reads the same way over its one settled hour (C1X largest 14.5 mm at E36, Galileo clock common mode -16.0
mm, between satellites largest 33.6 mm at E36). The Galileo common mode is the level the receiver inter-system bias
absorbs at a consumer; the between-satellite moves are the restored code information. The two stations carry, with the
key on, 14 to 22 % of the band-1 Galileo code weight of E03, E05, E15 and E34 in hours 2 to 6 (0.2 to 0.3 % off);
E36, tracked by 11 stations, moves most although the two stations carry 0.9 % of its weight: it is the satellite least
determined by code, so the redistribution through the shared receiver states reaches it first. Why the C1X family
moves and C1C barely does is not traced here (the run declares the Galileo ionosphere-free pin per satellite, three
pairs; which pair holds which satellite was not read).

**G4 (user, WTZZ00DEU `pos_err_3d_m` p50 by hour, arm minus control): FAILED as frozen (6 h, hour 3).**

| run | hour 1 | hour 2 | hour 3 | hour 4 | hour 5 | hour 6 |
|---|---:|---:|---:|---:|---:|---:|
| 6 h control (m) | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 |
| 6 h arm (m) | 0.0584 | 0.0128 | 0.0198 | 0.0116 | 0.0171 | 0.0132 |
| 6 h arm minus control (mm) | -0.40 | +0.50 | **-3.30** | -0.65 | +0.55 | +1.15 |
| 2 h arm minus control (mm) | -0.40 | +0.50 | | | | |

The bar was two-sided (within 2 mm in every hour 2 to 6). Hour 3 is outside it in the user's favour; no hour is worse
by more than 1.15 mm. The census replay at WTZZ00DEU reads the same (hour 3 0.023 to 0.020 m).

**G5 (16-site census, 6 h wires, consumers on the off arm's configuration): PASS.** Paired change of each site's mean
hourly p50 over hours 2 to 6: median -0.2 mm (bar 0.5 mm), 0 sites better and 0 worse at 1 cm. Median site p50 by hour:
control 0.142 0.050 0.036 0.029 0.032 0.030 m, arm 0.133 0.048 0.036 0.029 0.032 0.031 m. Per site (hours 2 to 6): DAV1
+7.7 mm (hour 2 0.218 to 0.236 m, settling to +3 mm by hour 6), BAKE +2.0, ANMG +0.8; CEDU -1.2, HARB -1.1, QUIN
-0.9; the other ten within 0.5 mm. Card NSP's noise floor (a neutral change moves no site by more than 0.19 mm) makes
DAV1's and BAKE's moves real responses, not noise. Hour 1, paired: median +1.5 mm, 2 better and 3 worse at 1 cm.

**G6 (sim): PASS.** `_rap_scc2_on_s2` against `_rap_scc2_off_s2`: `products.txt` identical (80,690 lines), every
`logs/*.csv` identical apart from the three wall-clock files (identical without those columns); the run-end line reads
0 rows in both forms. Inert by construction, as predicted (one code per band, no inter-signal state).

## 6. Verdict and what is owed (2026-11-07)

The defect is real and the fix does exactly what it states: the 8,051 rows return to the elevation law and nothing
else is reweighted. Off is byte-identical on both beds. The products move where the restored code information acts
(Galileo C1X biases and Galileo clocks between satellites, millimetres to 2 cm at E36), the GPS products by at most
1.3 mm.

By the rule frozen in section 4 the card does not recommend arming on its own gates: G4 failed as frozen. The failure
is in the user's favour (hour 3 3.3 mm better, every other hour within 1.15 mm), and the census passes at -0.2 mm, with
one site (DAV1) 7.7 mm worse over hours 2 to 6 and two (CEDU, HARB) about 1 mm better. Read as a correctness fix, the
case for arming is that the off posture weights two stations' canonical Galileo code at 3 m over sin(el) by accident
of a shared observation code, and the key removes that accident without touching the scale of card SCC. Arming, and
whether G4 is read as a one-sided no-harm bar, is the author's decision; the ledger row stays UNVALIDATED until then
and turns RECOMMEND_SCOPED (the second code channel's regime) with the arming.

Owed:
- DAV1: the 7.7 mm hours-2-to-6 response is not traced (DAV1 is not one of the two stations; a Galileo clock or C1X
  bias of a satellite it tracks is the likely route).
- A multi-day or 24 h arm before arming (the 6 h window holds one geometry of the two Canadian stations).
- The record of card SCC reads its nonzero fallback count as "a finding about the latch". On this posture the whole
  count was Galileo C1C rows of these two stations (section 2); the annotation belongs to that card's next results
  heading, not to its body.
- Under `processing.channel_mode: all` the same test applies (the predicate is shared); that mode was not run here.

## 7. Second-model verification (2026-11-07)

One Opus referee, read-only, reproduced every decision-driving number exactly from the run directories (ten claims confirmed): the off identity on f2, s2 and f6; the 8,051 rows changing weight by 100 (CHUR00CAN 4,051, STJO00CAN 4,000), all Galileo C1C, with the fallback count 9,447 to 0; WTZZ00DEU -0.40, +0.50, -3.30, -0.65, +0.55, +1.15 mm by hour; the census (median -0.195 mm, DAV1 +7.73 mm); the sim inert; the Galileo clock and C1X moves; the pure function, its call site, the default, the fingerprint tag and the reading of the key through `StationBuild::config` like its neighbouring scale keys; the ledger row, the golden verify and the test suite. The frozen two-sided rule is applied as written and the DAV1 response is reported as untraced, so the verdict stands.

Items the referee recorded that the card did not: (1) "bit-equal" for the other rows is text equality at seven significant digits in `network_pf_rows.csv`; bit identity follows from the unchanged arithmetic, not from the trace; (2) hours 2 to 6 hold 600 epochs in `products.txt`, and the card's strict window counts 599 because E-record times carry float noise; (3) the E36 C1X move is -8.93 mm and its clock move -21.41 mm (signs omitted in section 5); (4) the test `OffDividesBothC1CRowsOnDividesTheGpsRowOnly` checks a test-local copy of the split arithmetic rather than `StationSplit`, and no test covers the `scc:sysaware` fingerprint tag; (5) the tag is appended under `networkSecondCodeRowsActive()`, which includes `channel_mode all`, while the sigma scale it accompanies applies under `networkSecondCodeChannel()` only: safe, since it only over-declares; (6) the census has 17 replay pairs on disk including WTZZ00DEU, and the 16-site figures reproduce with WTZZ00DEU excluded, which the card does not say; the hour-1 notes leave out the large single-site moves (DAV1 -82 mm, KOKB +93 mm); (7) the unit-suite counts cannot be checked from artifacts.

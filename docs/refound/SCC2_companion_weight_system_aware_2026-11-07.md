# SCC2: the second-code weight, matched on the constellation as well as the signal

**STATUS**

- **id:** SCC2
- **topic:** code-bias
- **status:** open
- **summary:** Pre-registered 2026-11-07. The station split divides by 100 the weight of every code row whose observation code matches a receiver inter-signal state of its station, whatever the row's constellation; a Galileo C1C row beside `GPS_ISC_C1C` therefore takes the GPS companion's scale. Defect confirmed on the field baseline (8,051 rows in hours 2 to 6). Fix behind `network.second_code_system_aware` (default off).
- **source:** card [SCC](SCC_second_code_channel_2026-09-21.md) (R2, R5, R7: the inter-signal state and its scale); review card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, F1) and its second-model verification `D:/GnssProcessorSupport/srn/wave2_verdicts.json`, finding D2-03 (code lens); `src/core/network/assembly/StationSplit.cpp`, `src/core/network/assembly/SecondCodeScope.cpp`, `src/core/measurement/ReceiverBias.cpp`
- **knobs / posture:** new `network.second_code_system_aware` (bool, default false, fingerprinted when it acts); read, not changed: `network.second_code_channel` (armed), `network.second_code_sigma_scale` (10 in both goldens, a census setting of card SCC), `network.second_code_signals` (default C1C, C1W). Base: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `284ec29`.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** baseline `runs/_rf0hv17_field6` (C:); review executable `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`) with its controls `_rap_srn_ctl_{f2,s2,f6,s6}`; changed exe `gnss_processor_scc2.exe`; run root `D:/GnssProcessorSupport/runs_nom`, arms `_rap_scc2_*`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered. Question: does the station split's second-code row test take rows of a constellation that
holds no inter-signal state? Yes: the test collects the observation codes of the station's `<system>_ISC_<signal>`
states without the constellation, so at CHUR00CAN and STJO00CAN (GPS companion C1C, Galileo canonical C1C) every
Galileo C1C row is divided by 100, tagged a companion and counted as a fallback row: 8,051 rows in hours 2 to 6, and
the whole run-end fallback count of 9,447. The fix keys the test on (constellation, signal) behind a default-off key.
Gates: section 4.

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

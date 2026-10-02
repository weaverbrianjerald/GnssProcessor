# RPD: the within-epoch RAIM replay re-added each station without its pre-PD datum rows

**STATUS**

- **id:** RPD
- **topic:** raim-qc
- **status:** closed
- **summary:** RESTORE PATH HARDENED 2026-10-01 (section 7): a re-solve that fails is undone to the last landed state (exactly: the culprit sets, the detail and the WE-CI choice counters), attributed to the station the replay was re-adding, counted and evented; the undo leaves out, for the epoch, a station it cannot re-add (the primary assembly's own rule, at most five), and only an undo that still cannot complete skips the epoch, after the culprit-set aliases are retired. Replayed against this card's own defect on the outage arm, the unhardened restore ran 30 epochs on a partly rebuilt estimator and a skip-only first build withheld 80 (no harvest, so no recovery); the build as committed fails twice, leaves the returning station out of those two epochs and emits all 240. An injection test exercises every branch (undo bit-identical to the landed state; culprit-free undo 7.7 µm from the primary; a station dropped; the shared-stage and budget skips). FIXED 2026-10-01. Under `network.station_prior_before_pd` (card RF-PD-fix, armed on both goldens) a station's RXCB-DATUM rows reach the primary solve only through the `StationPrior` that `addStation` folds before its positive-definiteness test; the within-epoch RAIM replay (`resolve_dropping`) re-added every station without it, so no replay carried a receiver-DCB datum row. Two failure modes, both measured with a temporary diagnostic (section 2). (1) Where a station's block is positive-definite only with that row (the first assessed epoch, and the first epoch back of a station returning without a carry), the replay is rejected at that station, which aborts the whole replay, and the restore replay fails the same way: on a forced-adapt field arm with a 20-minute station outage this repeated on 30 consecutive epochs, the estimator holding 7 of 57 stations, and the run-end counters recorded none of it. (2) Elsewhere the replay admits every station and solves without the datum: with no culprit at all it misses the solve it replays by up to 7.9 mm (field) and 10.6 mm (sim) in the phase-bias columns, and on forced arms the products diverge from the first adapted epoch (field between-satellite clock 2 mm, phase bias 10 mm). The fix passes the same `StationPrior` at all three re-add sites; the fixed replay reproduces the primary solve to 8 µm and was never rejected. Gates: byte-identical on the four PRM arms (145 of 145, where the tier does not adapt), the committed exe identical to the diagnostic's fixed mode on five forced arms, the unit suite passes. Latent on the goldens as armed (σ̂0 sits at its floor of 1.0 against a trigger of 3.0; the 6 h baselines `_rf0hv11_{field,sim}6`, `_rf0hv12_{field,sim}6` and `_rf0hv13_sim6` never adapted); card RF-PD-fix's own field arms adapted on 14 epochs, each re-solved without the rows.
- **source:** [RF-PD-fix](RF-PD-fix_station_prior_before_pd_2026-09-15.md) sections 1.1 and 1.2; [RF-PD](RF-PD_station_rejection_2026-09-15.md) section 5 (the null the row cures); [PRM](PRM_parameter_registry_2026-09-29.md) section 13 (the A/B harness and its references); found while building card [LGM](LGM_los_phase_nuisance_2026-10-01.md)
- **knobs / posture:** no new knob; the forced arms set `network.raim_sigma0_trigger` 0.5 and `network.raim_k_sigma` 3.0 or 1.5 on the goldens (diagnostic arms only); no golden, ledger, posture or loader default changed; section 7 adds no knob (the injection switches were temporary)
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** main `1b2b081`; exes `build/x64-Release/gnss_processor_rpd_diag.exe` `3cc905390dcd3e37` (the diagnostic, not committed) and `gnss_processor_rpd1.exe` `0005034ac63bd236` (the fix as committed); arms `runs/_rap_rpd_*` and `runs/_rap_prm_rpd_{gf,gs,pf,ps}`; section 7: main `cf1c20a`, exes `gnss_processor_rpd7_inj2.exe` `c56d9a9ad4cdafc3` (the injector, not committed) and `gnss_processor_rpd7b.exe` `a9a6654048dd8b4d` (section 7 as committed), arms `runs/_rap_rpd7*` and `runs/_rap_prm_rpd7b_{gf,gs,pf,ps}`

## 1. The defect, read from the code

The primary solve adds each station with its own pre-PD datum rows
(`src/core/app/ApplicationNetworkAssembly.cpp:5471`, `StationPrior{R.prior_cols, R.prior_dNrr,
R.prior_dur}`). Under `network.station_prior_before_pd` the RXCB-DATUM row is routed to those vectors
and withheld from `R.carry_cols` (the exclusive emplace at `:4877`), so nothing after `addStation`
carries it.

The within-epoch replay, `resolve_dropping` (`src/core/filter/NetworkSolveGlue.cpp:1654`), rebuilds
the epoch over a fresh `beginEpoch()` and re-adds each station at one of three sites (the joint
coupling re-whiten, the local-block refloat, the plain re-add) through the four-argument `addStation`,
whose `prior` defaults to empty (`include/filter/NetworkEstimator.hpp:177-184`), then folds
`R.carry_cols` through `addStationPrior`. No replay carried a receiver-DCB datum row. Two consequences
follow from the code:

1. A station whose block is positive-definite only with that row is rejected (card RF-PD section 5:
   one null, the receiver DCB absorbed by the slant ionosphere and the ambiguities, which otherwise
   only an anchor pin row or a carried ambiguity row in `A_r` breaks). The replay has no per-station
   catch, so the rejection leaves `resolve_dropping`; the adaptation loop logs `re-solve failed ...
   restoring clean solve` and replays again with no culprit (`:2043-2054`), which is rejected at the
   same station and swallowed. The estimator is left holding the stations re-added before the throw,
   `x_s` keeps the primary solution, and `n_adapt` is not incremented, so the run-end T2 line does not
   count the attempt.
2. Otherwise every station is admitted and the replay solves a system without the datum rows the
   primary solve had: the within-epoch decision is taken against a different system from the one
   under test.

The tier runs only under `network.raim_within_epoch` with the carry warm (`:1415`) and adapts only
when σ̂0 exceeds `network.raim_sigma0_trigger` (3.0) and a culprit's effect exceeds
`network.raim_k_sigma` (6.0) times σ̂0. On both goldens σ̂0 sits at its floor of 1.0 on every assessed
epoch (`sigma0_first` in `runs/_rap_prm_g0a_*/logs/network_epoch_status.csv`), so the replay does not
run there.

## 2. The instrument (temporary, not committed)

Main `1b2b081` plus the fix, with three environment switches in the within-epoch block.
`GNSS_RPR_MODE=0` re-adds stations as before the fix (no prior); `1`, the default, passes it.
`GNSS_RPR_NULL` runs, on every assessed epoch before the first assessment, a culprit-free replay in
each mode and compares its shared solution with the primary solve (maximum and rms over all shared
columns, with the label of the maximum); the fixed mode runs last, so the estimator ends in its state.
`GNSS_RPR_DIAG` logs, for every adaptation re-solve, the stations re-added, the stations and rows of
pre-PD prior folded, and the station whose `addStation` threw. The null replay moves the estimator,
so its arms are read for its log lines only. The per-re-solve logging moves nothing: the committed exe
reproduces the diagnostic's mode 1 byte for byte on every forced arm (section 5). Mode 0 is the
replay before the fix, mode 1 the fix.

## 3. Results (2 h arms; the field from 00:00, the sim from 00:30:15)

### 3.1 The culprit-free replay on both goldens (`runs/_rap_rpd_null_{gf,gs}`)

| | field, 58 stations | sim, 60 stations |
| --- | --- | --- |
| assessed epochs | 234 | 235 |
| mode 0: rejected | 1, the first assessed epoch (TOW 259380): KRGG00ATF, after 3 stations | 1, the first assessed epoch (TOW 261240): S01000SIM, after 1 |
| mode 0: miss against the primary, p50 / p90 / max | 0.15 / 2.34 / 7.90 mm | 0.89 / 3.52 / 10.64 mm |
| mode 1: stations and rows folded | 58 and 58 on 234 of 234 | 60 and 60 on 235 of 235 |
| mode 1: miss, p50 / p90 / max | 0.07 / 0.28 / 8.1 µm | 0.23 / 0.51 / 1.0 µm |

The largest miss sits on a phase-bias column on every solved epoch, in both modes. Mode 1 is not
bit-exact: the primary solve folds some shared priors (the clock anchor among them) after the
null-space gate's reduce, which `NetworkNormalEquations::addSharedInfo` adds to the cached reduction
directly, while the replay folds them before its own reduction, so the two systems are summed in a
different order. Its residual is three to four orders below mode 0's miss and was not decomposed
further.

### 3.2 Forced adaptation (`runs/_rap_rpd_{frc,k15}{0,1,F}_{gf,gs}`)

`network.raim_sigma0_trigger` 0.5 puts the trigger below the σ̂0 floor, so the loop is entered on
every assessed epoch; `network.raim_k_sigma` 3.0 (`frc`) or 1.5 (`k15`) admits culprits. The same knob
sets the post-fit RAIM flag, identically in both arms of a pair.

| arm | re-solves (epochs), both modes | culprits settled | rows folded, mode 0 / mode 1 | rejections | between-satellite product difference over the run (biases per signal), p50 / max |
| --- | --- | --- | --- | --- | --- |
| field, k 3 | 30 (21) | 18 arcs, 12 stations | 0 / one per admitted station, 30 of 30 re-solves | 0 | clock 2.02 / 2.08 mm, code bias 1.16 / 1.48 mm, phase bias 2.98 / 10.08 mm, on 233 of 240 epochs |
| field, k 1.5 | 234 (89) | 160 arcs, 74 stations | 0 / one per station, 234 of 234 | 0 | clock 2.03 / 2.08 mm, code bias 1.50 / 2.92 mm, phase bias 3.03 / 10.08 mm |
| sim, k 3 | 0 | none | | 0 | none (33 of 33 files identical) |
| sim, k 1.5 | 10 (6) | 4 arcs, 6 stations | 0 / one per station, 10 of 10 | 0 | clock 0.03 mm, phase bias 0.06 mm, code bias below 1 µm, on 210 of 241 epochs |

The culprit sequence is the same in both modes (the first, at TOW 259410: arc BSHM00ISR/G05, arc
BSHM00ISR/G07, then the station excluded), so the difference is the system each re-solve solves. At
that epoch the clock moves 1.26 mm and the code bias 0.32 mm; on the field the difference then
persists through the carry (clock common mode 0.77 mm, between-satellite 2.0 mm, for the rest of the
run). The closed-loop consumer (WTZZ00DEU) moves at most 1.1 mm in 3D on the field and 0.1 mm on the
sim.

### 3.3 Forced adaptation across a station's return (`runs/_rap_rpd_out{0,1,N,F}_gf`)

The field k 1.5 arm with every observation type of four stations blanked for 20 minutes (longer than
`phase_leveling_max_gap_s` 900), staggered: KRGG00ATF TOW 261000 to 262170, SAVO00BRA 261900 to
263070, CUSV00THA 262800 to 263970, STJO00CAN 263700 to 264870 (`rapid_arm.py` windowed `strip`).

- Culprit-free replay (`outN`): mode 0 is rejected at each returning station on its first epoch back
  (KRGG00ATF at 262200 after 3 stations, SAVO00BRA at 263100 after 4, CUSV00THA at 264000 after 7,
  STJO00CAN at 264900 after 10) and on the first assessed epoch; mode 1 is never rejected, folds every
  station's row on 234 of 234 epochs, and misses by at most 4.8 µm.
- Mode 0 (`out0`): CUSV00THA's return coincided with an adaptation (arc BOGT00COL/G32). The re-solve
  was rejected at CUSV00THA after 7 stations, and so was the restore, on 30 consecutive epochs, TOW
  264000 to 264870. Over those epochs `network_rx_states.csv` holds receiver states for 7 stations per
  epoch (CUSV00THA none), against 56 to 57 in mode 1 and 56 on the epochs before: the estimator held
  the 7 stations re-added before the throw, and the other 50 stations' local states went
  unreconstructed and unharvested for 15 minutes. The returning station stayed without a carry and
  the culprit arc was never refloated, so the same failure recurred each epoch. The run-end T2 line
  (50 arcs, 32 stations) counts none of the 30; only the 30 `re-solve failed` warnings record them.
- Mode 1 (`out1`): the arc refloated in one re-solve at 264000 with 57 of 57 stations and 57 rows;
  no rejection over the run.

| 15 minutes from TOW | epochs with a landed re-solve, mode 0 / mode 1 | max product difference, clock / code bias |
| --- | --- | --- |
| 259200, 260100, 261000, 261900, 262800 | 13/13, 2/2, 5/5, 3/3, 10/10 | at most 2.30 / 1.91 mm |
| 263700 | 1 / 15 | 6.06 / 74.6 mm |
| 264600 | 0 / 14 | 9.55 / 95.5 mm |
| 265500 | 9 / 29 | 12.75 / 84.7 mm |

Over the run: mode 0 landed 82 re-solves on 43 epochs and failed 30, mode 1 landed 245 on 91 and
failed none. The held S-basis elected 13 cold values (emitted phase-bias steps declared to the
consumer) in mode 0 and 7 in mode 1. Every epoch emitted in both. The consumer moves at most 2.2 mm
in 3D before TOW 264600 and up to 8.7 mm after; its median over the last half hour is 0.028 m in mode
0 and 0.031 m in mode 1. The forced arms are not a grading posture (the over-active tier is the
larger effect there, section 6), so the fix is read as a correctness repair, not a consumer gain.

## 4. The fix

`src/core/filter/NetworkSolveGlue.cpp:1662-1673`: the station's `StationPrior{R.prior_cols,
R.prior_dNrr, R.prior_dur}` is built once per station in the replay and passed at all three re-add
sites (`:1750`, `:1804`, `:1806-1807`). The rows sit on receiver-DCB columns only (their one emplace,
`ApplicationNetworkAssembly.cpp:4877`), never on an ambiguity column, so the refloat edits cannot
touch them. With the knob off `R.prior_*` is empty, and `addStation` then takes no copy and folds
nothing (`NetworkNormalEquations.cpp:470-482`), so the replay is unchanged by construction.

Left as it is: the replay's missing per-station catch and the restore path (`:2043-2054`), which
leaves the estimator partly rebuilt if a replay throws. With the rows the replay re-adds the blocks
the primary admitted (the refloat edits keep each refloated column's data rows), and no fixed replay
was rejected in 519 adaptation re-solves and 703 culprit-free replays (sections 3.1 to 3.3).

ANNOTATED 2026-10-01: the restore path is hardened in section 7; the line references in sections 1
and 4 are those of commit `21d1e4f`.

## 5. Gates

1. **Byte identity where the tier does not adapt.** `python scripts/prm_ab.py make rpd`,
   `run rpd --exe build/x64-Release/gnss_processor_rpd1.exe`, `cmp g0a rpd --all-csv`: RESULT
   IDENTICAL, 145 of 145 comparisons over the four arms (golden and program postures, both beds). The
   tier adapted 0 times on all four (the T2 run-end line; no `re-conditioned` or `re-solve failed`
   line), which is the regime this gate certifies.
2. **The committed exe against the diagnostic's mode 1** on the five forced arms (`frcF` and `k15F` on
   both beds, `outF`): 33 of 33 files identical on each (`products.txt` without comments and every
   CSV, wall-clock columns dropped as in `prm_ab.py`). The mode 1 numbers of section 3 are the
   committed code's.
3. **Unit suite**, Debug `gnss_tests.exe`: 2752 tests from 350 test suites, 2751 passed, 1 skipped
   (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing fixture-gated skip), 0 failed. No
   case was added: the replay is a lambda inside the solve-glue seam with no unit seam of its own, and
   its instrument is the culprit-free replay of section 2.

## 6. Not examined

- The forcing itself moves the field consumer far more than the fix does: by half hour, the golden
  1.27 / 0.24 / 0.059 / 0.059 m (`runs/_rap_prm_g0a_gf`) against 0.78 / 0.18 / 0.077 / 0.032 m with
  the k 3 forcing (`frc1_gf`); the sim does not move. One bed, 2 h, mostly ignition, and the same knob
  moves the post-fit RAIM flag; not read further here.
- Mode 1's micrometre residual (section 3.1).
- Long beds with real outages: the 2 h goldens have no returning station, and a station returning
  without a carry is where mode 0 failed.

## 7. The restore path, hardened (2026-10-01)

### 7.1 What the restore did

When a re-solve threw, the adaptation loop's catch (`NetworkSolveGlue.cpp:2043-2054` at `21d1e4f`):

- cleared the three culprit sets and replayed with no culprit, which discarded every re-solve that
  had landed earlier in the epoch while `n_adapt` still counted them; the run-end response counters
  then read the cleared sets, σ̂0 for the A4 ceiling described the last landed state rather than the
  one emitted, and the WE-CI code de-weights stayed applied, so the replay was not the clean solve
  either;
- swallowed a failure of that replay, leaving the estimator partly rebuilt for the emit and the
  harvest (section 3.3: 7 of 57 stations reconstructed, 30 epochs);
- counted nothing (`n_adapt` counts landed re-solves only), and its warning named no station (the
  `addStation` rejection message names none).

### 7.2 What it does now

1. Each iteration keeps the last landed state: the culprit sets, the WE-CI de-weight set, the detail
   string and the two WE-CI choice counters, which are incremented at the choice
   (`NetworkSolveGlue.cpp:1959-1969`). A failed re-solve restores it and replays it
   (`:2077-2150`). For an iteration after the first, that replay repeats one that succeeded earlier
   in the epoch on the same inputs; for the first, it is the culprit-free replay of the primary solve
   (section 3.1). The adaptation stops there, and σ̂0, the counters and the events describe the
   landed state.
2. The replay names the station it was re-adding when it threw, or the shared system
   (`replay_fail_at` / `replay_fail_idx`, `:1660-1668`). Each failure is logged, counted
   (`we_resolve_failed`, with the first failure's TOW and station, `include/app/NetworkRunContext.hpp`)
   and evented (`T2_within_epoch`, `we_resolve_failed`).
3. The undo re-adds under the primary assembly's admission rule: a station it cannot re-add is left
   out of the epoch's final solve and the undo is repeated without it (`we_undo_sta_dropped`,
   event `we_undo_station_dropped`), at most `kMaxAdapt` (5) stations, the tier's own budget. The
   legacy pivot carries the datum, so its rejection is not survivable there, as in the primary. The
   drop is scoped to the epoch: the next primary solve rebuilds the estimator with the station in it.
4. An undo that still cannot complete (a throw in the shared system or the solve, a rejected legacy
   pivot, the budget spent) leaves a partial rebuild that no consumer may read. The epoch then takes
   the failed-solve disposition (no emit, no stash; status `we_restore_failed`, counter
   `we_restore_failed`, an event and an error line), after the block has retired its culprit-set
   aliases (`:2253-2274`); a return from inside the block would leave them dangling for the next
   epoch's primary `buildCarryDdPriors`.
5. Run end: a line of its own, `Network (T2 within-epoch re-solve failures)`, a warning when any
   count is non-zero (`NetworkTeardown.cpp:646-658`). The T2 response line keeps the shape that
   `scripts/cm_grade.py` parses.

### 7.3 Why the undo drops a station rather than skipping the epoch

The first build skipped the epoch whenever the undo failed. Replayed against this card's own defect
(the replay without the rows, on the outage arm of section 3.3) it skipped 80 consecutive epochs,
TOW 264000 to the end of the run: a skipped epoch harvests nothing and runs no post-fit tier, so the
returning station never got its carry, the culprit was never refloated, and the failure repeated
every epoch. The unhardened restore had run 30 epochs on a partly rebuilt estimator; the skip made
the defect loud but cost the rest of the run.

The build as committed drops the station instead. On the same arm (`runs/_rap_rpd7v2_s4_gf`) the
re-solve fails twice (TOW 264000, culprit arc BOGT00COL/G32; TOW 264030, culprit station
BOGT00COL), each undo leaves CUSV00THA out of that epoch's final solve, every epoch emits (240 of
240), and the run recovers on the next epoch. Against the fixed replay (`runs/_rap_rpd_out1_gf`) the
code bias then stays within 7.5 mm (95.5 mm for the unhardened restore) and the consumer within
1.1 mm; what remains is the datum-free re-solves of section 3.2, which the arm re-introduces on
purpose.

### 7.4 Injection test (temporary, not committed)

An environment-gated injector in the replay (exe `gnss_processor_rpd7_inj2.exe` `c56d9a9ad4cdafc3`)
throws while the replay re-adds its eighth station (a genuine partial rebuild), in a chosen attempt
or in the undo, on the field arm forced at `raim_k_sigma` 1.5, at TOW 259410, where three re-solves
land unperturbed (arc BSHM00ISR/G05, arc BSHM00ISR/G07, station BSHM00ISR). Arms
`runs/_rap_rpd7v2_{s1,s2,s5,s6,s7,s4,tie}_gf`.

| arm | injected | at TOW 259410 | over the run |
| --- | --- | --- | --- |
| S1 | re-solve 2 fails | undone to arc G05: 58 of 58 stations, solution identical to the landed one (largest difference 0.0) | 240 of 240 emitted; 1 failure |
| S2 | re-solve 1 fails | undone to the culprit-free replay: 58 of 58 stations, 7.7 µm from the primary solve | 240 emitted; 1 failure |
| S5 | re-solve 1 fails; the undo cannot re-add CUSV00THA | CUSV00THA left out, the undo completes: receiver states for 57 stations | 240 emitted; 1 failure, 1 drop |
| S6 | re-solve 1 fails; the undo fails in the shared stage | epoch skipped (`we_restore_failed`) | 239 emitted, the next epoch normal (57 stations); 1 failure, 1 skip |
| S7 | re-solve 1 fails; every undo fails at its eighth station | five stations left out (the budget), the sixth failure skips the epoch | 239 emitted; 1 failure, 5 drops, 1 skip |
| S4 | the replay without the rows, outage arm | (section 7.3) | 240 emitted; 2 failures, 2 drops |

Every count above matches its event rows in `network_events.csv` and the run-end line.

### 7.5 Gates

1. **Byte identity where no re-solve fails.** `python scripts/prm_ab.py make rpd7b`,
   `run rpd7b --exe build/x64-Release/gnss_processor_rpd7b.exe`, `cmp g0a rpd7b --all-csv`: RESULT
   IDENTICAL, 145 of 145 over the four arms. The five forced arms on the same exe
   (`runs/_rap_rpd7bF_{frc,k15}_{gf,gs}`, `runs/_rap_rpd7bF_out_gf`) against their section 3
   references (`rpd_frc1`, `rpd_k151`, `rpd_out1`): 33 of 33 files identical on each, and the new
   run-end line reads 0 failed re-solves on all five, so the hardening acts only when a re-solve fails.
2. **The injector build without injection against the build as committed** (`rpd7v2_tie_gf` against
   `rpd7bF_k15_gf`): 33 of 33 files identical, so the outcomes of section 7.4 are the committed
   code's.
3. **Unit suite**, Debug `gnss_tests.exe`: 2752 tests from 350 test suites, 2751 passed, 1 skipped
   (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing fixture-gated skip), 0 failed. The
   test binary does not link the solve-glue seam; the injection test of section 7.4 is its instrument.
4. **Build.** `src/core/app/NetworkRunContext.cpp` gets `/bigobj` in `CMakeLists.txt`, beside the
   existing per-file entries. The Debug build of main `cf1c20a` stops at that file with C1128 (number
   of sections exceeded) with or without this section's header change, which was checked by compiling
   it against HEAD's own header; card AIL's instrument (`0f9b2a5`, 80 lines added to the file) tipped
   it over. Release was unaffected.

Both section 7 exes were built while the shared tree also held a concurrent session's uncommitted
posture-table rows (`src/core/config/GnssConfigNetworkPosture.cpp`, card EXL); gate 1 covers them.

# SEA: the seasonal term of a SIF coordinate outside its block windows

**STATUS**

- **id:** SEA
- **topic:** consumer
- **status:** open
- **summary:** BUILT 2026-10-16, default behaviour (no knob): the periodic (seasonal) term of the SIF coordinate function is one block chosen by the epoch with the NGS M-PAGES v1.2 rule (the covering block; the first block before all windows, the last after all, the block that ended last inside a gap), so it no longer drops to zero where the linear term extrapolates. 8 stations of the field roster gain 0.4 to 6.4 mm; the covered stations are bit-identical. Owed: the same-build golden comparison (field and sim, 2 h), a separate stage.
- **source:** the register item "Build 5: SIF outside its window" ([OPEN_ITEMS](../status/OPEN_ITEMS.md), queued after NOM); NGS M-PAGES v1.2 `Lib/Station/Periodic.cpp`, `PeriodicMotion::computeDisplacement` (lines 148 to 200); [`StationInfoFile.cpp`](../../src/core/products/StationInfoFile.cpp)
- **knobs / posture:** none. No ledger row: the change is the intended behaviour of the register item, and no golden reaches the function (section 4)
- **dates in the text:** 2026-10-16
- **hashes / run dirs:** base main `4318fe3`, branch `mpg/seasonal` (code `4b4313a`); exe `gnss_processor_seasonal.exe` `ed32e9be281843c6`; unit gates `tests/test_station_info_seasonal.cpp` (suite `SifSeasonalTermTest`) and `tools/sif_propagate/test_sif_propagate.py` section 10; smoke runs under `D:/GnssProcessorSupport/gates/seasonal/`
- **gates (frozen at pre-registration):** section 6

**Outcome.** Question: should the seasonal coordinate term follow the linear term past the end of its block window, as M-PAGES v1.2 does? The old rule summed the blocks covering the epoch, so a station whose last segment had closed kept its linear extrapolation and lost the seasonal term at once (a step of up to 13 mm in the two bundled SIFs, 245 of 3,079 stations at the golden epoch). The rule is now M-PAGES's, with the diagnostics, warnings, a Python twin and the pyviz port moved in step. Covered epochs are unchanged to the bit. The unit gates pass; the golden identity check is the next stage.

## 1. The defect

`resolveStationCoordinate` formed the seasonal term as the sum over the periodic blocks whose
window covers the epoch (`start <= t < end`). The linear term does not stop at its occupation
segment: the nearest segment is extrapolated (with a warning) when none covers the epoch. The
periodic blocks end with the segments they were fitted with, so for a station whose last segment
has closed the linear term went on and the seasonal term was exactly zero, a step in the
coordinate at the window end. The roster path (`network.use_sif_coordinates`) and the PPP
a-priori (`processing.enable_station_info`) both take their coordinate from this function.

The field golden roster (58 stations, epoch 2024.3511, data_download year 2024 day 129) has 8
stations in this case, all past their last window: the table of section 3. The two bundled SIFs
hold 3,237 stations; at that epoch 2,834 are covered, 245 are past their last window (median
2.1 mm, 90th percentile 5.3 mm, maximum 13.3 mm at LCKI00IND and LCK200IND) and 158 have no
periodic block. None is before the first window or inside a gap at that epoch.

## 2. The rule

M-PAGES `PeriodicMotion::computeDisplacement` takes the blocks in start order and:

| epoch | block evaluated | `SifPeriodicReach` |
| --- | --- | --- |
| inside a window (`start <= t < end`) | the first such block, in start order | `Covered` |
| before the first window | the first block | `BeforeFirst` |
| at or after the end of the last window | the last block | `AfterLast` |
| between two windows | the last block, in start order, that ended at or before `t` | `InGap` |

and warns in the last three cases. The sinusoids continue past the window with the phase measured
from the block's own reference epoch, so the term is continuous where the last window ends. The
selection is by the epoch alone, independent of the occupation selection.

Differences from the old code that are deliberate (all in
`selectPeriodicBlock`, `src/core/products/StationInfoFile.cpp`):

- **One block, not a sum.** Overlapping windows are not added: only the first in start order
  applies, as in M-PAGES. In the two bundled SIFs two stations list overlapping windows
  (MARA00VEN, RIOB00BRA, both in the 1990s); no epoch of the present roster is affected.
- **File order is irrelevant.** The blocks are sorted by start with a stable sort (file order for
  equal starts). The same two stations list an old block last, so "the first block" and "the last
  block" must not mean file order.
- **Where one block covers the epoch the operations are the old ones**, in the same order, so the
  result is bit-identical (section 6).

## 3. What moves

`ResolvedStationCoord::periodic` and `SifRosterCoordinate::periodic` (a `SifPeriodicEvaluation`)
report the case, the signed years outside the evaluated block (`t - end` for `AfterLast` and
`InGap`, `t - start`, negative, for `BeforeFirst`) and the block's window. `describePeriodicExtrapolation`
renders the one-line text, and the two callers warn with it, beside the existing occupation
warning: `Application::initializePosition` (`PppSetup.cpp`, the PPP a-priori) and the roster
build in `GnssConfigLoadNetwork.cpp`.

The 8 roster stations, at the golden epoch. Before: no window covers the epoch, so the old rule
applied none of the term. After: the last block, continued (ECEF, mm; the norm is the offset).

| station | years past the last window | block window | X | Y | Z | norm |
| --- | --- | --- | --- | --- | --- | --- |
| BSHM00ISR | 0.359 | [2023.101, 2023.992) | 0.390 | -0.332 | 0.835 | 0.98 |
| PALM00ATA | 0.332 | [2020.568, 2024.019) | -0.266 | -1.007 | -0.626 | 1.22 |
| NIUM00NIU | 0.332 | [2022.951, 2024.019) | -0.479 | -0.263 | 0.028 | 0.55 |
| CHUR00CAN | 0.231 | [2010.671, 2024.120) | -1.483 | 4.894 | -3.925 | 6.45 |
| MAJU00MHL | 3.354 | [2015.441, 2020.997) | -1.005 | -0.469 | 0.001 | 1.11 |
| CPVG00CPV | 0.214 | [2023.636, 2024.137) | 0.652 | -1.013 | -0.730 | 1.41 |
| URUM00CHN | 0.212 | [2018.767, 2024.139) | 0.981 | 0.920 | 2.440 | 2.79 |
| WROC00POL | 0.225 | [2012.809, 2024.126) | 0.022 | -0.244 | 0.365 | 0.44 |

All 8 also extrapolate the linear term (their occupation segment closed with the block). The
other 42 resolvable roster stations are covered by an open last block and are bit-identical
(10 of them are pinned in the unit suite). Eight roster names are absent from the SIFs
(GAMB00PYF, NYA200NOR, STHL00GBR, WARK00NZL, WTZS00DEU, GOP600CZE, LEIJ00DEU, FFMJ00DEU); the
golden gives them explicit coordinates in any case.

One further row changes in the Python twin's cross-check against the C++ engine:
AMC400USA at 2018.5 is 2.63 yr before its only window (2021.13) and now gains the first block's
0.68 mm. Its oracle row is re-pinned (`test_sif_propagate.py`), with the same numbers asserted
in the C++ suite.

## 4. Why no knob

Standing invariant 6 asks for new work to be default off and byte-identical when off. This
change is the intended behaviour of a register item, approved as a build ("Evaluate periodic
coordinate blocks outside their window as M-PAGES v1.2 does"), and corrects a step in a
deterministic coordinate function rather than adding an estimation mechanism. The path it
reaches is narrow: both goldens carry explicit `position_ecef` for every roster station and
neither enables `processing.enable_station_info`, so the SIF coordinate function is not
evaluated on a golden run. The runs that reach it are those with `enable_station_info` (the PPP
a-priori seed, which the filter then estimates away) and `network.use_sif_coordinates` (a roster
position held fixed: here the change is up to 6.4 mm on the field roster). No config in
`config/` arms `network.use_sif_coordinates` (its ledger row is UNVALIDATED); only
`config/ppp_rtk_internal_AR.json` arms `enable_station_info`. No ledger row is added (the ledger
holds one row per config key and no key is added); the identity of both goldens is the gate of
section 6.

## 5. The twins

- **`tools/sif_propagate`** (the standalone Python port, "keep in step"): `_select_periodic_block`
  ports the rule; `ResolvedCoord` gains `periodic_reach`, `periodic_extrapolation_years`,
  `periodic_block_start_year`, `periodic_block_end_year`; four CSV columns and the JSON fields are
  appended after the extrapolation columns (earlier order unchanged); the CLI prints one `# WARNING:`
  line per extrapolated block (`format_periodic_warning`, the C++ text); README updated. Section 10
  of `test_sif_propagate.py` pins the rule on synthetic blocks; section 9 pins the 8 stations above
  from the shipped files.
- **`pyviz/gnssviz/eval/truth.py`** (`SifTruth`, described as a port of `resolveStationCoordinate`
  and used as the fallback and cross-check truth of the test-suite harness): the same selection,
  with a `SifExtrapolationWarning` for the three extrapolated cases and two new tests. Not named by
  the register item; moved so the truth source and the engine do not disagree by up to 6 mm at the
  stations above. The graded SIF truth at an extrapolated station therefore moves by the same
  amount. Run here with `pandas` stubbed (the package `__init__` imports it; `truth.py` needs
  only numpy): 82 pass (before the change 71 pass, 5 skip on the missing SIFs; the change adds 6
  cases and the SIF copy lets the real-file cases run).

## 6. The gates

Written before the golden comparison; U and S are met, G is the next stage.

- **U (unit).** Met 2026-10-17. `tests/test_station_info_seasonal.cpp`, suite `SifSeasonalTermTest`: the five cases
  of the rule, window start inclusive and end exclusive, every permutation of the file order,
  overlap not summed, equal starts, an open last block, switched off and absent, independence from
  the occupation selection, continuity where the last window ends (the term is non-trivial there,
  the step below 0.5 um), covered epochs bit-identical to the old summing rule (synthetic, and 10
  roster stations on the shipped SIFs), the warning text, the loader keeping the file order, the
  roster path carrying the diagnostics, and the 8-station table above. The shipped-SIF cases skip,
  not fail, when `data/static/sif` is absent (4 of the 22 cases). The Debug build is green and
  `build/x64-Debug/gnss_tests.exe` passes 3,162 cases with 8 skipped (the fixture-gated skips of
  other suites; none in this one). `tools/sif_propagate/test_sif_propagate.py` passes 652 checks
  and the pyviz truth tests 82. The AMC400USA oracle row and the 8-station table are the numbers
  the C++ and the Python twin agree on to 5 nm.
- **S (smoke).** Met 2026-10-17, exe `gnss_processor_seasonal.exe` (`ed32e9be281843c6`, main
  `4318fe3` plus this branch, Release), 10 epochs each (`processing.max_duration_s` 270):
  a PPP run on CHUR00CAN with `processing.enable_station_info` (exit 0; the log prints the
  occupation warning and, new, "epoch is 0.229 yr after the last periodic block [2010.6712,
  2024.1202); that block's seasonal term is applied"), and a 13-station network roster from the
  field golden with `network.use_sif_coordinates` and no `position_ecef` (exit 0; 8 periodic
  warnings, for exactly the 8 stations of section 3, none for the 5 covered stations; the roster
  provenance lines list all 13 as SIF-propagated). Configs and logs under
  `D:/GnssProcessorSupport/gates/seasonal/smoke_{ppp,net}/`.
- **G (golden identity, owed).** Field and sim golden, 2 h arms, same build: products,
  `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical, knob not applicable. The
  expectation follows from section 4; the run is the proof.

## 7. Not done

- The ledger row of `network.use_sif_coordinates` (UNVALIDATED, "inert on the explicit golden
  rosters; the 6 h DOY-129 arms `runs/_rap_sif_*_f6` were census-neutral") stands as it is. Those
  arms predate this change; a SIF-armed field roster now moves by at most 6.4 mm at 8 stations,
  far below what the census resolves, and no new arm is run. The row records no mechanism that the
  change alters, so it is not edited.
- A knob to restore the old window-only term is not added (section 4). Reverting is one function.

# TRP: the GPT3 a priori troposphere on the field posture, and the mapping-function label

**STATUS**

- **id:** TRP
- **topic:** network-vehicle
- **status:** open
- **summary:** Open 2026-11-07 (one pre-registered clause failed). From finding F3 of card SRN (build B3). The mapping function configured and logged as `gmf` is Niell's 1996 NMF: relabelled `nmf` (with `gmf` as the legacy spelling) with the products byte-identical. Reading the GPT3 path found a defect: `Gpt3::query` reduced the pressure with the 288 K standard-lapse form of the original GPT, not GPT3's virtual-temperature exponential, 17 to 22 hPa low at the Antarctic coast; fixed. On the 6 h field posture the GPT3 a priori moves each station's wet delay by its own GPT3 shift within 0.6 mm and leaves the user (within 0.4 mm by hour) and the 16-site census (median 0.0 mm, polar sites within 0.4 mm) unchanged; the expected 0.5 to 1 mm drop of the low-elevation phase residuals did not appear (FAILED). With the corrected GPT3, CAS100ATA and SCTB00ATA keep negative wet delays (-0.030, -0.023 m). Ledger rows UNVALIDATED / PARAM; the proposed manifest change is in section 5; nothing armed. Verification (section 6): every number reproduced, with an independent replica of both pressure reductions; the pre-registered SCTB00ATA expectation (about -0.005 m) is not met under the corrected form.
- **source:** card SRN (`docs/refound/SRN_scientific_review_network_2026-11-06.md` on branch `review/network-engine`, not yet on main) finding F3, sections 2.4, 3.5, 4 and build B3 (review worktree `D:/GnssProcessorSupport/worktrees/scirev`, main `adef08b`); `src/core/atmosphere/Troposphere.cpp`, `src/core/atmosphere/Gpt3.cpp`; Niell (1996), J. Geophys. Res. 101(B2), 3227-3246; Landskron and Boehm (2018), J. Geod. 92(4), 349-360
- **dates in the text:** 2026-11-07
- **knobs / posture:** `receiver.troposphere_apriori_source` (`standard_atmosphere` default, `gpt3`) and `receiver.gpt3_grid_path` (both read by the loader before this card; new ledger rows, UNVALIDATED and PARAM); `receiver.troposphere_mapping_function` accepts `nmf` (canonical) and `gmf` (legacy spelling, one note per process), `vmf3` unchanged. Nothing armed; the goldens and the manifest are untouched.
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-1` cut from main `284ec29` (source identical to the review commit `adef08b`): `92e563d` (label), `8719097` (GPT3 reduction), `0282909` (tests), then the docs commit; exe `gnss_processor_trp.exe` (sha256[:16] `8404262454708867`); review exe `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`); arms `D:/GnssProcessorSupport/runs_nom/_rap_trp_smoke_f01`, `_rap_trp_off_f2`, `_rap_trp_ctl_f6`, `_rap_trp_gpt3_f6`, `_rap_trp_gpt3old_f6` (review exe), census replays `_rap_trpc_ctl_*`, `_rap_trpc_g3_*`; SRN controls `_rap_srn_ctl_f2`, `_rap_srn_ctl_f6`, SRN arm `_rap_srn_gpt3_f2`

**Outcome.** Question: does a meteorological a priori hydrostatic delay (GPT3) correct the Antarctic wet delays without moving the user, and is the mapping label right? Result: the label was wrong (NMF, not GMF) and is corrected with byte-identical products; the GPT3 pressure reduction was defective at the Antarctic coast and is corrected. The GPT3 a priori moves every station's wet delay by its own hydrostatic shift and nothing else measurable: user and census within 0.5 mm, phase residuals unchanged, so the pre-registered residual clause fails. Two Antarctic wet delays stay negative. Standing: open; arming is the author's decision on the proposal of section 5.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question and context (2026-11-07)

Card SRN (F3) measured that the Saastamoinen zenith hydrostatic delay is evaluated from the pressure of a standard atmosphere reduced to the station's ellipsoidal height, with no dependence on latitude, longitude or season. The bilinear GPT3 grid of the repository gives 37.5, 42.2, 37.1 and 21.5 hPa less pressure on day 129 at SCTB00ATA, CAS100ATA, SYOG00ATA and PALM00ATA, so the a priori hydrostatic delay there is 85, 96, 84 and 49 mm too large. The station wet delay absorbs most of the error and reads negative (baseline medians over hours 2 to 6: -0.091, -0.082, -0.018, +0.019 m). The residue that the wet column cannot absorb is the difference between the wet and the hydrostatic mapping times the error, 1.5 to 2.9 mm at the 15 degree mask at those stations. A 2 h arm with the GPT3 source (`_rap_srn_gpt3_f2`) raised the four medians to -0.002, +0.007, +0.071 and +0.063 m and left the held-out user WTZZ00DEU unchanged to the millimetre.

Questions: (a) does the GPT3 a priori source move each station's wet delay by its own GPT3-implied shift over a 6 h run and leave the user and the 16-site census within the neutral spread; (b) does it lower the low-elevation phase post-fit residuals at the polar stations; (c) the mapping function named `gmf` in the configuration and the log is the Niell (1996) NMF: can the label be corrected without a numerical change?

The sim fixture cancels the hydrostatic delay by construction (`src/sim/SimConfig.hpp`, the standard-atmosphere and mapping pair synthesized and removed by the same model; `EphemerisProvider.cpp`), so a GPT3 a priori on the sim golden would add a modelling error the fixture does not contain. The sim golden keeps the standard atmosphere as a documented exception.

## 2. Design (2026-11-07, before the code)

### 2.1 What the code does now (read at main `284ec29`)

- `Troposphere::Troposphere` (`src/core/atmosphere/Troposphere.cpp`) loads the GPT3 grid when the a priori source is `gpt3` or the mapping function is `vmf3`: the path is `receiver.gpt3_grid_path` when set, otherwise `<files.internal_data_root>/static/gpt3/gpt3_5.grd`. A failed load logs a warning and falls back to the standard atmosphere and NMF.
- `Troposphere::computeSaastamoinenZHD` takes the pressure from `Gpt3::query` at the receiver's own geodetic latitude, longitude and ellipsoidal height and the fractional day of year of the epoch (`getDayOfYear`, 129.0 at 2024-05-08 00:00) when the source is `gpt3` and the grid is loaded; otherwise from the standard atmosphere. A failed query keeps the standard-atmosphere value.
- `Gpt3::query` evaluates the annual and semi-annual harmonics of each of the four surrounding 5 degree cells, reduces each corner's pressure from the cell's orthometric reference height to the station's orthometric height (`P(h) = P(Hs) (1 - 2.26e-5 dh)^5.225`, `dh = h - N - Hs`) and interpolates bilinearly.
- Each network station owns a `PppModel` built from the network configuration (`ApplicationNetworkSetup.cpp`, `st.model = std::make_unique<PppModel>(config_, ...)`); its `ModelGeometry` owns a `Troposphere`, and `Geometry` evaluates the hydrostatic delay at `rx_pos = state.position`, the station's own coordinate, at `obs.time` (`ModelGeometry.cpp`, the propagation corrections and the seed path). `PppModel::computeBaseModeledValue` adds `geom.tropo_zhd * mf_d`. The closed-loop consumer is a `RealTimeEngine` built from `GnssConfig::deriveClosedLoopConsumerConfig()`, which starts from a copy of the network configuration (`GnssConfig c = *this`), so the user takes the same source and grid. With source `gpt3` every station and the user therefore use their own coordinates and the run's day of year.
- `getTroposphereMapping` evaluates VMF3 from the GPT3 "a" coefficients when `vmf3` is configured and the grid loaded; otherwise the Niell (1996) NMF tables (hydrostatic mean and annual amplitude, wet mean, five latitude nodes) with the Niell height correction. The configuration value, the enumerator (`TroposphereMappingFunction::Gmf`), the run-start line (`Troposphere a priori: source=..., mapping=gmf`) and the constructor line (`mapping=GMF`) all call this path GMF; the Global Mapping Function of Boehm et al. (2006) is not implemented.

### 2.2 The label change (no numerical change)

- The enumerator becomes `TroposphereMappingFunction::Nmf`, documented as the Niell (1996) NMF.
- One owner for the spelling: `parseTroposphereMappingFunction` and `troposphereMappingFunctionLabel` beside the enum in `include/config/GnssConfigTypes.hpp` (the precedent is `processingModeLabel`). `nmf` is the canonical spelling and the default; `gmf` is accepted as the legacy spelling of the same NMF path, and the loader writes one note per process naming it; `vmf3` is unchanged; anything else warns and selects NMF as before. A label for the a priori source sits beside it.
- The run-start line reads `Troposphere a priori: source=standard_atmosphere, mapping=nmf, ...`; the constructor line, the fallback warning and the trace lines of `Troposphere.cpp` say NMF; the Doxygen of `getTroposphereMapping` and of the enum name the Niell (1996) NMF and state that GMF is not implemented.
- Gate: products and every pre-existing trace byte-identical on a 2 h field arm against the review executable's control (section 3, G1).

### 2.3 Tests

A new file `tests/test_troposphere_mapping_label.cpp`, one suite: the spelling table (`nmf`, `gmf`, upper case, `vmf3`, unknown, absent key), the labels, NMF and the legacy spelling giving identical mappings, and the hydrostatic delay under `gpt3` at a known coordinate and day against a hand value computed in the test from the grid cells (a synthetic grid whose four corner cells differ in pressure, harmonics and reference height, so the bilinear weights, the harmonic and the height reduction are all exercised), plus the same station on another day and another station on the same day moving as the hand values say.

### 2.4 A defect in the GPT3 height reduction, found while reading (2026-11-07)

`Gpt3::query` reduces each corner cell's pressure to the station with `P(h) = P(Hs) (1 - 2.26e-5 dh)^5.225`, the standard-lapse form of the original GPT (Boehm et al. 2007), whose constants stand for a 288.15 K atmosphere. GPT3, like GPT2 and GPT2w before it (Lagler et al. 2013; Landskron and Boehm 2018, the reference code `gpt3_5_fast.m`), reduces with the cell's own virtual temperature: `P(h) = P(Hs) exp(-c dh)`, `c = g M_d / (R Tv)`, `Tv = T0 (1 + 0.6077 Q)`, with `g` 9.80665 m/s^2, `M_d` 28.965e-3 kg/mol, `R` 8.3143 J/(mol K), `T0` the cell's temperature and `Q` its specific humidity (stored in g/kg). The two forms agree within 1 to 2 mm of hydrostatic delay where the cell heights are near the station's. They part where a corner cell sits a kilometre or more above or below the station in air far from 288 K: the Antarctic coastal stations, whose southern corner cells lie on the ice sheet (CAS100ATA: cells at 1121 and 1644 m, plateau temperatures near 240 K). A replica of both forms on day 129.125 (scratch `gpt3_ref.py`, the grid of the repository) gives:

| station | standard atmosphere (hPa) | as built (hPa) | GPT3 reference (hPa) | shift as built (mm) | shift reference (mm) |
| --- | --- | --- | --- | --- | --- |
| SCTB00ATA | 1015.5 | 978.0 | 985.7 | +85.3 | +67.7 |
| CAS100ATA | 1010.6 | 968.3 | 986.7 | +96.1 | +54.2 |
| SYOG00ATA | 1007.3 | 970.2 | 992.5 | +84.3 | +33.5 |
| PALM00ATA | 1009.5 | 988.0 | 988.2 | +48.9 | +48.5 |
| MBAR00UGA | 862.6 | 864.4 | 865.4 | -4.2 | -6.4 |
| WTZR00DEU | 935.8 | 942.6 | 943.2 | -15.6 | -17.0 |
| BOGT00COL | 739.7 | 745.2 | 754.0 | -12.5 | -32.6 |
| KOKV00USA | 880.7 | 886.0 | 891.6 | -12.1 | -25.1 |
| DAV100ATA (census) | 1007.9 | 972.9 | 988.3 | +79.7 | +44.5 |
| ARHT00ATA (census) | 997.1 | 960.2 | 965.3 | +83.9 | +72.3 |

"Shift" is the standard-atmosphere hydrostatic delay minus the GPT3 one, the rise the wet delay should show. The as-built pressures at CAS100ATA and SYOG00ATA (968 and 970 hPa at sea level) lie well below the circumpolar trough (near 985 to 990 hPa); the reference values do not. Over the 58 stations, the user and the 16 census sites, the two forms differ by more than 5 mm at 12 sites, all with a corner cell over 900 m from the station.

Response: `Gpt3::query` takes the reference reduction (one formula change; the specific-humidity column is read at load). The path is reached only with `receiver.troposphere_apriori_source: gpt3` (the VMF3 mapping reads only the "a" coefficients, which have no height reduction), so every configuration without the GPT3 source is unchanged. The temperature reduction (linear lapse `dT`) already matches the reference. The as-built form is still graded, as the review asked, with the review executable (it predates the change; its control `_rap_srn_ctl_f6` exists): arm `trp_gpt3old_f6`. The reference form is graded with this card's executable: arm `trp_gpt3_f6`. The bars of section 3 apply to each arm with its own implied shifts.

### 2.5 Ledger

Rows for `receiver.troposphere_apriori_source` and `receiver.gpt3_grid_path` (parent: the source), card TRP. The recommended verdict for the source is RECOMMEND_SCOPED on the field posture only, the sim golden keeping the standard atmosphere as a documented exception. The verdict written on this branch must also let `make_golden_config.py --verify` pass with the goldens and the manifest unchanged (section 5).

## 3. Gates (pre-registered 2026-11-07)

Release only, exe `gnss_processor_trp.exe`; arms from `config/golden/golden_field_refound.json` with `scripts/rapid_arm.py`.

- **G0 smoke:** a 0.1 h field arm with the two keys exits 0, logs `source=gpt3, mapping=nmf` and the grid load.
- **G1 label byte identity:** a 2 h field arm with no key changed (`trp_off_f2`) against the review executable's `_rap_srn_ctl_f2`: `products.txt` without `#` lines and every pre-existing `logs/*.csv` identical.
- **G2 the arm against its same-exe control:** `trp_ctl_f6` (golden unchanged) and `trp_gpt3_f6` (`receiver.troposphere_apriori_source=gpt3`, `receiver.gpt3_grid_path=C:/GnssProcessor/data/static/gpt3/gpt3_5.grd`), 6 h.
  - User: WTZZ00DEU 3D p50 and p90 by hour (`logs/closed_loop_trace.csv`, `pos_err_3d_m`); bar: within 2 mm of the control in each of hours 2 to 6.
  - Station wet delay: the median of `zwd_m` over hours 2 to 6 (`logs/network_zwd.csv`) at SCTB00ATA, CAS100ATA, SYOG00ATA, PALM00ATA, MBAR00UGA and WTZR00DEU; bar: the arm-minus-control change within 0.03 m of the GPT3-implied shift (+0.085, +0.096, +0.084, +0.049 m at the four Antarctic stations; computed for MBAR00UGA and WTZR00DEU in section 4). SCTB00ATA may stay marginally negative (about -0.005 m).
  - Phase post-fit rms at 15 to 20 degrees of elevation at the four Antarctic stations (`logs/network_pf_rows.csv`, phase rows, hours 2 to 6); expected 0.5 to 1 mm lower than the control.
- **G3 census:** `scripts/clock_level/gcensus.py`, consumer-only replays of each run's own wire at WTZZ00DEU and the 16 sites (the arm's consumer with the arm's keys); paired change of each site's mean hourly p50 over hours 2 to 6; bar: median paired change within 0.5 mm; the polar sites (ARHT00ATA, DAV100ATA, UTQI00USA, BAKE00CAN) read individually, hour 1 included.
- **G4 unit suite:** Debug `gnss_tests.exe` all pass with the new suite (0 failures; skips fixture-gated only).

## 4. Results (2026-11-07)

Exe `gnss_processor_trp.exe` (Release, sha256[:16] `8404262454708867`, the branch at `0282909`); the as-built GPT3 arm on the review exe `gnss_processor_srn.exe`. Every run exited 0. Grader: scratch `grade_trp.py` (user by hour from `closed_loop_trace.csv`, wet-delay medians over hours 2 to 6 from `network_zwd.csv`, phase post-fit rms by elevation from `network_pf_rows.csv`), byte identity by scratch `identity.py`; census by `scripts/clock_level/gcensus.py`. Hours count from 00:00.

### 4.1 G0 smoke and G1 label byte identity: PASS

- `trp_smoke_f01` (0.1 h, the two keys): exit 0; the run-start line reads `Troposphere a priori: source=gpt3, mapping=nmf, gpt3_grid_path=C:/GnssProcessor/data/static/gpt3/gpt3_5.grd`; `GPT3: loaded 2592 cells` 118 times (one grid per `Troposphere`: two per station model and the consumer's; a startup cost of a few seconds in Release, not a defect).
- `trp_off_f2` (2 h, no key) against `_rap_srn_ctl_f2`: `products.txt` without `#` lines IDENTICAL (113,888 lines); 31 of 34 `logs/*.csv` identical, the other three (`network_datum_graph.csv`, `network_run_summary.csv`, `network_service_health.csv`) differ only in their wall-time columns (identical with the timing column removed). The run-start line changes from `mapping=gmf` to `mapping=nmf`; no legacy note (the golden does not set the key).
- `trp_ctl_f6` (6 h, no key) against `_rap_srn_ctl_f6`: `products.txt` IDENTICAL (343,049 lines), the same three timing files differ. The 6 h control of this card therefore equals the review's control, so `trp_gpt3old_f6` (review exe) and `trp_gpt3_f6` (this exe) share one control wire.

### 4.2 G2 the 6 h arm against its control

| hour | control 3D p50 / p90 (m) | GPT3 as built p50 / p90 | GPT3 reference p50 / p90 | change, reference (mm) |
| --- | --- | --- | --- | --- |
| 1 | 0.0588 / 0.1329 | 0.0585 / 0.1333 | 0.0584 / 0.1334 | -0.4 / +0.5 |
| 2 | 0.0123 / 0.0190 | 0.0123 / 0.0191 | 0.0123 / 0.0191 | +0.0 / +0.1 |
| 3 | 0.0231 / 0.0308 | 0.0229 / 0.0305 | 0.0229 / 0.0304 | -0.2 / -0.3 |
| 4 | 0.0123 / 0.0165 | 0.0122 / 0.0162 | 0.0122 / 0.0162 | -0.1 / -0.3 |
| 5 | 0.0165 / 0.0213 | 0.0164 / 0.0211 | 0.0164 / 0.0211 | -0.1 / -0.2 |
| 6 | 0.0120 / 0.0155 | 0.0122 / 0.0156 | 0.0122 / 0.0156 | +0.2 / +0.2 |

User bar (within 2 mm in hours 2 to 6): PASS for both arms (largest change 0.3 mm).

Wet delay, median over hours 2 to 6 (m); "as built" is `trp_gpt3old_f6`, "reference" is `trp_gpt3_f6`:

| station | control | as built | change | implied | reference | change | implied |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SCTB00ATA | -0.0901 | -0.0055 | +0.0846 | +0.0853 | -0.0230 | +0.0671 | +0.0677 |
| CAS100ATA | -0.0833 | +0.0120 | +0.0953 | +0.0961 | -0.0295 | +0.0538 | +0.0542 |
| SYOG00ATA | -0.0168 | +0.0666 | +0.0834 | +0.0843 | +0.0164 | +0.0332 | +0.0335 |
| PALM00ATA | +0.0180 | +0.0666 | +0.0486 | +0.0489 | +0.0662 | +0.0481 | +0.0485 |
| MBAR00UGA | +0.2511 | +0.2470 | -0.0041 | -0.0042 | +0.2447 | -0.0064 | -0.0064 |
| WTZR00DEU | +0.1477 | +0.1323 | -0.0154 | -0.0156 | +0.1309 | -0.0168 | -0.0170 |

Wet-delay bar (within 0.03 m of the implied shift): PASS for both arms; over all 58 stations the change differs from the implied shift by at most 0.8 mm (as built) and 0.6 mm (reference), median 0.0. The wet column absorbs the zenith a priori change almost exactly. Negative medians: control 3 (SCTB00ATA, CAS100ATA, SYOG00ATA), as built 1 (SCTB00ATA, -0.006 m), reference 2 (SCTB00ATA -0.023, CAS100ATA -0.030 m).

Phase post-fit rms over hours 2 to 6 (mm), 15 to 20 degrees of elevation (all elevations in parentheses):

| station | control | as built | reference | rows |
| --- | --- | --- | --- | --- |
| SCTB00ATA | 2.724 (1.654) | 2.713 (1.652) | 2.715 (1.652) | 2,302 |
| CAS100ATA | 2.221 (1.684) | 2.222 (1.683) | 2.222 (1.684) | 3,012 |
| SYOG00ATA | 2.695 (1.667) | 2.694 (1.666) | 2.694 (1.667) | 2,761 |
| PALM00ATA | 1.489 (1.156) | 1.487 (1.155) | 1.487 (1.155) | 2,643 |

Phase-residual bar (0.5 to 1 mm lower): FAILED for both arms; the change is -0.011 to +0.001 mm. The expectation of card SRN (the wet-minus-hydrostatic mapping difference times the a priori error, about 3 mm at 15 degrees) assumed the residue reaches the phase rows; it does not at this level. A slant error that varies smoothly over an arc is shared among the float ambiguities, the station clock and the wet column, and the 15 to 20 degree residual rms is dominated by other errors (2.2 to 2.7 mm at three of the four stations).

### 4.3 G3 census: PASS

`gcensus.py make` with each run as its own consumer base and wire (`trpc_ctl` from `_rap_trp_ctl_f6`, `trpc_g3` from `_rap_trp_gpt3_f6`, so the arm's consumers take the GPT3 source too), 17 replays each, all exit 0; `gcensus.py grade trpc_ctl trpc_g3`:

- Median site p50 by hour: control 0.142, 0.050, 0.036, 0.029, 0.032, 0.030 m; arm 0.142, 0.050, 0.036, 0.028, 0.032, 0.030 m. WTZZ00DEU replay 0.059 0.012 0.023 0.012 0.017 0.012 against 0.058 0.012 0.023 0.012 0.016 0.012.
- Hours 2 to 6 paired: median +0.0000 m, none better or worse at 1 cm, largest site change 0.5 mm (CEDU00AUS). Hour 1 paired: median +0.0000 m. Bar (median within 0.5 mm): PASS.
- Polar sites, read individually (hour 1, then the hours 2 to 6 change): ARHT00ATA 0.301 against 0.301 m, -0.4 mm; DAV100ATA 0.343 against 0.343 m, +0.4 mm; UTQI00USA 0.132 against 0.132 m, 0.0 mm; BAKE00CAN 0.106 against 0.106 m, 0.0 mm. The GPT3 shifts at ARHT00ATA (+72 mm) and DAV100ATA (+45 mm) leave the user's hour 1 unchanged: the consumer estimates its own wet delay, which absorbs the a priori change as the stations' do. The 0.1 to 0.2 m start-up transient card SRN anticipated for a polar user does not show in position.

The census of the as-built arm was not run (the reference form is the one proposed; the as-built form is superseded by `8719097`).

### 4.4 G4 unit suite: PASS

Debug `gnss_tests.exe` at `0282909`: 3,841 cases in 524 suites, 3,807 passed, 34 skipped (the 33 fixture-gated cases and the new suite's shipped-grid case, the worktree's `data/` being empty), 0 failed. With the shipped grid copied into the worktree for one run, the 13 GPT3 cases that need it or belong to the new suite (`Gpt3Test`, `Gpt3RealGrid`, `TroposphereMappingLabel`) pass, the CAS100ATA hydrostatic delay within 2e-8 m of the literal (P = 986.7088 hPa).

### 4.5 Reading

- The label: settled. NMF is what runs; `nmf` is the name; `gmf` still loads.
- The GPT3 reduction: the as-built form put the Antarctic coastal pressures 17 to 22 hPa below the GPT3 reference (968 and 970 hPa at sea level at CAS100ATA and SYOG00ATA, below the circumpolar trough); the reference form is the published model. Neither the user nor the census can tell the two apart, because every receiver's wet column absorbs the zenith a priori; the station wet delays (a product for meteorology and for any time-linked wet delay, card SRN build B2) can.
- What GPT3 buys on this posture is a physically plausible station wet delay at most sites, not position: the four Antarctic medians move by +68, +54, +34 and +49 mm. Two stay negative (SCTB00ATA -0.023, CAS100ATA -0.030 m). The remaining bias is not traced here; candidates are the day's weather (GPT3 is a climatology: a 10 hPa synoptic anomaly is 23 mm), the NMF hydrostatic mapping at high southern latitude (its seasonal model is weakest there; VMF3 from the same grid is the unarmed alternative) and the station heights.

## 5. Ledger, proposed manifest change and what is owed (2026-11-07)

Ledger (`docs/CONFIG_promotion_ledger.json`, rendered): `receiver.troposphere_apriori_source` UNVALIDATED (area `receiver`, default `standard_atmosphere`), `receiver.gpt3_grid_path` PARAM (parent the source, default empty). The pre-registered verdict for the source was RECOMMEND_SCOPED. A probe of that verdict (regime `True`, the goldens and the manifest unchanged) fails `make_golden_config.py --verify` on all three goldens ("UNARMED in-regime RECOMMEND_SCOPED"), as the generator is designed to: a RECOMMEND row in regime is the promotion step, and the manifest is the author's. The row is therefore UNVALIDATED on this branch, the precedent of card OTL before its arming, and states the recommendation in its text. `make_golden_config.py --verify` PASS and `config_posture_check.py --verify-loader` 0 coverage gaps.

Recommendation: RECOMMEND_SCOPED, field posture only, contingent on the author's reading of the failed residual clause (section 4.2: the clause's expectation was wrong; no instrument found a cost). Proposed change at arming, in one commit:

1. `config/golden/golden_manifest.json`, the field lineage (the BeiDou lineage is generated from the field golden's output and inherits the pin): pin `receiver.troposphere_apriori_source: "gpt3"` and `receiver.gpt3_grid_path: "C:/GnssProcessor/data/static/gpt3/gpt3_5.grd"` (or leave the path unset: on the field golden the default resolves to the same file through `files.internal_data_root`).
2. The sim lineage: an exception for `receiver.troposphere_apriori_source`, reason "the sim fixture synthesizes and removes the hydrostatic delay with the standard atmosphere and the same mapping (`src/sim/SimConfig.hpp`, `EphemerisProvider.cpp`), so GPT3 would add an error the fixture does not contain".
3. The ledger row to RECOMMEND_SCOPED with `golden: true`, a regime (`True`, the sim exception carrying the scope) and `armed_when` `val("receiver.troposphere_apriori_source", "standard_atmosphere") == "gpt3"`, plus its row in the loader's posture table (`GnssConfigNetworkPosture.cpp`), which `--verify-loader` requires of a RECOMMEND row.
4. Fingerprints: the a priori source changes every station's modelled range, so a run armed with it must not resume from a snapshot written without it. `network/state/SnapshotFingerprint.cpp` covers network keys only: neither this key nor the ocean tide loading keys of card OTL enter it (read 2026-11-07). The cold goldens are unaffected; a warm-resume lineage armed across the change would need the key added to the fingerprint (owed before such a lineage, not before arming).

Owed, none blocking the label or the reduction fix:

- The negative wet delays at SCTB00ATA and CAS100ATA under the reference GPT3: a VMF3-mapping arm on the same day (the VMF3 open item of `docs/status/OPEN_ITEMS_CODE_DEFECTS.md` asks for a regenerated `tropo_vmf3_tight` run first), and a check against a surface pressure record for the day where one exists.
- Multi-day: day 129 is one synoptic state; a census over card MDAY's days 128 to 132 would show whether the user stays inert under other weather.
- The sim code still names its own mapping "GMF" (`src/sim/EphemerisProvider.cpp`, `include/sim/EphemerisProvider.hpp`); comment text only, outside this card's change.
- The snapshot fingerprint of item 4 above, before any warm-resume lineage arms the key.

## 6. Second-model verification (2026-11-07)

One Opus referee, read-only, re-derived every decision-driving number from the run directories and the code (eleven claims confirmed, one partly, one unverifiable; the verdicts are kept with the review's records under `D:/GnssProcessorSupport/srn/`). Confirmed with the referee's own scripts: the two identities; the user by hour (the largest hours 2 to 6 change of the p50 is 0.2 mm; the 0.3 mm of section 4 is the p90 column); the wet-delay medians, with an independent Python replica of both pressure reductions on the shipped grid (CAS100ATA 968.3 hPa as built against 986.7 hPa, SYOG00ATA 970.2 against 992.5; the implied shifts within 0.6 mm of the observed changes); the Antarctic phase residuals (changes of -0.008 to +0.001 mm); the census (16 sites, paired median +0.025 mm, largest CEDU +0.51 mm, ARHT00ATA -0.45 mm, so "polar sites within 0.4 mm" reads 0.45 mm); the code (the virtual-temperature form with the specific-humidity column, the gpt3 a priori path as the only reader of that pressure, the enum rename with the legacy spelling, the loader default unchanged); the ledger rows, the golden verify and the restored data file. Unverifiable from artifacts: the unit-suite counts (arithmetically consistent, 3,833 plus 8 with one more skip) and the executable's hash. The executable was compiled at 03:23, before the code commits of 04:05 and 04:06, from the uncommitted tree; its run-start line and its wet-delay shifts match the committed code.

Corrections and omissions the referee recorded: (1) the implied-shift bars of section 3 (+0.085, +0.096, +0.084, +0.049 m) belong to the as-built reduction; against them the corrected arm misses by 0.018 to 0.051 m at SCTB00ATA, CAS100ATA and SYOG00ATA, and the pass rests on the per-arm implied shifts of section 2.4, which the replica confirms; (2) the pre-registered expectation that SCTB00ATA stays marginally negative (about -0.005 m) is not met under the corrected form (-0.023 m, and CAS100ATA -0.030 m), and section 4 does not name it as a miss; (3) `receiver.troposphere_mapping_function` has no ledger row although its canonical spelling and default changed; (4) `tests/test_atmosphere_models.cpp` line 253 still names the removed enumerator in a comment; (5) `Gpt3::harmonic` uses the calendar day of year without an epoch offset, matching the reference code as the referee recalls it, not checked against the source. The decision stands: byte-neutral off, nothing armed, the residual clause failed, the card open.

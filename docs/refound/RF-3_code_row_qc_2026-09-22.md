# RF-3: QC that acts on code rows; the gross code rows are the re-coupling edge of the slip response, and the build that follows

**STATUS**

- **id:** RF-3
- **topic:** raim-qc
- **status:** open
- **summary:** BUILT 2026-09-22 (main `0436002`, RF-3a `network.amb_float_drop_var_m2` and RF-3b's four screen rules, default OFF). The drop floor removes the walk (rows above 3 m 394 to 80 at 100 m2, T4_attrib 81 to 19, WTZZ00DEU hours 3 to 6 to 0.015 0.015 0.027 0.037 m, census absolute Up smaller at 10 of 16) but costs hour 1 (0.560 to 0.651 m; census +0.034 m): drops happen in hour 1 too; a warm-up gate is the next arm. The screen's one-row rule and same-epoch emission work (rows above 10 m 100 to 32, census 13 of 16 better on hours 4 to 6, none worse, first changed emission at the declaring epoch); the hold (`gross_los_bound`) is FALSIFIED (the erase is the bounded response); the station tier needs the `fixed` rule to have a population. Earlier summary: READ 2026-09-22 (Opus read worker, config-only arms; every number re-graded by the manager from `runs/`), plus a birth-floor ladder run by the manager. The field baseline `runs/_rf0hv9_field6` folds 101 post-fit code rows above 10 m (404 above 3 m) at full weight, in 70 line-of-sight episodes. Five of the six largest have CLEAN code data: they are the RE-COUPLING edge of the slip and gap re-entry response, not code faults. At a refloat the float ambiguity is re-born with the arc's FIRST-STASH variance (STHL00GBR G11: 0.036 m2 from two hours earlier, `network.amb_float_birth_var_m2` being 0 in the posture), so a level wrong by 40 m is folded at 0.19 m and walks down geometrically for 24 epochs while the code rows carry the misfit with the ionospheric band ratio. The gross-LOS screen (card GL) cures what it declares because its response ERASES the carry: four of six episodes die within one epoch of the fold; with a one-row declaration full-weight gross rows fall 101 to 33 and 13 of 16 census sites improve; the prefit fuse is blind by construction (the fault is created by the solve). A birth floor of 4 or 100 m2 removes the walk (rows above 3 m 404 to 101 / 86; WTZZ00DEU hours 3 to 6 0.028 0.030 0.036 0.045 to 0.015 0.016 0.029 0.038 m) but slows every BIRTH (hour 1 0.56 to 0.66 m; census hour 1 +0.03 to +0.04 m, count median 342 to 301). Build pre-registered in section 4: a DROP-only floor (RF-3a), the gross-LOS screen's one-row rule, bounded decoupling, same-epoch emission and station tier (RF-3b), the wire's discontinuity tokens on this path (RF-3c). Nothing built.
- **source:** the re-founding plan (RF-3: "QC that acts on CODE rows ... and the wire contract on every channel"); [RF-1](RF-1_float_ambiguities_design.md) sections 3.3 and 3.4 (the birth floor and the drop); [SCU](SCU_station_coordinates_unknowns_2026-09-22.md) and [GRK](GRK_gap_reentry_key_mismatch_2026-09-22.md) (the gross rows named there); card GL (`include/filter/NetworkGrossLos.hpp`)
- **knobs / posture:** built 2026-09-22: `network.amb_float_drop_var_m2`, `network.gross_los_min_rows_rule`, `network.gross_los_bound` (KEEP OFF), `network.gross_los_emit_exclude`, `network.gross_los_station_tier`; existing: `network.gross_los_screen` (+ `gross_los_k` 8, `gross_los_min_rows` 2, `gross_los_epochs`, `gross_los_deweight`, `gross_los_clear_*`, `gross_los_max_gap_s`, `gross_los_max_station_frac`), `network.prefit_fuse_sigma`, `network.amb_float_birth_var_m2` (0 in the posture). Proposed: `network.amb_float_drop_var_m2`, the GL rules of section 4. Posture: the RF-0hv working bases (baseline `runs/_rf0hv9_{field,sim}6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv9_field6`; arms `runs/_rap_rf3_gl`, `_rf3_gl1`, `_rf3_fuse`, `_rf3_bv025`, `_rf3_bv4`, `_rf3_bv100`; census keys `rf3gl`, `rf3gl1`, `rf3fuse`, `bv4`, `bv100` (against `grk`); exe `gnss_processor_sid3.exe` (`41C04F7F...`); the read report `runs/_msc/rf3_read_report_2026-09-22.md`

## 1. The population and its cause

`runs/_rf0hv9_field6/logs/network_pf_rows.csv`: 101 code rows above 10 m (100 at full weight; the
one exception is a companion row), 404 above 3 m, 70 LOS episodes. The six largest, with the cause
read from the raw RINEX (code minus phase per band with the arc mean removed, and the between-code
differences) and from the events at the episode's first epoch:

| episode | onset (TOW), epochs | residuals (m) | raw data | first-epoch events | cause |
| --- | --- | --- | --- | --- | --- |
| STHL00GBR G11 | 269640, 23 | -40 / -40 / -67 decaying | L1W and L2W phases slipped after a one-epoch dropout; all three codes flat (C1C-L1C within 0.3 m) | `arc_resume shifted_fresh` L1W, L2W; `reentry_relevel refloat`; `T4_attrib refloat` every epoch to 269910 | MODEL |
| CPVG00CPV G13 | 272490, 18 | -29 / -47 | a band-1 phase slip of 306 cycles; C1C-C2W flat | `lli_slip`, `if_slip refloat`, two `relevel_free` responses | MODEL |
| BSHM00ISR G13 | 273240, 5 | +42 to +134 ramp | only C1C and L1C survive; code minus phase scatters 4 m | `arc_resume unmeas_fresh`; `reentry_relevel refloat` (LLI) | MODEL |
| KIT300UZB G22 | 279690, 17 | +24 flat | a self-consistent single-signal arc (code minus phase drifts 5 m, twice the iono) | `arc_resume unmeas_bridged` (no response: the carry was bridged) | MODEL |
| MBAR00UGA G20 | 273900, 34 | -9 / -9 / -15 | as STHL00GBR | `arc_resume shifted_fresh`; `reentry_relevel refloat` | MODEL |
| BSHM00ISR G22 | 273450, 8 | +85 then +25 | LLI on re-acquisition; a real 60 m raw code transient at the first epoch only | `arc_resume unmeas_fresh`; `reentry_relevel refloat` | MIXED: the first epoch is the receiver, the plateau is the model |

The L2 to L1 residual ratio in the two-band episodes is the ionospheric 1.647, the phase
residuals stay at millimetres, and every episode starts on a re-entry or slip response.
The mechanism, from `network_amb_state.csv` at STHL00GBR G11: at 269640 the arc is dropped
(`drop:relevel_free`) and re-stashed in the same epoch with `fold_var_m2` 0.0363 = the arc's
`var_first_m2` (its first-stash variance from TOW 262000), with value 47.2 m against the
previous 56.4 m; the next epochs fold the same 0.036 m2 prior and the value walks 40.1, 34.4,
29.7, 26.0, 22.9, 20.3, 18.3 m (a geometric decay: a stiff prior relaxing at the carry's q),
while `drop:t4_attrib` erases the entry every epoch and the harvest re-stashes it. RF-1's design
(section 3.4) restores `max(var_first, amb_float_birth_var_m2)` at a drop and the floor was to be
calibrated from the young bucket of the prior-residual census; the working posture carries no
floor (0), so a drop restores the stale stiff variance. This is standing invariant 1's re-coupling
edge, and the class card SPEC_slip_relevel_walk named on the leveling vehicle, in the float
vehicle's own form.

The wire effect of the six episodes is small (G11's emitted C1W bias drifts -0.133 m over seven
epochs with no `code_disc` token; clock steps below 0.003 m; no census site moves), but 101 rows
of 40 to 67 m at full weight enter the normal equations, and the run-end block-Baarda and
congruence instruments read them (the congruence screen's worst station-epoch names three of
the six episodes exactly: BSHM00ISR t = 1.29e5 with a delta of 4475 m Up at TOW 273300).

## 2. The screens, and a birth-floor ladder (6 h arms from the baseline, exe `gnss_processor_sid3.exe`)

| arm | rows above 3 m / above 10 m at full weight | WTZZ00DEU 3D p50 by hour (m) | below 0.1 m | clock wire (m) | `T4_attrib` | census against `grk` (16 sites) |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | 404 / 100 | 0.560 0.059 0.028 0.030 0.036 0.045 | 611 | 0.0045 / 0.0028 | 81 | |
| `rf3_gl` (`gross_los_screen`) | - / 61 | unchanged | 611 | 0.0042 | | counts 6 better 0 worse; hours 2 to 6 p50 better at 11 |
| `rf3_gl1` (+ `gross_los_min_rows` 1) | - / 33 (+150 rows folded at 1e-4 weight) | unchanged | 611 | 0.0041 | | counts 5 better 0 worse; p50 better at 13 |
| `rf3_fuse` (`prefit_fuse_sigma` 1000) | 404 / 101 | unchanged | 611 | 0.0045 | | null (9 rows capped in 6 h) |
| `rf3_bv025` (`amb_float_birth_var_m2` 0.25) | 205 / 42 | 0.557 0.061 0.030 0.033 0.040 0.049 | 611 | 0.0043 / 0.0029 | 58 | not run |
| `rf3_bv4` (4) | 101 / 39 | **0.659** 0.054 **0.015 0.016 0.029 0.038** | 598 | 0.0043 / 0.0030 | 21 | hour 1 +0.042 m (3 better, 7 worse); hours 2 to 6 4 better, 3 worse (median -0.001); absolute Up smaller at 11 of 16; count median 342 to 301 |
| `rf3_bv100` (100) | 86 / 35 | 0.651 0.051 0.015 0.015 0.027 0.036 | 585 | 0.0043 / 0.0030 | 19 | hour 1 +0.031 m (3 / 6); hours 2 to 6 4 / 4 (-0.002); Up smaller at 10 of 16; count median 301 |

Readings:

- The gross-LOS response is the right response: its erase of the carry is a true rebirth and
  the episode dies within one epoch of the fold (MBAR00UGA G20 released back to full weight
  two epochs later at +0.45 m where the baseline was still at -5.4 m twelve epochs on). Its
  declaration reaches the wire two epochs after the onset, so the two largest rows of every
  episode are emitted before any response lands. Four of the six episodes have ONE code row
  after their second band is lost, unreachable at `gross_los_min_rows` 2. On the two BSHM00ISR
  episodes (a faulty station: the whole-station guard fired) the folded row runs away to 445 m
  and the LOS ends by expiry, never by release: the decoupling edge is unbounded (invariant 7).
  The satellite stand-down of the plan's phrase is the wrong shape for this fault (per line of
  sight, one station each) and its own loader banner records a held-out consumer at 56 m under
  it; the per-LOS fold is already consumer-safe at these thresholds (no census site moves,
  720 of 720 emitted).
- The prefit fuse cannot see the class: the fault does not exist before the solve.
- A birth floor removes the walk in proportion to its size and takes the WTZZ00DEU consumer to
  0.015 m at hours 3 and 4, but it slows every birth as well as every drop (RF-1 applies the
  floor at the first carried prior of a new arc too): hour 1 costs 0.10 m at WTZZ00DEU and
  0.03 to 0.04 m across the census, and the epochs below a decimetre fall at sites that hover
  near the threshold (DAV100ATA 335 to 249, ASCG00SHN 168 to 99, WUH200CHN 67 to 36) while
  others gain (HARB00ZAF 349 to 550 at 100 m2, BADG00RUS 185 to 242). The floor must apply at
  the DROP and not at the birth.

## 3. What is not it

- The code observables (five of six clean; the receiver's share of BSHM00ISR G22 is one epoch).
- RAIM (a code row carries zero on every ambiguity column: unreachable by construction).
- The prefit fuse (blind to four of six; do not arm).
- The satellite stand-down (`gross_los_sat_standdown`: the wrong shape and not consumer-safe).

## 4. The build, pre-registered (NOT built)

RF-3a, the drop-only floor:

- A1. `network.amb_float_drop_var_m2` (double, default 0 = today). At a DROP (every slip, T4, T3
  and gap re-entry response that restores the birth prior, `NetworkAmbFloat.cpp`) the restored
  variance is `max(var_first, amb_float_birth_var_m2, amb_float_drop_var_m2)`, elevation-mapped
  like the birth floor; the birth stash is untouched, so hour 1 is untouched. Counters in both
  knob states: drops, drops where the restored variance was the drop floor, and the largest
  |value change| the next fold absorbed (the walk's size).
- A2. Default OFF byte identity on both beds; not fingerprinted if the value is only a variance
  at a forward-only response (verify: RF-1's drop is not fingerprinted).
- A3. Tests: `tests/test_amb_float_vehicle.cpp` gains cases (a drop restores the drop floor when
  it exceeds the birth floor; the birth stash ignores it).

RF-3b, the gross-LOS screen made complete (card GL's rules):

- B1. `gross_los_min_rows` becomes a rule: `min(gross_los_min_rows, the LOS's code rows this
  epoch)`, so a one-row LOS is reachable and a multi-row LOS keeps the multipath discrimination.
- B2. The decoupling edge bounded: while a LOS is declared, its freed states are either held at
  their pre-declaration value with zero process noise or refused a contribution with a counter,
  so a folded row's residual stays a measure of the fault (today it runs to 445 m and the clear
  test can never fire). RELEASE becomes the normal exit and EXPIRY the exception, counted.
- B3. The declaration reaches the emission of the epoch it declares: the declared LOS's rows are
  excluded from that satellite's emitted clock and code bias for the epoch (the epoch still
  emits; a count when the satellite falls below `gross_los_sat_min_stations` contributors).
- B4. The station tier as a verdict: when the whole-station guard fires, the response is a
  station-level one with its own two edges, not N sequential LOS declarations.
- B5. Telemetry in both knob states: LOS-epochs over the bar with fewer rows than
  `gross_los_min_rows`; declarations ended by expiry against release; the largest residual of a
  folded row.

RF-3c, the wire contract on this path: a declared gross LOS whose response changes what a
satellite's emitted code bias or clock was built from writes the existing `code_disc` /
`clock_disc` tokens (the format header defines them; nothing writes them on this path).

Gates (6 h arms: the episodes sit in hours 4 to 6; rapid mode's 2 h is not the window):

- G0. Knobs absent: the three graded files identical on both beds.
- G1 (RF-3a). Field, `amb_float_drop_var_m2` at 4 and at 100 m2: rows above 3 m at full weight
  at most 110 (the `rf3_bv4` value) with hour 1 within 0.01 m of the baseline (0.560 m);
  `T4_attrib` at most 30; hours 3 to 6 at most 0.020 0.020 0.030 0.040 m; census against `grk`:
  hour 1 within 0.01 m at the median, hours 2 to 6 better at more sites than worse, absolute Up
  smaller at 9 or more of 16. Sim: within 0.005 m per hour of `runs/_rf0hv9_sim6`; the slipped
  sim bed (`rapid_arm.py` op `slip;types=L1?,L2?;rate=1.0;seed=1` on the sim base) within 0.02 m
  per hour of the same bed without the knob and with the walk metric (the largest |value change|
  at the first fold after a drop) smaller.
- G2 (RF-3b, on top of G1's winner). Rows above 10 m at full weight at most 10; declarations
  ending by expiry 0; the largest folded residual at most 50 m; the wire's first changed
  emission at the declaring epoch; census not worse than G1's winner; the consumer within 0.005 m
  per hour of it.
- G3 (RF-3c). The `code_disc` / `clock_disc` tokens present at every declaration that moved the
  emitted value by more than the wire's declared step; the consumer's discontinuity counters
  read them (card CLK-R's instrument).

Invariants pre-flight: 1 (RF-3a IS the re-coupling rule for the drop; RF-3b bounds the
decoupling edge), 3 (the satellite stand-down is a deliberate non-build, with data), 4 (all
counters in both knob states), 6 (byte identity), 7 (both edges of the screen are events; RF-3c
declares the step on the wire).

## 5. Build result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; 6 h arms)

Main `0436002`. RF-3a `network.amb_float_drop_var_m2` and RF-3b's four sub-knobs
(`gross_los_min_rows_rule`, `gross_los_bound`, `gross_los_emit_exclude`, `gross_los_station_tier`)
plus telemetry, all default OFF, byte-identical OFF on both beds (G0 PASS, and again on the final
exe). Every drop site routes through the drop floor (the five slip latches and the gap cliff, T4 and
T3; cards GRK's and SID's refloats use the same latch); the erases (the gap prune, the anchor
re-reference, the within-epoch drop, the gross-LOS erase) have no restored variance to floor. A
pre-existing inconsistency closed on the way: the RAIM sites passed the zenith birth floor where the
birth stash elevation-maps it (exact today at floor 0). The emission exclusion is the contributor-set
form: the emitted clock and code bias are joint-solve states with no per-contributor decomposition,
so the screen's DECISION runs ahead of the emission and the declared LOS leaves the satellite's
contributor set for the epoch (withheld below `gross_los_sat_min_stations`, else emitted with the
producer's discontinuity mark through the `ura_retro_sats` drain, whose predicate gained the knob:
a mark that would otherwise never land on a posture without the datum knobs). `AmbFloatVehicleTest`
+5, `NetworkGrossLosRf3b` 5 cases; `gnss_tests` 2505 ran, 2504 passed, 1 known skip; exe
`gnss_processor_rf3.exe` (`6486D125...`).

### 5.1 RF-3a, the drop-only floor

| | base | 4 m2 (`runs/_rap_rf3b_a4`) | 100 m2 (`_rap_rf3b_a100`) |
| --- | --- | --- | --- |
| rows above 3 m / above 10 m at full weight | 394 / 100 | 155 / 42 | 80 / 35 |
| `T4_attrib` | 81 | 23 | 19 |
| drops / floored | 1900 / 0 | 1827 / 1060 | 1827 / 1811 |
| WTZZ00DEU 3D p50 by hour (m) | 0.560 0.059 0.028 0.030 0.036 0.045 | 0.723 0.066 0.020 0.019 0.030 0.040 | 0.651 0.051 0.015 0.015 0.027 0.037 |
| below 0.1 m | 611 | 584 | 585 |
| clock wire (m) | 0.0045 / 0.0028 | 0.0044 / 0.0031 | 0.0043 / 0.0030 |
| census against `grk`: hour 1 median | | +0.045 m (5 better, 7 worse) | +0.034 m (3 / 6) |
| census hours 2 to 6 better / worse | | 4 / 2 (median -0.002 m) | 3 / 4 (-0.002 m) |
| absolute Up smaller | | 11 of 16 (median 0.0273 to 0.0228 m) | 10 of 16 (0.0249 m) |
| sim (`_rap_rf3b_a4_sim`) | | within 0.0002 m per hour of the sim base | |
| slipped sim bed, ON against OFF (hours 1 to 6, m) | | -0.025 +0.035 +0.060 +0.006 -0.046 -0.010; drops 12 858 to 12 831 | |

The floor removes the walk in proportion to its size and takes the settled consumer to 0.015 m,
and it costs hour 1: the statement of section 4 that the birth stash alone decides hour 1 was
wrong, since drops happen in hour 1 as well (332 `relevel_free` drops there on every arm; the
count does not change, the cost of each does, on a network that is still converging). The walk
metric as defined (the |value change| at the first fold after a drop) is LARGER with the floor
(a looser prior lets more correction through per fold: slipped bed medians 0.306 to 0.373 m at
the first fold, 0.107 to 0.159 m at the second); it measures the correction, not the residual walk,
and is kept as reported. Frozen G1 FAILS on the hour-1 bars at both values; a warm-up gate on the
floor is the next arm (section 5.4).

### 5.2 RF-3b, the screen's rules

| | base | `_rap_rf3b_gl` (available + bound + emit_exclude + station tier) | `_rap_rf3b_gl_nobound` (no bound) |
| --- | --- | --- | --- |
| rows above 3 m / above 10 m at full weight | 394 / 100 | 219 / 38 | 213 / 32 |
| declarations / releases / expiries | | 8 / 3 / 4 | 7 / 5 / 2 |
| largest folded residual (m) | | 2 352 | 445 |
| first changed emission | | the declaring epoch (TOW 269340) | the declaring epoch |
| station-tier declarations / guard firings | | 0 / 0 | 0 / 0 |
| WTZZ00DEU 3D p50 by hour (m) | 0.560 0.059 0.028 0.030 0.036 0.045 | 0.560 0.059 0.028 0.030 0.039 0.048 | 0.560 0.059 0.028 0.030 0.039 0.048 |
| clock wire (m) | 0.0045 / 0.0028 | 0.0042 / 0.0028 | 0.0041 / 0.0028 |
| census against `grk` | | counts 5 better 0 worse; hours 4 to 6 p50 better at 13 of 16; absolute Up smaller at 10 | the same |

- The one-row rule and the same-epoch emission work: the first changed emission lands at the
  declaring epoch (two epochs earlier than the pre-split screen), rows above 10 m at full weight
  100 to 32, the census better at 13 of 16 sites on hours 4 to 6 with none worse.
- `gross_los_bound` as built (the freed states held with zero process noise) is FALSIFIED: the
  largest folded residual grows 445 to 2 352 m, expiries 2 to 4, and the two episodes the erase
  repairs (STHL00GBR G11, CPVG00CPV G13) stop releasing. The erase IS the bounded response on this
  vehicle (the arc re-births uninformative and the code rows determine it within an epoch); the
  hold keeps the wrong value. KEEP OFF.
- The station tier's population is empty under the `available` rule: the bar becomes
  `min(2, rows)`, so BSHM00ISR's 5 of 7 lines of sight no longer clear `gross_los_max_station_frac`
  and the guard never fires; the tier is covered by its unit case only. The two expiries and the
  445 m folded row are BSHM00ISR, the faulty station, where the LOS's whole determination is the
  folded rows: the station tier under the `fixed` rule is the next arm.
- Frozen G2 fails on the expiry and the largest-residual bars (both BSHM00ISR) and passes the
  other four.

### 5.3 RF-3c, read only

`code_disc` is written from the emitter's per-(satellite, signal) counter (`NetworkProductEmitter.cpp`
about line 483), bumped by a constellation-wide datum change, a per-satellite mark, a
sum-to-zero channel change, the satellite's IF-pin band change or the opt-in jump test;
`clock_disc` from the per-satellite counter (about line 427), gated on `network.emit_clock_disc`,
datum components only. The screen's emission exclusion already reaches the mark through the
`ura_retro_sats` drain, at the UNION scope (`kDiscChanAll`, detail `union`): the tokens are written
today under `gross_los_emit_exclude`, without the channel the response moved or the tier's name. A
dedicated writer needs no format change: carry the channel, name the tier in the `R` record.

### 5.4 Next arms (in flight at the time of writing)

A warm-up gate on the drop floor (`network.amb_float_drop_floor_after_ep`), the floor at 100 m2
from epoch 120; the same with the screen (one-row rule and emission exclusion, no bound); and the
station tier under the `fixed` rule.

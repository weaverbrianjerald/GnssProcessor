# GAL: GPS and Galileo dual frequency on the re-founded posture

**STATUS**

- **id:** GAL
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT, NOT ARMED: Galileo E1 and E5a beside GPS improve the consumer on both beds (census median count 348 to 434.5); adoption waits on the frozen clock level it exposed.
- **source:** the author's decision of 2026-09-22; [RF-0c](RF-0c_gf_pin_2026-09-15.md) and [RF-0d](RF-0d_per_signal_pin_2026-09-15.md) (the pin and its cross-band rule), [RF-0f](RF-0f_isc_state_2026-09-15.md) (an inter-signal correction on disjoint station sets), [B5D](B5D_band5_code_datum_2026-09-20.md) (the assertion class), [SID](SID_signal_identity_2026-09-22.md) sections 11 and 12 (families and canonical stations), [SCC](SCC_second_code_channel_2026-09-21.md); the retired golden `config/golden/golden_sim_tri_w6.json` (GPS and Galileo triple on the pre-re-founding posture); `docs/SPEC_family_gf_datum.md` (the per-family geometry-free pin, built 2026-08-13 on the retired `code_bias_gf_datum` path, never run on data)
- **knobs / posture:** `processing.active_systems: ["GPS", "Galileo"]` with `processing.signal_priorities.Galileo` (C1C, C1X, L1C, L1X, C5Q, C5X, L5Q, L5X; E5b left out under `frequency_mode: dual`) on the RF-0hv working bases at main `fdfff99`; every new knob default OFF and byte-identical; nothing armed in the bases by this card
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** baseline pair `runs/_rf0hv10_{field,sim}6`; arms `runs/_rap_gal_*` (`_rap_gal_g2b_sim6`, `_rap_gal_g3x2_sim6`, `_rap_gal_g4c_field2`, `_rap_gal_g5_field6`, `_rap_gal_g5s_field6`); census keys `gal`, `galg` (plain wire, confounded), `gals`, `galgs` (contract-bearing wire) against `rf3gl2nb`; exe `gnss_processor_gal.exe` v3 `13b778f0...`; main `787eca5`

**Outcome.** A second constellation at every station was tested against the consumer errors left on the GPS-only posture. Frozen bars were MISSED: G2's inter-system bias, code-bias truth and GPS gross rows, G3's split-family recovery (C1X 0.60 to 0.88 m rms against 0.02), G5's GPS clock wire. G3 exposed the root (section 6.1): every satellite clock level frozen at ignition. That work passed from card [CLV](CLV_clock_level_2026-09-22.md) (superseded) to card [CTU](CTU_coupling_time_update_2026-09-28.md)'s exact update, armed by card [EXL](EXL_exact_update_lah_promotion_2026-10-01.md); the goldens stay GPS-only. Card [OWD](OWD_owed_defects_2026-09-22.md) fixed five other owed defects.

## 1. The decision and the reading it tests

Status of the GPS-only posture at the decision (baseline pair `runs/_rf0hv10_{field,sim}6`, 6 h
cold, the gross line-of-sight screen armed by card RF-3; every number re-read from the run
directories by the manager):

| bed | consumer 3D p50 by hour (m) | epochs below 0.1 m of 720 | clock wire, hours 2 to 6, all / tracked (m) | gross code rows above 3 m / 10 m |
| --- | --- | --- | --- | --- |
| field, WTZZ00DEU | 0.560 0.059 0.028 0.030 0.039 0.048 | 611 | 0.0041 / 0.0028 | 213 / 32 |
| sim, SZZ000SIM against truth | 0.598 0.103 0.096 0.056 0.055 0.049 | 448 | 0.0040 / 0.0030 | 595 / 1 |
| field, 16-site census (key `rf3gl2nb`), medians over sites | 0.670 0.169 0.104 0.077 0.092 0.070 | 348 | | |

The census splits. Seven sites settle at 0.02 to 0.06 m in hour 6 (QUIN00USA 0.020, ARHT00ATA
0.024, STPM00SPM 0.030, BAKE00CAN 0.030, HARB00ZAF 0.040, DAV100ATA 0.042, CEDU00AUS 0.057),
seven sit above 0.1 m with standing Up offsets (BADG00RUS 0.176 with a mean Up of -0.128,
WUH200CHN 0.177 / -0.167, POHN00FSM 0.181, UTQI00USA 0.167, ANMG00MYS 0.141, KOKB00USA 0.124 /
-0.084, ASCG00SHN 0.103), two lie between. The sim consumer carries a standing +0.045 m Up
against truth in hours 4 to 6 (+0.047 +0.046 +0.036). None of this is hygiene: over the week the
gross rows fell from 507 to 213 and the clock-anchor trips from 50 to 1 without moving those
sites.

The reading: what those sites and the first hour share is geometry (few satellites per station,
a slow separation of clock, wet delay and slant ionosphere), so the lever is the number of
satellites per station, and Galileo is the second constellation every roster station and every
census site tracks (section 2). GPS L5 adds a third frequency on 17 satellites and serves
integer resolution, which is not in play; it stays sequenced with its return plan (card L5P
section 4). The family ignition datum of card SID section 12 is off the consumer's critical path
and stays owed.

## 2. What is known (measured 2026-09-22, before any build)

**Tracking.** Every one of the 58 roster stations and the consumer track Galileo on E1, E5a and
E5b, in two disjoint channel families: 28 roster stations report C1C L1C C5Q L5Q C7Q (the pilot
channels) and 30 report C1X L1X C5X L5X C7X (the combined channels), as does WTZZ00DEU; no
station reports both. The sim fixture `runs/simnet60_quiet_wu93_24h` carries C1C L1C C5Q L5Q
C7Q L7Q at all 60 stations and at the consumer SZZ000SIM. Of the 16 census sites ten are on the
combined channels (CHTI00NZL, ANMG00MYS, QUIN00USA, BADG00RUS, STPM00SPM, ASCG00SHN, CEDU00AUS,
UTQI00USA, ARHT00ATA, WUH200CHN) and six on the pilot channels (SCRZ00BOL, HARB00ZAF,
POHN00FSM, DAV100ATA, KOKB00USA, BAKE00CAN).

**Products.** In `data/2024-129/products` the COD MGX final orbit carries 25 Galileo satellites
(7 225 E position records beside 9 248 GPS), the 30 s clock 72 000 Galileo records, the OSB file
Galileo biases for C1C, C1X, C5Q, C5X, L1C, L1X, L5Q and L5X, and the IGS broadcast file 9 803
Galileo navigation records (the source of the broadcast group delays).

**The inter-family truth on the satellite side (COD OSB, 25 satellites).** C1X minus C1C: mean
0.000 ns, sd 0.137 ns (0.041 m), extremes -0.24 and +0.38 ns; C5X minus C5Q: mean 0.000 ns, sd
0.161 ns (0.048 m), extremes -0.21 and +0.33 ns; L1X minus L1C and L5X minus L5Q identically
zero. A rule that asserts a zero inter-family correction per satellite therefore asserts a value
wrong by 0.05 m sd and up to 0.1 m: the class of card B5D at one twentieth of its size.

**The sim's Galileo truth.** `truth_bias.csv` carries the product OSB for every Galileo signal
(E02 C1C -0.316 m, L1C -0.258 m), `truth_ambiguities.csv` the Galileo arcs, `truth_windup.csv`
the Galileo wind-up, `truth_visibility.csv` the per-epoch GPS and Galileo counts (15 satellites
at the first epoch, 8 and 7), and every station a receiver inter-system bias of 8.000 m
(`isb_gps_gal_m` in `truth_clock.csv` and `truth_scalars.csv`); the quiet fixture has no slips on
either constellation. The satellite clock truth is the COD clock, as for GPS.

**What the record says.** On posture C (August 2026, the pre-re-founding slant posture) GPS and
the Galileo pilot family ran at 305 field stations for 4 h with no refusals
(`_pc305_ggq_tri_slant_4h`), and the run with both families withheld every epoch: the S-basis
gate counted 22 directions below its cutoff against 2 declared, because the pin pair of every
Galileo satellite was (C1C, C5Q) and the pins never touched a combined-channel observation
(`docs/SPEC_family_gf_datum.md`; its per-family pin was built on the retired `code_bias_gf_datum`
path and never run on data). RF-0f: an inter-signal correction between two channels tracked by
disjoint station sets is unobservable, and the channel left the wire (C2X at 2 stations). The
retired golden `golden_sim_tri_w6.json` ran GPS and Galileo triple with the pilot priorities
only. On the user side, adding Galileo to a static daily PPP at JPLM00USA with the COD products
moved the vertical by +0.028 m (2026-06-28; the code-domain suspects were ruled out, the receiver
antenna model was not).

## 3. The design

**3.1 The second constellation on the network path.** Every per-system quantity is made per
system where the code assumes GPS; nothing else changes. The list, each to be verified or built
and reported with a file and line:

- the receiver inter-system bias: one near-constant state per station (the existing
  system-time-offset state family, `filter.process_noise.system_time_offset_q` 1e-12, initial
  sigma 1000 m), on the producer and on the consumer;
- the Galileo satellite clock common mode: one null direction against the stations'
  inter-system biases, declared by the S-basis and closed by a datum row the run states (the
  clock datum station's bias, or a sum); the Q3 clock anchor per satellite unchanged;
- the satellite code datum per Galileo satellite: the ionosphere-free pin on the (C1C, C5Q)
  pair and the geometry-free pin at the broadcast group delay BGD(E1, E5a) of the navigation
  message, run-latched, the role T_GD plays for GPS (card RF-0c); the pin composes with the
  consumer's own free slant ionosphere whatever its value, so the datum's requirement is
  constancy over the run, not truth;
- the phase-bias families per (satellite, signal) with the anchor election per column (card
  RF-2), the roster and ledger per (station, system, band) (card SID), the slip detectors on the
  E1 and E5a wavelengths, the gap re-entry tiers, the gross line-of-sight screen (card RF-3);
- the products wire: C, B and P records for the Galileo satellites and the N declaration per
  system; the consumer reading them, allocating its inter-system bias and its Galileo
  ambiguities, and the per-satellite consumer trace;
- the models: Galileo satellite antenna offsets and variations from `igs20.atx` (E01, E05), the
  receiver antenna's Galileo frequencies where the file has them and the stated fallback where
  it does not (counted per station), the Galileo attitude law, the wind-up.

**3.2 The combined-channel family on the field.** Under the pin rules of RF-0d as they stand, a
Galileo satellite tracked by both families gets its ionosphere-free and geometry-free pins on
(C1C, C5Q); C5X is a cross-band extra and is pinned through its geometry-free combination
against C1C at the same BGD, which asserts C5X minus C5Q equal to zero at the satellite
(section 2: wrong by 0.048 m sd); C1X is a same-band extra and stays free with its family
common-mode row, determined by the 30 combined-channel stations alone (a bipartite structure
with one null direction, the common mode), so the rank closes. The alternative datum is the
per-family pin: the geometry-free combination of (C1X, C5X) pinned at the same BGD, no
ionosphere-free pin on the family (its ionosphere-free content per satellite is estimable
against the satellite clock the pilot family defines, up to the family common mode), and the
inter-family content carried on the wire in the family's own B records. That is
`docs/SPEC_family_gf_datum.md` re-expressed on the `sat_gf_datum` path. The card builds it as a
knob only if the sim emulation (3.3) shows the assertion costs; the field runs the pin rules as
they stand first, with the assertion counted (`n_sat_gf_isc_zero`).

**3.3 The sim emulation of the split.** `scripts/rapid_arm.py` relabels the Galileo types at 30
of the 60 stations (C1C to C1X, L1C to L1X, C5Q to C5X, L5Q to L5X, `sys=E`) and adds a
per-satellite constant N(0, 0.05 m) to C1X and to C5X (the size the OSB measures), the consumer
SZZ000SIM relabelled with them, so that the field's structure (two disjoint families, the
consumer on the combined family, a known inter-family truth) is graded against truth before the
field runs. The script's frequency table gains the Galileo bands for that.

## 4. The gates, sim first (rapid mode: 2 h for structure, 6 h for the truth grades, the census and adoption)

| gate | arm | bar |
| --- | --- | --- |
| G0 | `gal_g0_field2`, `gal_g0_sim2` (GPS only, 2 h) | `products.txt` (comment lines stripped), `closed_loop_trace.csv` and `network_epoch_status.csv` identical to `runs/_rf0hv10_{field,sim}6` over the window |
| G1 | `gal_g1_sim2` (GPS and Galileo pilot family, 60 stations, 2 h) | 240 epochs emitted; the S-basis's declared dimension equal to its count below the cutoff at every epoch; 25 Galileo satellites on the wire with C, B (C1C, C5Q) and P (L1C, L5Q) records; an inter-system bias state at 60 stations; the geometry-free pin at BGD for every Galileo satellite with 0 fallbacks; the run-end counters and the family census with Galileo lines |
| G2 | `gal_g2_sim6` (6 h) | consumer 3D p50 by hour not worse than 0.598 0.103 0.096 0.056 0.055 0.049 m by more than 0.010 m in any hour, and better in hours 1 and 2; epochs below 0.1 m not below 448; the mean Up per hour against truth beside the base's +0.047 +0.046 +0.036 m (hours 4 to 6); the clock wire per system against the COD clock (GPS not worse than 0.0040 / 0.0030 m, Galileo reported); the Galileo satellite code biases against `truth_bias.csv` in the realized datum (rms per signal; C1C and C5Q at or below the GPS pair's); the consumer's inter-system bias equal to 8.000 m less the datum the run states, within 0.05 m from hour 2; the GPS gross rows not above 595 / 1, the Galileo rows counted separately |
| G3 | `gal_g3x_sim6` (the split emulation, 6 h) | the wire's C1X and C5X biases against the injected constants after removing the family common mode: rms at or below 0.02 m from hour 3, with no satellite frozen (card SID section 12's signature: a per-satellite value that does not move from its first hour while the receiver states walk); the consumer on the combined family within 0.010 m per hour of G2's from hour 3; the combined family's gross rows not above the pilot family's. FAIL means the assertion of 3.2 costs and the per-family pin is built before the field |
| G4 | `gal_g4_field2` (58 stations, 2 h) | the family census (pilot 28 stations, combined 30); the declared dimension against the count below the cutoff; the pin counts (25 satellites at BGD, the zero-correction assertions on C5X counted); 25 Galileo satellites per epoch on the wire with both families' records; gross rows per system |
| G5 | `gal_g5_field6` (6 h) and the census | WTZZ00DEU 3D p50 by hour against 0.560 0.059 0.028 0.030 0.039 0.048 m: not worse by more than 0.010 m in hours 3 to 6, better in hour 1 or 2; the GPS clock wire not worse than 0.0041 / 0.0028 m; census key `gal` (GPS and Galileo consumers at the 16 sites) against `rf3gl2nb`: the median count of epochs below 0.1 m not below 348, more sites better than worse in hours 2 to 6 and in hour 1, and the seven sites above 0.1 m named with their hour-6 p50 and mean Up before and after; census key `galg` (GPS-only consumers on the same wire) to separate the wire's GPS half from the consumers' gain |
| G6 | cost | `loop_ms` of G1 against G0 on the sim and of G4 against G0 on the field; carried states per epoch |

Adoption is the author's decision, not this card's: the bases gain the two constellations, the
ledger a row, the goldens their pins, the baseline pair moves, and card L5P's return plan R-1
re-runs on the new base with three bands per constellation.

## 5. Risks named before the runs

- The receiver antenna calibrations: `igs20.atx` carries the Galileo frequencies for some
  receiver antennas only; the fallback per station is reported, and a vertical shift of the
  JPLM00USA kind (+0.028 m) would show in G2's Up bias on the sim (antenna TRM59800.00 NONE) and
  at WTZZ00DEU.
- The broadcast group delay: the Galileo navigation record carries two (E5a/E1 and E5b/E1); the
  pin takes E5a/E1 and states it; the value only needs to be constant over the run.
- E5b left out: `frequency_mode: dual` selects the two lowest distinct bands per system (1 and 5
  for Galileo); the C7 and L7 types in the files are not modelled; a Galileo triple is card
  L5P's ground, later.
- The census's ten combined-channel sites replay a wire whose combined family carries the
  assertion of 3.2; G3 measures that assertion's cost on the truth bed first.
- Time: the sim 6 h loop is 169 s and the field 229 s at 20 threads on the GPS-only posture; the
  second constellation is expected to add of the order of half again.

## 6. Result (2026-09-22; Opus build worker; every number re-graded by the manager from `runs/` unless marked as the worker's)

Main `787eca5`. What was already per system on both paths: the inter-system bias as a free
per-epoch column per station (`StateManager.cpp:292-315`; no process noise on the producer,
a Kalman state on the consumer), one clock pivot per constellation (`NetworkSbasis.cpp:282-317`;
the Q3 anchor with the per-constellation median removed, the emitted frame hold per
constellation), the code and phase bias families per (satellite, signal), the slip detectors
on the per-system wavelengths (the E1/E5a MW gate is 8 sigma at 1.42 cycles against 1.44 for
L1/L2), the wire keyed on the satellite id, the consumer's reader. What was GPS-only and is
built: the geometry-free pin gave the broadcast target only to a (band 1, band 2) pair, so a
Galileo satellite was pinned at zero while the time-linked anchor of an extra channel would
have taken BGD, a metre-level inconsistency between the two channel families; the pin now
takes c times BGD(E1, E5a) for a (band 1, band 5) Galileo pair, run-latched, and the extra-channel
anchor uses the pair's target. Also built: pin counters and run-end lines per constellation
(`NetworkMultiGnss`), a loader consistency check between `active_systems` and
`signal_priorities` (an error for a Galileo list without the system), the per-station receiver
antenna source line, `closed_loop_isb.csv`, the inter-system bias rows of
`network_rx_states.csv`, `scripts/rapid_arm.py` with the Galileo frequencies (selftest 86),
`--systems` on the two wire graders (default output byte-identical), `scripts/gal_grade.py`,
`GalileoNetworkTest` (15 cases); `gnss_tests` 2600 ran, 2599 passed, 1 known skip;
`--verify-loader` 0 gaps; golden `--verify` PASS.

Three defects fixed beyond the brief. (1) The satellite-clock validity bound of 1 ms in
`GnssCorrections.cpp` rejected E05 (+4.75 ms), E11 (+2.91 ms) and E12 (-1.03 ms), so the field
wire carried 22 Galileo satellites instead of 25; the bound is 0.1 s. The sim fixture was
generated under the old bound and lacks those three satellites until it is regenerated. (2) The
inter-signal verdict loop of `NetworkSolveGlue.cpp` ran with `network.sat_isc_state` off and
erased every C5X code bias from the wire; it is gated on the knob. (3) The consumer's
inter-system bias froze across a declared per-constellation clock re-reference: the wire
re-references each constellation's frame once in the run (GPS +1.36 m at 4.71 h on the sim,
present in the GPS-only baseline and harmless to a one-constellation consumer; Galileo +0.54 m at
3.34 h), both declared by the E record and the R record. The first sim run diverged to 2.44 m in
hour 6 for that reason. The existing consumer knob `estimation.clock_disc_isb_reinflate_std`
(default 0; set to 10 on every Galileo arm from G1b on) lets the bias re-solve; its reset sets
the variance rather than raising it, recorded as owed.

| gate | measured | verdict |
| --- | --- | --- |
| G0 | `products.txt` (comments stripped), `closed_loop_trace.csv`, `network_epoch_status.csv` identical to `runs/_rf0hv10_{field,sim}6` over 2 h on both beds, across the three exe versions, with and without the antenna fallback flag | PASS |
| G1 (`runs/_rap_gal_g1b_sim2`) | declared 2 = 2 below the cutoff at every epoch; pivots E02 and G01; Galileo pin rows 4 661 at a non-zero BGD and 480 at a broadcast BGD of exactly 0 (E03, E21), 0 fallbacks; inter-system bias at 60 stations, 0 Galileo-only station-epochs; the wire 21 to 22 Galileo satellites per epoch, 22 distinct (the fixture lacks E05, E11, E12), every C, B and P record present | PASS except the 25-satellite count, a fixture limit |
| G2 (`runs/_rap_gal_g2b_sim6`) | consumer 0.519 0.061 0.037 0.032 0.035 0.026 m against 0.601 0.104 0.096 0.056 0.055 0.049; 587 epochs below 0.1 m against 448; mean Up against truth hours 2 to 6 -0.040 +0.015 -0.028 -0.021 -0.006 m against +0.080 +0.087 +0.047 +0.046 +0.036; clock wire GPS 0.0033 / 0.0029 m against 0.0040 / 0.0030, Galileo 0.0044 / 0.0035; the Galileo code biases against `truth_bias.csv` within-arc rms E C1C 0.0056 and E C5Q 0.0032 against G C1C 0.0055 and G C2W 0.0032 (worker); the consumer's inter-system bias 0.5 to 0.95 m from 8.000 m (worker; section 6.1); GPS gross rows 645 / 0 against 595 / 1, Galileo 399 / 0 | consumer, count, wire PASS; bias truth FAIL by 0.1 mm; inter-system bias FAIL; GPS rows above 3 m FAIL |
| G3 (`runs/_rap_gal_g3x2_sim6`) | headers verified (30 stations and the consumer on C1X L1X C5X L5X, 30 on the pilot types); the injected constants identical at every station and the consumer (E30 C1X -0.028 m, E08 +0.077 m); the wire's C1X minus C1C against the constants, common mode removed: rms 0.881 0.868 0.869 0.876 0.866 0.596 m by hour, per satellite E30 +3.15, E08 +1.34, E09 -1.04 m with ranges of 0.05 to 0.09 m over hours 2 to 6; C5X minus C5Q rms 0.029 0.028 0.028 0.027 0.027 0.028 m; the consumer 0.299 0.063 0.045 0.035 0.042 0.029 m (599), within 0.010 m of G2 from hour 3; gross rows combined 218 / 1 against pilot 745 / 6; declared 2 = 2 | recovery FAIL for both signals; consumer PASS; rows PASS |
| G4 (`runs/_rap_gal_g4c_field2`) | family census pilot 28, combined 30; declared 2 = 2 at every epoch; pin rows 5 381 at BGD plus 480 at zero; 4 852 zero-correction assertions on C5X; the wire 24 to 25 Galileo satellites per epoch, 25 distinct; no Galileo row through the second code channel; receiver antenna source 52 exact, 6 on the GPS fallback (JPLM00USA AOAD/M_T NONE, STJO00CAN TWIVC6150 NONE, KOKV00USA ASH701945G_M NONE, SYOG00ATA TRM159900.00 SCIS, PARK00AUS LEIAR25 NONE, SVTL00RUS JAVRINGANT_DM JVDM), WTZZ00DEU exact | PASS |
| G5 (`runs/_rap_gal_g5_field6`; `_g5s_field6` = the same solve with `network.serialize_sbasis_contract`, byte-identical) | WTZZ00DEU 0.197 0.063 0.032 0.006 0.010 0.009 m against 0.560 0.059 0.028 0.030 0.039 0.048; 623 epochs below 0.1 m against 611; mean Up hours 2 to 6 +0.013 +0.021 -0.003 -0.006 -0.006 m against +0.014 -0.004 -0.024 -0.034 -0.046; clock wire GPS 0.0047 / 0.0029 m against 0.0041 / 0.0028, Galileo (L1X, L5X) 0.0040 / 0.0025; gross rows GPS 168 / 32 against 213 / 32, Galileo 958 / 14 of which C1C 749 (E12 715 rows at DARW00AUS, HKSL00HKG, PARK00AUS and others), C5Q 143, C1X 11, C5X 55; the combined family's records on 24 to 25 satellites per epoch, the pilot family's with 1 826 (C1C) and 2 201 (C5Q) satellite-epoch holes | WTZZ PASS; GPS wire FAIL by 0.0006 m; GPS rows PASS |
| G5 census, plain wire (keys `gal`, `galg`) | `gal`: median count 248.5 against 348, hours 2 to 6 worse at all 16 sites (median +0.356 m); every site diverges after the two re-references (the WTZZ00DEU replay 0.256 0.085 0.044 0.557 0.595 2.804 against the in-process 0.197 0.063 0.032 0.006 0.010 0.009) | confounded: the replay reader has no clock-discontinuity channel without the S contract |
| G5 census, contract-bearing wire (keys `gals`, `galgs`) | the WTZZ00DEU replay identical to the in-process consumer; `gals` (GPS and Galileo consumers): median count 434.5 against 348, 14 sites better and 2 worse, hours 2 to 6 better at 10 and worse at 2 (median -0.028 m), hour 1 better at 11 and worse at 4 (median -0.324 m); `galgs` (GPS-only consumers on the same wire): 244, 6 better and 10 worse, hours 2 to 6 3 and 6 (median +0.004 m) | `gals` PASS on every bar; the wire's GPS half costs GPS-only consumers a little |
| G6 | loop per epoch sim 219 to 544 ms (2.5 times), field 287 to 1 252 ms (4.4 times); carried ambiguities sim 930 to 1 577, field 875 to 1 477; B records per epoch sim 64 to 106, field 96 to 183 (worker) | read |

The seven census sites above 0.1 m, hour-6 p50 and mean Up (m), `rf3gl2nb` to `gals` (worker;
the `gals` counts re-graded): BADG00RUS 0.183 / -0.095 to 0.122 / -0.073; WUH200CHN 0.157 /
-0.132 to 0.176 / -0.088; POHN00FSM 0.160 / +0.093 to 0.058 / +0.042; UTQI00USA 0.131 /
+0.088 to 0.085 / +0.045; ANMG00MYS 0.188 / +0.085 to 0.169 / +0.055; KOKB00USA 0.044 / -0.009
to 0.050 / -0.024; ASCG00SHN 0.104 / -0.085 to 0.073 / -0.054. Counts: POHN00FSM 0 to 323,
ASCG00SHN 180 to 428, KOKB00USA 125 to 295, UTQI00USA 13 to 108, BADG00RUS 185 to 286,
ANMG00MYS 204 to 394, WUH200CHN 69 to 103.

**6.1 The reading: the satellite clock level is frozen at an ignition value, posture-wide.**
G3's recovery failure is not a family datum. On the GPS-only sim baseline `runs/_rf0hv10_sim6`,
where the a priori clock is the truth, the wire's per-satellite clock error has an rms across
satellites of 1.29 m at epoch 1, 1.50 at epoch 4, 1.49 at epoch 16, 1.47 at epoch 64 and 1.45 at
epoch 480 (with the per-epoch median removed: 1.19, 1.47, 1.48, 1.45, 1.46); G16 reads -0.67
-2.29 -2.96 -3.50 -3.87 m over epochs 1 to 6 and then -3.5 m for the rest of the run, G03 +0.47
to +2.98 by epoch 8 and +2.97 at hour 6. The per-satellite median ionosphere-free code residual
over hours 2 to 6 runs from -2.64 to +1.89 m (sd 1.01 m across 32 satellites) and is the same
hour to hour (G16 -2.53 -2.76 -2.71 -2.63 -2.54 -3.27); the GPS and Galileo sim arm shows the
same (-2.83 to +1.75 m, sd 1.04), and the field baseline's wire sits 0.85 m rms per satellite
from the COD clock with the same constancy (code residual medians -2.29 to +0.53 m). Part of
the sim offset is the datum (the network pins the ionosphere-free combination of C1C and C2W at
zero where the product's clock datum is P1/P2: the offsets correlate -0.67 with the product's
own IF(C1C, C2W) bias); the rest is error, and the code residuals prove it: per satellite the
C1C and C2W residual medians are +0.29 and -0.17 times the clock offset, exactly the pattern of
a wrong clock level of which the free slant ionosphere absorbs 71 percent per line of sight and
the code keeps the rest, so the ionosphere levels of every station are wrong per satellite as
well. The phase is consistent throughout (the ambiguities absorb a constant), which is why the
float consumer, the within-arc wire instrument and every card since RF-0 never saw it;
`simnet_grade.py`'s clock residual of 1.75 m rms had been read as a datum.

Twelve config-only arms on the sim base, 2 h, the clock error rms across satellites at epochs
4 / 16 / 60 / 240 and the code-residual spread in hour 2 (base 1.50 / 1.49 / 1.47 / 1.47, sd
0.97 m): the Q3 anchor off, its variance floor at 1 m2, its walk at 1e-3 (`_rap_cil_e2`, `_e3`,
`_e4`): 1.50 / 1.64 / 1.55 / 1.51, sd 1.1 (the anchor is not the cause; it only suppresses the
walk, 0.003 against 0.008 m, and its removal costs the consumer 0.6 to 1.0 m in hour 1); the
leveling young-arc inflate, variance floor and level q (`_e5`, `_e6`, `_e7`): 1.36 to 1.46 at
epoch 240; the float vehicle's birth floors (1 and 4 m2), its process noise and the local-block
and in-block carry knobs (`_e8`, `_e11`, `_e11b`, `_e12`, `_e13`, `_e14`): byte-identical to the
base (inert on this posture); the filter's ambiguity q (`_e9`): the level unchanged and the
consumer destroyed (10.5 m). One lever moves it: `network.phase_leveling_warmup_epochs: 76`
(`_rap_cil_e10_warm`, the harvest of the ambiguity carry delayed to epoch 76): 1.50 / 1.16 /
0.80 / 0.78 with the code-residual spread 0.97 to 0.12 m (range -0.18 to +0.37), the remaining
0.8 m being the datum share. The level therefore converges as the code accumulates for as long
as the ambiguities solve free, and freezes at whatever value it holds on the epoch the carry
begins (the default: 6 emitted epochs, when the single-epoch code solves put it 1.5 m rms off).
Card RF-IGN measured the same lever on the field golden. The mechanism is the one the float
vehicle's design note names (RF-1 section 1.3: the carried priors count the level's information
as phase-precise, "the level freezes at K_eff = (B+1)/(B-1) epochs", 3 for two bands): the
carried satellite phase bias and ambiguities hold the (clock, phase bias) level direction at
phase stiffness, so the code rows' level information never reaches the clock. Delaying the
harvest is not the fix (it costs the consumer 2.1 and 0.61 m in hours 1 and 2 and the wire's
walk 0.12 to 0.18 m); the fix is an honest level variance under the carry, the next card.
It surfaces with two constellations: a second channel family with a free ionosphere-free level
absorbs the clock error into its code biases while the pinned family carries it in residuals
(G3; E12's 715 gross C1C rows on the field), and the consumer's inter-system bias reconciles two
constellations' frozen levels (G2). The wire's code corrections are wrong per satellite at the
metre level for any code-based user, on the GPS-only posture as well. The author's decision of
2026-09-22: fix this root before adoption.

**6.2 Owed from this card** (the author chose to fix the roots before continuing):
the frozen clock level (6.1); the replay reader's clock-discontinuity channel gated on
`serialize_sbasis_contract` (a declaration gated on an output option, the product-legend
lesson; every multi-constellation replay needs it today); the consumer's inter-system bias
reset that sets the variance and can shrink it (`StateManager.cpp:1703-1705`); the
group-delay latch on the first navigation record (E21's first record is 0 while the later ones
are -0.23 to -2.3 ns; the largest within-day spread is 0.698 m on E24); the sim fixture without
E05, E11 and E12 (regenerating it under the corrected bound moves every sim baseline); the
Galileo yaw law (nominal yaw steering only; eclipses handled by dropping the satellite); the
BeiDou group-delay pair rule; the GPS clock wire 0.0041 to 0.0047 m on the field, unattributed;
two archived GPS-only configs that carry a Galileo list and are now refused by the consistency
check (`config/test_suite/ppp_rtk_network_global_5min_gpsonly.json`,
`config/archive/test_suite/_sdewl_probe_gps10.json`); post-fit rows touching only the clock
column written without a signal. Adoption of the two constellations, with
`estimation.clock_disc_isb_reinflate_std` and `network.serialize_sbasis_contract` on the
consumer and wire side, waits on 6.1.

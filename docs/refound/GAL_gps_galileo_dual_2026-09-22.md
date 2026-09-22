# GAL: GPS and Galileo dual frequency on the re-founded posture

**STATUS**

- **id:** GAL
- **topic:** network-vehicle
- **status:** open
- **summary:** Build card, pre-registered 2026-09-22 by the author's decision (the second constellation before GPS L5 and before the family ignition datum of card SID). Galileo E1 and E5a join GPS L1 and L2 on the RF-0hv working posture, the sim first (a clean pilot-channel family at 60 stations against truth, then an emulation of the field's disjoint pilot and combined channel families with an injected inter-family truth), then the field (28 stations on C1C/C5Q, 30 on C1X/C5X, the consumer WTZZ00DEU on C1X/C5X) with the 16-site census. The reading under test: the consumer errors left on the GPS-only posture are geometric and regional (the 0.56 m first hour, the sim's standing +0.045 m Up, the 0.10 to 0.18 m settled errors with standing Up offsets at the Asian, Pacific and high-latitude census sites), and a second constellation at every station addresses them where a third GPS frequency does not.
- **source:** the author's decision of 2026-09-22; [RF-0c](RF-0c_gf_pin_2026-09-15.md) and [RF-0d](RF-0d_per_signal_pin_2026-09-15.md) (the pin and its cross-band rule), [RF-0f](RF-0f_isc_state_2026-09-15.md) (an inter-signal correction on disjoint station sets), [B5D](B5D_band5_code_datum_2026-09-20.md) (the assertion class), [SID](SID_signal_identity_2026-09-22.md) sections 11 and 12 (families and canonical stations), [SCC](SCC_second_code_channel_2026-09-21.md); the retired golden `config/golden/golden_sim_tri_w6.json` (GPS and Galileo triple on the pre-re-founding posture); `docs/SPEC_family_gf_datum.md` (the per-family geometry-free pin, built 2026-08-13 on the retired `code_bias_gf_datum` path, never run on data)
- **knobs / posture:** `processing.active_systems: ["GPS", "Galileo"]` with `processing.signal_priorities.Galileo` (C1C, C1X, L1C, L1X, C5Q, C5X, L5Q, L5X; E5b left out under `frequency_mode: dual`) on the RF-0hv working bases at main `fdfff99`; every new knob default OFF and byte-identical; nothing armed in the bases by this card
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** baseline pair `runs/_rf0hv10_{field,sim}6`; arms `runs/_rap_gal_*`; census keys `gal` and `galg` against `rf3gl2nb`; exe `gnss_processor_gal.exe`

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

# SFR: the sim fixture re-based with the field's coloured phase error

**STATUS**

- **id:** SFR
- **topic:** sim-bed
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (the outside review's decision after card FWS: a re-base, sequenced before any hold work for the real-time posture). The sim's phase error is the wrong shape for every persistence-sensitive result: its ionosphere-free post-fit phase residual is about 80 % white (14 mm) with a weak 5-minute multipath, where the field's is about 90 % a Gauss-Markov process (5 to 7 mm, tau 80 to 120 s, strongest in the tropics) over about 2 mm of white noise (section 1). The re-base adds a per-line-of-sight, band-common Gauss-Markov phase error to the simulator, calibrated per station class on the post-fit instrument (the field's raw error is not observable), and cuts the white phase noise to the field's level in the same regeneration. The old fixture's numbers become superseded, not wrong.
- **source:** [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 2.1, 5 and 6; [LSR](LSR_long_scale_remainder_2026-10-01.md) section 6; `config/sim/simnet60_quiet_24h.json` (the current recipe, commit `2306134`); `include/sim/SimConfig.hpp` (the noise and multipath blocks); `docs/refound/GRB_golden_rebase_2026-09-22.md` (the golden re-base recipe)
- **knobs / posture:** the simulator's new phase-colour block (default amplitude 0, byte-identical); `noise.phase_sigma_zen_m`; the sim golden's base re-pointed at the new fixture
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** the calibration instrument `scripts/clock_level/phase_acf.py`; the old fixture `runs/simnet60_quiet_wu93_24h`; the new fixture directory named in section 4

## 1. The target (`phase_acf.py`, 6 h runs, 2026-10-01)

Per station, the ionosphere-free phase residual 2.5457 v(L1) - 1.5457 v(L2W) over arcs of at least
40 epochs, de-meaned per arc; the class median of the autocorrelation at 1 / 2 / 5 / 20 / 60 epochs,
the median rms, and the white-plus-Gauss-Markov split of the class median (phi = acf(2) / acf(1)):

| run | class | stations | rms | acf 1 / 2 / 5 / 20 / 60 | GM share, tau | sigma GM / white |
| --- | --- | --- | --- | --- | --- | --- |
| field golden `_rf0hv11_field6` | tropical | 23 | 7.20 mm | 0.74 / 0.58 / 0.32 / 0.07 / -0.01 | 0.94, 121 s | 6.99 / 1.72 mm |
| | mid (23.5 to 50 degrees) | 18 | 5.12 mm | 0.64 / 0.47 / 0.23 / 0.02 / -0.02 | 0.87, 97 s | 4.77 / 1.86 mm |
| | high | 17 | 5.39 mm | 0.61 / 0.46 / 0.21 / 0.00 / -0.03 | 0.79, 112 s | 4.79 / 2.46 mm |
| sim golden `_rf0hv11_sim6` | tropical | 26 | 16.87 mm | 0.21 / 0.18 / 0.08 / 0.00 / -0.01 | 0.23, 223 s | 8.17 / 14.76 mm |
| | mid | 22 | 15.43 mm | 0.20 / 0.17 / 0.08 / 0.01 / -0.02 | 0.23, 200 s | 7.38 / 13.55 mm |
| | high | 12 | 16.60 mm | 0.20 / 0.16 / 0.08 / 0.02 / -0.02 | 0.24, 150 s | 8.17 / 14.45 mm |

The program posture's field arm (`_rap_rev1_blk6_f`) reads the same within 0.05 in every
autocorrelation. Strongest field stations (lag-1): SEYG00SYC 0.91, SOLO00SLB 0.90, MAJU00MHL 0.89,
URUM00CHN 0.85, HKSL00HKG 0.84, JPLM00USA 0.82, LCK400IND 0.82, SGOC00LKA 0.80.

## 2. The build

- **The simulator.** A phase-only, band-common (the same metres on every band, so the
  ionosphere-free combination keeps all of it and the free slant ionosphere none), per-line-of-sight,
  first-order Gauss-Markov error: sigma at zenith and tau, elevation-scaled like the white noise, a
  stationary start, phi = exp(-dt / tau) across a visibility gap. Global defaults with amplitude 0
  (every existing recipe regenerates byte-identical), a per-station override through the roster (the
  `scripted_slips` pattern), its own salted stream, a truth file of the realization, selftest cases.
  The existing multipath block (one shared code-and-phase draw per band) is left as it is.
- **The null gate.** The current simulator with the current recipe regenerates
  `runs/simnet60_quiet_wu93_24h` byte for byte (the `src/sim` changes since the fixture are comments,
  but the simulator calls `gnss_core` models that have changed since; if it does not, the
  differences are recorded and the re-base is graded against a same-exe regeneration).
- **The calibration.** Short fixtures (2.5 h) and 2 h sim golden arms graded with `phase_acf.py`;
  the white sigma and the per-class (tropical, mid, high by the station's latitude) sigma and tau
  adjusted until section 3's C1 holds.

### 2.1 The build and the null gate (2026-10-01)

**Built** (main, this commit): `include/sim/PhaseColour.hpp`
(`sim::PhaseColourGenerator`, the per-line-of-sight unit-variance first-order Gauss-Markov recursion:
a stationary start at first sight, phi = exp(-dt / tau) over the time since the satellite's previous
visible epoch, its own `mt19937_64` and its own normal distribution, no draw and an exact 0.0 when
sigma is 0); the `phase_colour` block of `SimConfig` (sigma at zenith, default 0; tau, default 100 s;
the elevation floor, default the white noise's; a dedicated seed) with per-station overrides through
the roster (`phase_colour_sigma_zen_m`, `phase_colour_tau_s`, copied into the child config by
`writeStationConfig` only when present); in `src/sim/main.cpp` one draw per satellite and epoch,
added in metres to every band's carrier phase and to no code, `truth_phase_colour.csv` and the
summary lines written only when the block is on. `gnss_simulator_selftest` 99 of 99 (80 earlier and
19 new, suites `SimPhaseColour`, `SimPhaseColourConfig`, `SimPhaseColourRoster` in
`tests/sim/test_sim_phase_colour.cpp`). Exe `gnss_simulator_sfr1.exe`.

**The null gate, as section 2 wrote it, cannot pass, and not because of the block.** No simulator
built from the current source regenerates `runs/simnet60_quiet_wu93_24h`: card GAL (`787eca5`,
2026-09-22) raised `MAX_CLOCK_BIAS` in `src/core/corrections/GnssCorrections.cpp` from 1e-3 to 1e-1 s,
so three Galileo satellites whose a priori clocks exceed a millisecond (E05, E11, E12) are now
synthesised where the old forward model dropped them, and three more satellites reorder every draw
of every stream. The archived simulator of the fixture (`gnss_simulator_fable_24h.exe`) still
reproduces it byte for byte on the current data, and HEAD's simulator built beside the new one
matches it 13 of 13 files on three stations with the block off: that is the block's byte-identity.
The re-base is therefore graded against a regeneration by the same exe (candidate `k0` of section
3.1, no change), not against the archived fixture. The golden ignores Galileo, but the GPS
realizations move with the draw order.

**A defect found on the way, not fixed here.** `src/sim/main.cpp` shares one
`std::normal_distribution` object between the white-noise stream and the wet-delay stream
(`tropo_rng`); the value the distribution caches between calls crosses streams, so the "dedicated"
wet-delay series changes whenever the number of white-noise draws does. The new block has its own
distribution. Whether to give the wet-delay stream its own distribution in the same regeneration is
recorded at the regeneration (it moves the wet-delay truth, which the re-base moves anyway).

**The review** (an independent adversarial read of the diff, 2026-10-01): no defect blocks it; the seven
claims hold (off draws nothing and changes no byte; the recursion is a stationary unit-variance AR(1) on the
time since the satellite's previous visible epoch; band-common in metres on every phase band and never on
code, |delta phase x lambda - e| at most 0.25 mm and the geometry-free part at most 0.37 mm, both the RINEX
rounding; the roster override round-trips and an absent one inherits; the elevation factor is the white
noise's). Open: no automated test runs the simulator's wiring itself (the selftest is product-free; the
band-common case reproduces the formula), so the wiring rests on the two manual RINEX checks; the process
also advances on an epoch the RINEX later drops (fewer than 5 satellites), statistically harmless; a
non-object `phase_colour` value reads as off, as for the other blocks.

## 3. Predictions, written before any build

- **C1 (the calibration closes).** Per class, the re-based sim's post-fit residual matches the
  field's within 0.10 in the autocorrelation at 1, 2 and 5 epochs and within 30 % in rms.
- **C2 (the short lags follow).** The walk-share persistence ratio (`wsf_persist.py`) of a 2 h sim
  golden-posture arm on the program posture's instrument rises at 5 and 20 epochs to within 0.2 of
  the field's (1.45 and 1.51 on the accumulated-wind-up arm), as card FWS's 30 mm sinusoid arm did.
- **C3 (the long lags do not).** At 60 and 120 epochs the ratio stays below the field's by more than
  0.3: the field's long-scale remainder is not this colour (cards FWS, SOI, LSR), and a fixture that
  matched it here would be matching something else.
- **C4 (what moves).** The re-base moves every persistence-sensitive sim number; each is re-captured
  on the new fixture and the old one annotated as superseded: card PVN's within-arc NEES, card SMH's
  G2 and G3, card SMD's sim floor, card QPV's rule, cards REV and SMC's revision NEES, cards CTU and
  CTU2's G1 and G2, the sim census baselines and the sim golden's baseline pair.

If C1 cannot close with one Gauss-Markov term per class, the residual's shape needs a second
component (card LSR's untested field ingredients, a phase-centre-like error swept by the geometry
over a pass or a long-period multipath), recorded before it is added.

### 3.1 The calibration ladder, predicted before it ran (2026-10-01)

Candidates from the 24 h fixture's own per-station child configs (`sfr_calib.py genp`: the station,
its seed salt and biases as generated; only the span, the white phase noise, the multipath phase
amplitude and the colour block change), 2 h sim golden arms from 00:30:15, graded with
`phase_acf.py`. The sim's existing multipath (2 mm at zenith per band, tau 300 s, one draw per band
shared with the code multipath) already gives the post-fit residual a Gauss-Markov part of about 7 to
8 mm that its 14 mm of white noise hides; its time constant is about twice the field's.

| candidate | white phase sigma (zenith, per band) | multipath phase amplitude | colour (sigma at zenith, tau) |
| --- | --- | --- | --- |
| `k1` | 0.5 mm (from 3 mm) | 2 mm (unchanged) | none |
| `k2` | 0.5 mm | 0 | tropical 5 mm, 120 s; mid 3.5 mm, 100 s; high 3.5 mm, 110 s |

- **K1.** `k1` raises the class-median lag-1 autocorrelation to at least 0.6 but its time constant
  (from the median acf(2) / acf(1)) reads above 150 s, longer than the field's 80 to 120 s.
- **K2.** `k2` meets C1 in every class (within 0.10 at 1, 2 and 5 epochs, rms within 30 %), or the
  ladder's next rung is set from `k2`'s miss by scaling sigma by the rms ratio and tau by the
  ratio of the fitted time constants.

### 3.2 Two more rungs, predicted before they ran (2026-10-01, after `k3`)

`k3` (section 4) matches the rms within 3 % and the autocorrelation at 1 and 2 epochs within 0.07 in
every class, and reads 0.15 low at 5 epochs in every class: the field's colour has a heavier tail than
one exponential (its acf(5) is above a single-exponential extrapolation of its acf(1) and acf(2), the
sim's below). Two candidates, run together:

| candidate | white | multipath phase | colour (sigma at zenith, tau) |
| --- | --- | --- | --- |
| `k4` (section 3.1's rule on `k3`'s miss) | 0.5 mm | 0 | tropical 4.89 mm, 203 s; mid 3.43 mm, 164 s; high 3.28 mm, 238 s |
| `k5` (two components) | 0.5 mm | 1.2 mm (tau 300 s, per band) | tropical 4.0 mm, 150 s; mid 2.6 mm, 120 s; high 2.4 mm, 150 s |

- **K4.** The longer time constant lifts acf(5) by at least 0.08 in every class, and acf(1) by no more
  than 0.08, so `k4` meets C1 at 5 epochs in at least two classes.
- **K5.** The second, slower component lifts acf(5) to within 0.10 of the field in every class while
  acf(1), acf(2) and the rms stay within C1: `k5` meets C1 in every class.

The candidate that meets C1 in more classes becomes the regeneration recipe (ties to the one with the
smaller summed lag-5 miss).

## 4. The calibration (2026-10-01)

Candidates by `sfr_calib.py genp` with `gnss_simulator_sfr1.exe`; 2 h sim golden arms from
00:30:15 on `gnss_processor_wup2.exe`; `phase_acf.py`. `k0` (the current recipe, the same exe)
is the control the ladder is read against, not the archived fixture (section 2.1).

| candidate | class | rms | acf 1 / 2 / 5 / 20 / 60 | GM share, tau | sigma GM / white |
| --- | --- | --- | --- | --- | --- |
| field golden (the target) | tropical | 7.20 mm | 0.74 / 0.58 / 0.32 / 0.07 / -0.01 | 0.94, 121 s | 6.99 / 1.72 mm |
| | mid | 5.12 mm | 0.64 / 0.47 / 0.23 / 0.02 / -0.02 | 0.87, 97 s | 4.77 / 1.86 mm |
| | high | 5.39 mm | 0.61 / 0.46 / 0.21 / 0.00 / -0.03 | 0.79, 112 s | 4.79 / 2.46 mm |
| `k0` (control) | tropical | 14.48 mm | 0.14 / 0.12 / 0.06 / -0.01 / -0.01 | 0.15, 289 s | 5.63 / 13.34 mm |
| | mid | 12.72 mm | 0.13 / 0.10 / 0.04 / 0.01 / -0.04 | 0.17, 110 s | 5.22 / 11.60 mm |
| | high | 14.13 mm | 0.11 / 0.12 / 0.02 / -0.00 / -0.01 | | |
| `k1` (white 0.5 mm) | tropical | 7.20 mm | 0.72 / 0.58 / 0.28 / -0.02 / -0.05 | 0.89, 141 s | 6.81 / 2.36 mm |
| | mid | 6.26 mm | 0.69 / 0.53 / 0.20 / 0.01 / -0.08 | 0.90, 112 s | 5.94 / 1.99 mm |
| | high | 7.30 mm | 0.70 / 0.55 / 0.23 / 0.02 / -0.07 | 0.90, 124 s | 6.92 / 2.34 mm |
| `k2` (white 0.5 mm, multipath phase 0, colour 5 / 3.5 / 3.5 mm at 120 / 100 / 110 s) | tropical | 7.58 mm | 0.63 / 0.43 / 0.09 / 0.00 / -0.05 | 0.92, 80 s | 7.26 / 2.20 mm |
| | mid | 5.27 mm | 0.58 / 0.38 / 0.05 / 0.05 / -0.06 | 0.88, 71 s | 4.95 / 1.81 mm |
| | high | 5.77 mm | 0.58 / 0.37 / 0.02 / 0.09 / -0.09 | 0.90, 68 s | 5.48 / 1.81 mm |
| `k3` (white 0.5 mm, multipath phase 0, colour 4.75 / 3.4 / 3.26 mm at 181 / 137 / 181 s) | tropical | 6.99 mm | 0.67 / 0.51 / 0.17 / 0.00 / -0.06 | 0.89, 108 s | 6.58 / 2.37 mm |
| | mid | 5.08 mm | 0.61 / 0.42 / 0.08 / 0.05 / -0.07 | 0.88, 81 s | 4.77 / 1.75 mm |
| | high | 5.36 mm | 0.60 / 0.42 / 0.06 / 0.10 / -0.10 | 0.85, 85 s | 4.95 / 2.07 mm |

**K1 half holds, half fails.** Cutting the white noise alone lifts the lag-1 autocorrelation to 0.69
to 0.72 (at least 0.6, as predicted), but the fitted time constant reads 112 to 141 s, inside the
field's range rather than above 150 s: the estimator absorbs the slow part of the 300 s multipath, so
its post-fit colour is shorter than the injected one. `k1` is already within C1 in the tropical and
mid classes and misses only the high class's rms (+35 % against the 30 % bar); it has no class
structure (every class 6 to 7 mm where the field's tropics are 40 % above its other stations).
**K2 fails**: the rms is within 7 % in every class but the colour decays too fast (post-fit tau 68
to 80 s, lag-5 autocorrelation 0.02 to 0.09 against 0.21 to 0.32), for the same reason: a post-fit
time constant is about two-thirds of the injected one. The rule of section 3.1 sets `k3`: sigma by
the rms ratio, tau by the ratio of the fitted time constants (tropical 4.75 mm, 181 s; mid 3.4 mm,
137 s; high 3.26 mm, 181 s).

**`k3`** matches the rms within 3 % and the autocorrelation at 1 and 2 epochs within 0.07 in every class
and reads 0.15 low at 5 epochs in every class (C1 fails at lag 5 only): the field's colour has a heavier
tail than one exponential. Section 3.2 sets two more rungs.

**C2 on `k1`** (the program posture's block-clone instrument, `gnss_processor_wsx1.exe`, 2 h, `wsf_persist.py`,
ratio at 5 / 20 / 60 / 120 epochs): `k0` 1.09 / 1.09 / 0.84 / 0.60 (card FWS's control `wsx_ctl_s` 1.10 /
1.09 / 0.85 / 0.66), `k1` 1.60 / 1.82 / 1.49 / 1.01, the field's 2 h control 1.47 / 1.66 / 1.83 / 2.23. The
field's colour alone brings the field's short-lag persistence to the sim (C2's direction), and it reverts
by 120 epochs (C3's), as card FWS's 30 mm sinusoid arm did.

### 4.1 `k4`, `k5` and the elevation shape (2026-10-01)

| candidate | class | rms | acf 1 / 2 / 5 / 20 / 60 | GM share, tau |
| --- | --- | --- | --- | --- |
| `k4` | tropical | 7.03 mm | 0.68 / 0.53 / 0.19 / -0.00 / -0.06 | 0.88, 117 s |
| | mid | 5.04 mm | 0.62 / 0.44 / 0.11 / 0.05 / -0.07 | 0.88, 87 s |
| | high | 5.28 mm | 0.60 / 0.43 / 0.08 / 0.11 / -0.10 | 0.83, 92 s |
| `k5` | tropical | 7.29 mm | 0.68 / 0.52 / 0.18 / -0.02 / -0.04 | 0.90, 109 s |
| | mid | 5.47 mm | 0.65 / 0.47 / 0.13 / 0.03 / -0.08 | 0.91, 90 s |
| | high | 6.00 mm | 0.65 / 0.49 / 0.14 / 0.06 / -0.08 | 0.87, 104 s |

**K4 fails**: lengthening the time constant moves the post-fit acf(5) by 0.02 to 0.03, not 0.08; the
estimator absorbs the slow part of a band-common error. **K5 fails**: it meets C1 in the mid (lag 5
at exactly -0.10) and high classes and misses the tropical lag 5 by 0.14. Across the ladder the shape
is best in `k1` (lag 5 within 0.04 in every class), whose colour is the sim's existing multipath
(per band, 300 s, its amplitude capped at 20 degrees of elevation), and worst wherever the new block
carries the colour with the white noise's elevation shape (1 / sin e down to 5 degrees, 5.8 times at
the 10 degree mask): the large low-elevation part of the block's error is what the estimator absorbs
and what shortens the post-fit colour. That is the review's caution (section 2.1) measured.

**`k6`, predicted before it ran:** the block with the multipath's elevation shape and time constant
(`phase_colour.elev_factor_floor_deg` 20, tau 300 s), the multipath phase off, the white noise at
0.5 mm, and sigma per class from `k1`'s rms ratios (the IF equivalent of 2 mm per band is about 6 mm
band-common): tropical 6.0 mm, mid 4.9 mm, high 4.4 mm. **K6:** it meets C1 in every class.

The recipe is the candidate that meets C1 in the most classes over the whole ladder, ties to the
smaller summed lag-5 miss (section 3.2's rule, extended from `k4` and `k5` to every candidate):
today `k1` and `k5` tie at two classes and `k1` has the smaller lag-5 miss (0.09 against 0.31).

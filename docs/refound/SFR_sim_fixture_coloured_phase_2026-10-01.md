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

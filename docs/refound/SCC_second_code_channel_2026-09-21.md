# SCC: the second code channel (a station contributes every allow-listed band-1 code it tracks)

**STATUS**

- **id:** SCC
- **topic:** code-bias
- **status:** open
- **summary:** Pre-registered 2026-09-21 (approved by the author the same day). Build card, default OFF: a network station contributes, as code-only rows, every allow-listed band-1 code it reports besides its latched canonical code, so that the C1C and C1W satellite code-bias families stop competing for the same 59 stations. One new unknown per station (the receiver's band-1 inter-signal bias); no second phase, ambiguity or phase-bias family; no new datum row. Requirements, gates and bars are frozen below; nothing is built.
- **source:** [CSL](CSL_canonical_signal_latch_2026-09-20.md) section 9 (the code half of the latch rule is not the lever: one code per station and band makes the two families share the stations); [MSC](MSC_multi_site_consumer_2026-09-21.md) sections 4 and 5 (the thinned C1W biases and the consumer that leaves by tens of metres on a dropout)
- **knobs / posture:** `network.second_code_channel` (bool, default false), `network.second_code_signals` (list, default `["C1C", "C1W"]`), `network.second_code_sigma_scale` (number, at least 1, default 1.0); closed-loop consumer pinned off
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** baselines `runs/_rap_r6_dual` (field), `runs/_rf0hv5_sim6` (sim); census replays `runs/_rap_msc_<site>_r6_d`

## 1. The problem and the design read

One canonical code per station and band means a satellite's C1W code bias is supported
only by the stations that latched C1W. Under the armed `priority` latch that is 19 of 60
(the wire carries a C1W bias for a median of 24 satellites per epoch against 31 for C1C,
5323 satellite-epochs lack it); with `canonical_signal_code_follows_phase: false` C1W is
complete and C1C thins from 31 to 28 (card CSL section 9). A consumer whose code bias
vanishes leaves by tens of metres (card MSC).

A read-only design map (Opus, 2026-09-21; the two load-bearing claims re-read by the
manager in source) established:

1. **The seam.** `PppModel::selectUncombinedSignals` (`src/core/model/PppModel.cpp`)
   emits one code and one phase per band from a per-band container; everything downstream
   of `selected_signals` is per signal (row assembly, satellite code-bias allocation and
   column registration at two or more stations, scatter, emission). The companion's raw
   value survives to that point. The existing `channel_mode: all` widening adds the phase
   too and every tracking attribute, so it is not reused.
2. **The receiver side needs one new state.** `StateInitializer::resolveReceiverBias` keys
   the receiver code bias on the band digit: C1C and C1W both map to `GPS_DCB` with the
   same coefficient. A second band-1 code at the same station would therefore assert a
   zero receiver inter-signal bias and push it into the satellite column. (Card B5D is the
   record of what an asserted zero inter-signal bias costs.)
3. **The satellite-side datum needs nothing.** In `NetworkSbasis::buildCodeBiasDatumRows` a
   same-band companion of the pin's first signal is already free (counter
   `n_sat_gf_sameband_free`), the C1C column is pinned absolutely (IF pin and GF pin), and
   the C1W family's level is held by its own time-linked sum-to-zero row. The one null
   direction of the new model (all receiver inter-signal biases against all satellite
   ones) is closed by that row.
4. **One detector guard is mandatory.** The Melbourne-Wubbena detector holds one code per
   band from `obs.observed` and breaks ties by attribute priority (`W` before `C` on GPS
   L1), so an unguarded companion would silently move the detector's code from C1C to C1W
   at 22 stations.
5. **The beds.** Field roster: 41 stations report both C1C and C1W, 18 only C1C, none only
   C1W (20 also carry C1X, 19 C1L). Every sim file has one band-1 code, so the knob is
   inert on the sim as delivered.

## 2. Requirements (frozen)

- **R1.** `network.second_code_channel: true`: in network mode, for every band-1 code
  signal in `network.second_code_signals` that a station reports for a satellite and that
  is not the code already selected for that band, one code-only row is added. No phase
  row, no ambiguity, no phase-bias column. The list is explicit; the loader refuses an
  entry that is not a code type and, in this card, an entry that is not on band 1 (band 2
  would wake the untested bridging path of `network.sat_isc_state`).
- **R2.** Receiver side: a band-1 code that is not the station's LATCHED canonical code is
  modelled with a per-station receiver inter-signal-bias state (key `<system>_ISC_<signal>`,
  unit coefficient, stored with the receiver code-bias states), whether the row entered as
  a companion or as the per-satellite fallback for a missing canonical code. The key must
  not match the `_IFB`, `_DCB` or per-slot parsers (the station split drops `_IFB`
  columns). It carries no zero-mean prior. It is restored by the receiver code-bias carry;
  the loader refuses the knob without `network.carry_rx_code_bias`.
- **R3.** Satellite side: no change to the datum rows.
- **R4.** Detectors unchanged: the Melbourne-Wubbena detector ignores rows added by R1 (a
  fallback code stays eligible exactly as today).
- **R5.** `network.second_code_sigma_scale` (at least 1, default 1.0) multiplies the code
  sigma of the rows that R2 routes through the inter-signal state.
- **R6.** Interlocks: refused with `processing.channel_mode: all`; pinned off in the
  closed-loop consumer's derived config; appended to the warm-resume fingerprint only when
  armed.
- **R7.** Telemetry, both edges (invariant 7): run-end counters for companion rows, cells,
  cell openings and closings, receiver inter-signal states alive, their reseeds, fallback
  rows routed by R2, and satellite-signal columns that reached two stations only through
  companions; event rows for a cell opening and closing; an armed run with zero companion
  rows warns at run end (invariant 4).
- **R8.** Default OFF with byte identity (invariant 6). Pure kernels (companion selection,
  key make and parse with the negative parser pins, cell edges) in a new suite owned by
  one new test file.
- **R9.** Tool: `scripts/rapid_arm.py` gains a `clone` op (`from=`, `to=`, `noise=`
  metres, seeded) that adds an observation type as a noisy copy, and a helper that prints
  the constants the `bias` op draws, so that the sim bed can carry a second band-1 code
  with known receiver and satellite inter-signal biases.

## 3. Gates and bars (frozen before the build)

Exe: a renamed Release copy. Arms from the baselines' own `arm.json`, 6 h.

- **G0 identity.** Knob absent: field and sim `products.txt` (lines not starting with `#`),
  `closed_loop_trace.csv` and `network_epoch_status.csv` identical to `runs/_rap_r6_dual`
  and `runs/_rf0hv5_sim6`. Knob ON on the unmodified sim: the same three files identical
  (no station has a second code) and the run-end warning present.
- **G1 mechanism, field ON.** (a) Median satellites per epoch with a C1W bias on the wire
  at least 30, and at most 100 satellite-epochs with a C1C bias and no C1W bias (baseline
  24 and 5323). (b) Satellite-epochs with a C1C bias not below the baseline's. (c) Every
  R7 counter nonzero where the data demand it; receiver inter-signal states at the
  stations that report both codes. (d) No new rank or positive-definiteness complaint and
  no increase of station rejections against the baseline. (e) The set of `T0_mw_detect`
  event rows (time, satellite, station) identical to the baseline's.
- **G2 truth, sim with a cloned C1W** (noise 0.3 m, receiver constants sigma 0.5 m,
  satellite constants sigma 0.5 m). Knob OFF on the cloned files: the three files of G0
  identical to the sim baseline. Knob ON: the wire's C1W minus C1C bias per satellite
  against the injected satellite constant, both with the across-satellite mean removed,
  one common sign fitted: correlation magnitude at least 0.98 and residual rms at most
  0.10 m over hours 3 to 6; the consumer within 0.03 m of the sim baseline in every hour
  from hour 2.
- **B1 closed loop at WTZZ00DEU** against 0.48 0.05 0.05 0.08 0.09 0.10 m: every hourly
  p50 within 0.03 m or better; mean 3D over hours 3 to 6 not worse by more than 0.010 m;
  clock wire at most 0.0117 (tracked) and 0.0194 (all). Epochs below a decimetre are
  reported and are not a bar: the site carries a standing Up offset of 0.08 to 0.10 m, so
  the count switches on changes of a centimetre (card EV7 section 1.5).
- **B2 census, 16 sites,** dual consumers replayed on the ON wire against the `r6`
  replays: mean hourly p50 over hours 2 to 6 better (by more than 0.01 m) at more sites
  than worse, median change at most 0; hour 1 median change at most +0.05 m; no
  code-bias freshness flip at any site; no epoch above 5 m at QUIN00USA.
- **B3** only if B1 and B2 pass: the same reading at 12 h against a 12 h baseline from the
  same exe.
- Arms: scale 1, 3 and 10. The scale is chosen by B1 and B2 together; a tie goes to 1.

**Reading.** G0, G1 and G2 are build gates: a failure is a defect to fix, not a result.
B1 and B2 decide the recommendation; adoption into the working bases is the author's
decision. WTZZ00DEU alone decides nothing (card MSC).

## 4. Deferrals, triaged (invariant 3)

- Band 2 companions (28 stations carry C2X, 25 C2L): refused by the loader in this card;
  field-inert by construction. The follow-up would make the `sat_isc_state` bridging
  roster non-empty for the first time.
- C1X and C1L: admitted by the loader only if listed, never by default; unvalidated.
- A second phase signal (the exact-integer slip witness) is a different card.
- Warm resume: fingerprinted when armed; no warm lineage exists on this posture
  (field-inert today).
- Card EV7 is not expected to be cured by this card: on the arm where the C1W code column
  is already supported by many stations, the episode of TOW 278520 is as large.

## 5. Invariants pre-flight

1: the new receiver state is time-linked through the existing receiver code-bias carry;
its reseed is counted (R7) and its first sighting is loose (the initial code-bias sigma),
so a fresh state determines itself before it informs anything else. 4: counters and the
zero-row warning. 5: the wire is graded by coverage counts and by the de-meaned truth
comparison, not by raw steps. 6: G0. 7: cell and column edges are events (R7); the column
edge already has a TOPOLOGY row.

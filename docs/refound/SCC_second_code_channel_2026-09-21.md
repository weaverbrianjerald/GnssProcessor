# SCC: the second code channel (a station contributes every allow-listed band-1 code it tracks)

**STATUS**

- **id:** SCC
- **topic:** code-bias
- **status:** open
- **summary:** RE-GRADED 2026-09-21 on the EV7-fixed base: a trade, NOT armed, the author's decision. With the EV7 fix ON, the second code channel takes the 16 census sites' hour 1 down by 0.08 m (scale 1) or 0.14 m (scale 10), removes QUIN00USA's excursion entirely (240 epochs above 5 m after hour 2 to 0) and 46 of 50 code-bias dropouts, and is better than the fix alone on hours 2 to 6 at 6 sites against 3 or 4; it costs WTZZ00DEU, a C1W consumer, 0.02 m from hour 3 (its standing Up offset), which fails B1, and is slightly worse than the fix alone in the last half hour at 10 or 11 sites. Earlier summary: BUILT 2026-09-21 (main `61d67c2`), default OFF, NOT armed. The mechanism works: with the knob ON the wire carries a C1W code bias for a median of 31 satellites per epoch (24 before), 2 satellite-epochs lack it (5210 before), C1C is not reduced, the detector rows are identical, and the code-bias dropouts at the 16 census sites fall from 50 to 4. It fails the WTZZ00DEU bars at every scale (a C1W consumer whose standing Up offset grows by 1 to 3 cm), and at scale 1 it amplifies the episode of card EV7. The sim truth gate fails as frozen (0.275 m rms), and the diagnosis puts the error on the PINNED C1C column: the sim network carries a per-satellite code misfit of decimetres on its pinned signals that never relaxes, while the free C1W column sits where the data put it. Re-grade on the EV7-fixed base is owed. Original summary: Pre-registered 2026-09-21 (approved by the author the same day). Build card, default OFF: a network station contributes, as code-only rows, every allow-listed band-1 code it reports besides its latched canonical code, so that the C1C and C1W satellite code-bias families stop competing for the same 59 stations. One new unknown per station (the receiver's band-1 inter-signal bias); no second phase, ambiguity or phase-bias family; no new datum row. Requirements, gates and bars are frozen below; nothing is built.
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

## 6. Result (2026-09-21; Opus build worker, the numbers below re-graded by the manager from `runs/`)

### 6.1 The build

Main `61d67c2`. New: `include/filter/NetworkSecondCodeScope.hpp`,
`src/core/filter/NetworkSecondCodeScope.cpp`, `tests/test_network_second_code_scope.cpp`
(suite `NetworkSecondCodeScope`, 29 cases). Sixteen files modified (939 lines added).
`gnss_tests`: 2408 ran, 2407 passed, 1 known skip. Exe `gnss_processor_scc.exe`, sha256
`5DF14002...`. `config_posture_check.py --verify-loader`: 0 coverage gaps; golden verify
PASS; `rapid_arm.py selftest` 47 of 47 (the `clone` op and the `draws` helper of R9).

Two corrections to the design read of section 1. The band map is keyed by band name, so the
latch accessor keys on the RINEX band digit. Under an active latch there is no
per-satellite fallback code (the cached branch returns before the fallback walk), so the
fallback clause of R2 only covers a constellation with no latch; measured fallback rows: 0.

### 6.2 G0 and G1: PASS

G0: the three files of `s0_field`, `s0_sim` and `s0_sim_on` (knob ON on the unmodified sim)
are identical to the two baselines, and the armed sim run prints the zero-row warning.

| G1, field ON (identical for the three scales) | baseline | ON | bar |
| --- | --- | --- | --- |
| satellites per epoch with a C1W bias, median | 24 | 31 | at least 30 |
| satellite-epochs with a C1C bias and no C1W bias | 5210 | 2 | at most 100 |
| satellite-epochs with a C1C bias | 22473 | 22586 | not below |
| receiver inter-signal states at peak | | 41 | the 41 stations that report both codes |
| companion rows folded; cell openings; closings; reseeds | | 241076; 884; 554; 0 | nonzero where the data demand it |
| column-epochs that reach two stations only through companions | | 5434 | |
| rank and positive-definiteness complaints; station rejections | 47; 0 | 47, 47, 43; 0 | no new kind, no increase |
| `T0_mw_detect` rows, symmetric difference against the baseline | 200 rows | 0 | identical |

### 6.3 G2: FAIL as frozen, and what the failure is

Arms `s2_clone_off` and `s2_clone_on`: the sim with C1W cloned from C1C (noise 0.3 m) and
injected station and satellite constants (sigma 0.5 m each).

- Knob OFF on the cloned files is NOT identical to the sim baseline (clock p50 7.9e-5 m,
  maximum 1.5e-3 m, from the first epoch). A rewrite control without the clone is
  identical, and a different noise seed gives a third wire: some path upstream of this
  card reads a second band-1 code that is present in the file. The latch stays on C1C and
  no C1W record is emitted. The consumer of that observable is not named yet (open).
- Knob ON: the wire's C1W minus C1C bias against the injected satellite constant, both
  de-meaned, sign +1: correlation 0.913 (bar 0.98), residual rms 0.275 m (bar 0.10), over
  32 satellites. The error is static: 0.220 m in hour 1, then 0.279, 0.278, 0.276, 0.275
  and 0.274 m, with a formal sigma of 0.005 m on the column. Worst: G16 -0.64, G09 -0.55,
  G03 +0.54, G18 -0.52, G06 -0.45 m.
- The manager's read of `network_pf_rows.csv` (hours 3 to 6, 176 069 code rows per
  signal): the per-satellite MEAN post-fit residual of the pinned C1C rows has a spread of
  0.285 m across satellites and correlates +0.977 with the truth error; the C2W rows carry
  the opposite sign (0.183 m, correlation -0.955); the free C1W rows have a spread of
  0.038 m (correlation -0.03). With the C1C mean residual removed, the truth error is
  0.061 m rms.

So the free column sits where the data put it, and the pinned pair does not. The sim
network carries a per-satellite code misfit of decimetres on its pinned signals (about
5500 code rows per satellite, so the misfit is significant at tens of sigma) and never
relaxes it: the levels are born early and the carried states are stiffer than their true
accuracy. That is a finding about the datum and the born levels (cards RF-0c and RF-IGN),
not a defect of this build, and it is measurable on any run as the per-satellite mean
post-fit code residual. A C1C consumer sees that misfit in its code rows; a C1W consumer
under this card does not. The frozen bar stays failed; 0.061 m is a post-hoc statistic.
The consumer sub-bar reads 0.036 m at hour 3 against 0.03, in the better direction.

### 6.4 B1 and B2

| arm | 3D p50 by hour (m) | mean 3D, hours 3 to 6 (m) | below 0.1 m | clock wire, all / tracked (m) | refloats | B1 |
| --- | --- | --- | --- | --- | --- | --- |
| `r6_dual` | 0.48 0.05 0.05 0.08 0.09 0.10 | 0.0818 | 533 | 0.0184 / 0.0107 | 183 | |
| `s1_on` | 0.40 0.06 0.05 0.09 0.10 0.14 | 0.0986 | 420 | 0.0232 / 0.0095 | 255 | FAIL (hour 6 by 0.032; mean by 0.017; wire all) |
| `s3_on` | 0.51 0.07 0.07 0.12 0.12 0.14 | 0.1154 | 224 | 0.0185 / 0.0110 | 200 | FAIL (hours 4 to 6; mean by 0.034) |
| `s10_on` | 0.52 0.07 0.07 0.12 0.12 0.14 | 0.1164 | 217 | 0.0182 / 0.0110 | 196 | FAIL (hours 4 to 6; mean by 0.035) |

WTZZ00DEU is a C1W and L1W consumer (`closed_loop_sat_trace.csv`). Its error is its
standing Up offset, which goes from -0.078, -0.084, -0.098 m (hours 4 to 6) to -0.088,
-0.097, -0.132 m at scale 1 and -0.112, -0.119, -0.135 m at scale 10: the C1W biases it
reads have changed by decimetres and its float levels follow.

| census key | median p50 by hour (m) | mean hourly p50, hours 2 to 6: better / worse, median change | hour 1 median change (m) | code-bias dropouts (sites) | QUIN00USA epochs above 5 m |
| --- | --- | --- | --- | --- | --- |
| `r6` | 0.827 0.282 0.101 0.104 0.113 0.169 | | | 50 (7) | 401 |
| `scc1` | 0.804 0.203 0.096 0.123 0.109 0.307 | 5 / 9, +0.016 m | -0.077 | 4 (2) | 383 |
| `scc3` | 0.681 0.214 0.097 0.125 0.108 0.190 | 8 / 3, -0.011 m | -0.223 | 4 (2) | 382 |
| `scc10` | 0.687 0.230 0.086 0.091 0.112 0.178 | 9 / 2, -0.013 m | -0.138 | 4 (2) | 339 |

B2: scale 1 fails the paired bar; scales 3 and 10 pass it and the hour 1 bar. Every scale
fails the two absolute sub-bars. The dropouts fall from 50 to 4 (DAV100ATA 2, ARHT00ATA 2),
which is the mechanism this card was built for. The QUIN00USA sub-bar rested on a wrong
premise: card EV7 section 5.5 shows that its excursion follows a clock-anchor declaration,
not a missing code bias. Hour 6 is the hour of the EV7 episode; at scale 1 its census
median goes from 0.169 to 0.307 m and the run has 255 refloats against 183, so companion
rows at full weight amplify that episode.

### 6.5 Reading

Not armed. G0 and G1 pass. G2 fails as frozen, and the diagnosis clears the build and
names a standing misfit of the pinned code datum instead. B1 fails at every scale. B2
passes its paired bars at scales 3 and 10 only. Because hour 6 of every arm contains the
EV7 defect and scale 1 amplifies it, the card is graded again on the EV7-fixed base (arms
`fx_s1` and `fx_s10` of card EV7's build) before any recommendation.

### 6.6 What stays open

- The OFF-path sensitivity to a second band-1 code in the file.
- The pinned pair's per-satellite code misfit (0.285 m on C1C, 0.183 m on C2W on the sim),
  its size on the field, and its relation to the consumers' standing offsets.
- The armed fingerprint append is not exercised by any run (no snapshot is written on this
  posture).
- Band 2 companions (section 4).

## 7. Re-grade on the EV7-fixed base (2026-09-21; arms of card EV7's build, exe `gnss_processor_ev7.exe`, manager re-graded)

All arms carry `network.phase_datum_gate_drop_erase: true`. Census keys: `ev7fix` (the fix
alone), `fxs1`, `fxs10`.

| arm | 3D p50 by hour at WTZZ00DEU (m) | mean 3D, hours 3 to 6 (m) | below 0.1 m | clock wire, all / tracked (m) | refloats |
| --- | --- | --- | --- | --- | --- |
| `g1_field_on` (fix alone) | 0.48 0.05 0.06 0.09 0.09 0.09 | 0.0815 | 577 | 0.0091 / 0.0072 | 69 |
| `fx_s1` (scale 1) | 0.40 0.07 0.07 0.11 0.11 0.10 | 0.1011 | 316 | 0.0092 / 0.0067 | 85 |
| `fx_s10` (scale 10) | 0.52 0.07 0.07 0.11 0.11 0.11 | 0.1042 | 260 | 0.0098 / 0.0069 | 90 |

| census key | median p50 by hour (m) | median count below 0.1 m | hours 2 to 6 against `ev7fix`: better / worse, median | hour 1 against `ev7fix` | last half hour against `ev7fix` | QUIN00USA epochs above 5 m after hour 2 | code-bias dropouts |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `r6` (old base) | 0.827 0.282 0.101 0.104 0.113 0.169 | 206.5 | | | | 380 | 50 |
| `ev7fix` | 0.827 0.273 0.096 0.093 0.098 0.096 | 235 | | | | 240 | 50 |
| `fxs1` | 0.804 0.237 0.107 0.119 0.095 0.078 | 245 | 6 / 3, -0.007 m | 9 / 4, -0.077 m | 6 / 10, +0.0071 m | 0 | 4 |
| `fxs10` | 0.687 0.232 0.101 0.091 0.097 0.075 | 287.5 | 6 / 4, -0.006 m | 10 / 4, -0.138 m | 5 / 11, +0.0025 m | 0 | 4 |

- The amplification of the EV7 episode is gone with the fix (section 6.4 attributed the
  hour 6 loss at scale 1 to it): the clock wire and the trips of the two arms equal the
  fix alone.
- B1 still fails: WTZZ00DEU is 0.02 m worse from hour 3 at both scales (mean 3D over hours
  3 to 6 0.1011 and 0.1042 m against 0.0815 m; hour 4 misses the 0.03 m window against the
  old baseline by 0.004 m at scale 1). The whole difference is the site's standing Up
  offset (card EV7 section 7.7).
- Across the census the channel is a gain on the fixed base at hour 1 and on the two
  absolute measures (no consumer leaves by metres any more; 46 of 50 dropouts gone), a
  small gain on hours 2 to 6, and a small loss in the last half hour at more sites than
  it helps. Card EV7's `row_kinds` proxy (rows that survive their column) falls from 13
  edges to 1 with this card armed while the 13 phase drop edges stay: the code columns no
  longer hang on one station.

**Reading.** By the frozen rule B1 fails, so the card is not recommended as it stands. The
evidence for it is nevertheless stronger than on the old base, and the one site that pays
is the site whose error is an unexplained standing offset. The decision is the author's;
scale 10 is the candidate if it is armed (best hour 1, best median count, no excursion).

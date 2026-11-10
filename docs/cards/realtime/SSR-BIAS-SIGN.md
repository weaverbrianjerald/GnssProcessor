# SSR-BIAS-SIGN: SSR code and phase biases applied with the sign of a Bias-SINEX OSB

**STATUS**

- **id:** SSR-BIAS-SIGN
- **topic:** realtime
- **status:** open
- **summary:** The SSR decoder stored the RTCM / IGS-SSR wire value of every code and phase bias unchanged, while the store and the model treat a stored bias as the bias present in the observation (the OSB sense, `kSatBiasSign` +1). The wire value is the correction added to the observation, the negative. Every streamed bias was therefore applied twice over with the wrong sign. FIXED in the decoder (both bias bodies negate), unit-tested. Live after the fix (2026-10-29, 30 min, CAS IGS-SSR, static): the same-band code misfit C1C minus C1W fell from 0.58 m to 0.05 m (median over pairs) and the float wide-lane became integer (median |fraction| GPS 0.326 to 0.036, Galileo 0.219 to 0.062); the narrow-lane did not (ionosphere-free NL fractions near uniform), so AR still fixed nothing (1,639 searches, ratio at most 2.58). Open: the narrow-lane.
- **dates:** 2026-10-29
- **knobs / posture:** none for the fix. The live sessions use `estimation.ambiguity_resolution.mode` 1 and `model_corrections.bias_coverage_requires_apply` true (card SSR-SIG-ID section 5).
- **code:** `src/core/rtcm/RtcmSsrDecoder.cpp` (`decodeCodeBiasBody`, `decodePhaseBiasBody`), `include/rtcm/RtcmTypes.hpp` (`SsrCodeBias`, `SsrPhaseBiasEntry::bias_m`), `include/measurement/ObservationEquation.hpp` (`kSatBiasSign` documentation), `tests/test_rtcm_ssr_phase_bias.cpp` (`RtcmSsrSignalIdTest.WireBiasIsTheNegativeOfTheStoredBias`, the negated expectations)
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\live_ge_static_ar` (before, exe `gnss_processor_ssrsig.exe`, sha256[:16] dc447feab9d92b7e); `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\{smoke_sign,live_ge_static_ar_sign}` (after, exe `gnss_processor_ssrsign_wt.exe`, sha256[:16] 9cfeeba869cf3ba2, built in the worktree `D:\GnssProcessorSupport\worktrees\ssr-bias-sign` from 1527cf9 plus this change only)
- **reference:** cssrlib `pppssr.py` (model `range - cbias` and `range - pbias` with the wire values for `RTCM3_SSR` and `IGS_SSR`; Bias-SINEX OSBs negated, "IGS uses sign convention different with RTCM"); `rtcm.py` `decode_cssr_cbias` / `decode_cssr_pbias` keep the wire value
- **source:** card [SSR-SIG-ID](SSR-SIGNAL-ID.md) section 7 (0 fixes, float fractions uniform)

**Outcome.** Why did the live float ambiguities carry no integer information once every bias reached its own signal? The streamed biases were applied with the wrong sign: the decoder kept the wire value, a correction added to the observation, where the store expects the bias present in it. A same-band code test showed the misfit (C1C minus C1W between satellites, 0.25 to 1.12 m and constant), and it collapsed to 0.05 m after the decoder negated the biases. The wide-lane became integer; the ionosphere-free narrow-lane did not, so the slant ionosphere is not the binding limit at 30 minutes and AR still fixed nothing. Open: the narrow-lane (section 6).

> Results are appended TO THIS FILE, below the text.

---

## 1. Why the floats were biased, not imprecise

In the session of card SSR-SIG-ID section 7 the filter claimed a formal L1 ambiguity sigma of 0.047
cycles (GPS) and 0.017 cycles (Galileo) at 20 to 30 minutes, while the L1, L2 and wide-lane fractions
were uniform (median |fraction| 0.22 to 0.37 against 0.25 for uniform). The best MLAMBDA candidate's
squared norm reached 412,911 on 26 ambiguities at a ratio of 1.0. An imprecise float under an honest
covariance gives uniform fractions with a large sigma and a norm near the number of ambiguities; a small
sigma with uniform fractions and a huge norm is a float that the model has pushed off every integer.

## 2. The convention

RTCM and IGS-SSR send each satellite code and phase bias as the correction added to the observation:
cssrlib forms the model as `range - cbias` and `range - pbias` with the wire values of both dialects and
negates Bias-SINEX OSBs instead. The project's store holds the bias present in the observation:
`SsrCodeBias` and `SsrPhaseBiasEntry` say "subtract from the observation", the model adds the stored
value to the clean range (`addSsrBias`, `kSatBiasSign` +1), and the network emitter writes its estimated
states in that sense. The decoder copied the wire value into the store, so a decoded bias b entered as
+b where -b belongs: every row carried twice the bias. The closed loop (emitter to store, no wire) was
consistent and never exercised the decoder's sign; the `kSatBiasSign` documentation said the convention
was to be confirmed against a provider's stream.

## 3. The same-band code test

C1C and C1W share a frequency, so the geometry, clocks, orbit, troposphere and ionosphere are common, the
receiver code biases cancel between satellites, and no state can absorb a difference between the two
rows. The between-satellite post-fit residual difference C1C minus C1W, mean per pair after 600 s,
isolates the applied code biases:

| session | pairs | median of the per-pair \|mean\| | largest | per-pair standard deviation |
| --- | --- | --- | --- | --- |
| `live_ge_static_ar` (before) | 9 | 0.578 m | 1.120 m | 0.006 to 0.071 m |
| `smoke_sign` (after, 120 s) | 9 | 0.036 m | 0.105 m | 0.003 to 0.021 m |
| `live_ge_static_ar_sign` (after, 30 min) | 8 | 0.053 m | 0.103 m | |

The sessions are at different hours and on different satellites; the effect is a factor of ten.

## 4. Fix and test

`decodeCodeBiasBody` and `decodePhaseBiasBody` store the negative of the wire value; `RtcmTypes.hpp` and
the `kSatBiasSign` documentation state both senses. `RtcmSsrSignalIdTest.WireBiasIsTheNegativeOfTheStoredBias`
pins a wire +2.50 m code bias and a wire +0.1234 m phase bias as stored -2.50 m and -0.1234 m; the decode
cases of cards SSR-PB-HDR and SSR-SIG-ID expect the negated values. Unit suite on 1527cf9 plus this change
(Debug): 3,632 cases, 3,631 passed, 1 fixture-gated skip, 0 failed; the 30 bias and observation-equation
cases also pass in the worktree's Release test build.

## 5. Live session after the fix

`live_ge_static_ar_sign`: 1,800 epochs from 00:23 to 00:53 GPS time on 2026-10-30, exit 0; phase biases for
every modelled satellite but one in each epoch, the one the stream also sends no clock for (32,926 of
34,726 satellite-epochs), 0 phase-bias resets.

| median \|fraction\|, 20 to 30 min (uniform 0.25) | before | after |
| --- | --- | --- |
| GPS wide-lane L1C minus L2W | 0.326 | 0.036 |
| Galileo wide-lane L1C minus L5Q | 0.219 | 0.062 |
| GPS ionosphere-free narrow-lane (wide-lane rounded) | 0.231 | 0.254 |
| Galileo ionosphere-free narrow-lane (wide-lane rounded) | 0.070 (about 5 satellites) | 0.303 |
| GPS uncombined L1 | 0.271 | 0.206 |

MLAMBDA: 1,639 searches, ratio median 1.23, 90th percentile 1.97, maximum 2.58 (before: 1.01, 1.02, 1.10),
0 fixes. Best-candidate norm 2,293 at the first search, 29,244 at the last (before: 11,687 and 412,911).
3D position error against the surveyed coordinate (`config/sbf_replay_covl145.json`), median over 1,200 to
1,800 s: 0.228 m, formal 3D sigma 0.006 m.

The wide-lane is the Melbourne-Wübbena-type combination of the streamed code and phase biases; its integer
fractions confirm the corrected sign for both. The narrow-lane formed from the ionosphere-free combination
of the floats is free of any slant-ionosphere error and is still near uniform at 20 to 30 minutes (0.125 in
the last 100 s for both systems, too short to call). The ionosphere is therefore not the binding limit;
the narrow-lane needs the non-dispersive part of the float (position, troposphere, clocks, orbit, antenna
models, wind-up) within about a quarter of its 0.107 m wavelength, and the float position sits 0.21 to
0.29 m from the surveyed coordinate in three sessions (median 3D over the last 10 minutes; at the last
epoch 0.13 to 0.20 m west and 0.13 to 0.18 m north in each), under a formal sigma of a few millimetres.

## 6. Open

- The narrow-lane: a longer live session (60 to 90 minutes) to separate slow convergence from a constant
  error; the 0.2 m offset in the same direction across sessions points to a constant one. To check: the
  surveyed coordinate (frame, epoch, ARP or marker) and the receiver antenna override `LEIAR25 NONE`
  against the mounted antenna and radome.
- The float covariance claims millimetres against a decimetre error (position) and 0.05 cycles against
  uniform narrow-lane fractions; the ratio test and the success-rate gate read that covariance.
- Wide-lane-only fixing (`estimation.ambiguity_resolution.wl_only`) is now possible in principle; not run.
- The orbit reference point is set by message format in the decoder (legacy APC, IGS-SSR CoM), while the
  caster's sourcetable carries both points in both formats (SSRA00BKG1 is IGS-SSR APC, SSRC00BKG0 is
  RTCM-SSR CoM). The stream used here, SSRC01CAS1, is IGS-SSR CoM and is treated correctly; a stream of the
  other combinations would get the wrong point. cssrlib treats both formats as referring to the L1 APC.
- The phase wind-up accumulator is seeded with the absolute angle and unwrapped (checked); not a candidate.

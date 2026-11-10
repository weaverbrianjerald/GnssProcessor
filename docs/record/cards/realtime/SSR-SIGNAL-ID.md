# SSR-SIG-ID: SSR bias signal ids read through the MSM observation table

**STATUS**

- **id:** SSR-SIG-ID
- **topic:** realtime
- **status:** closed
- **summary:** The SSR code- and phase-bias decoders (legacy 1059 family and 1265 to 1270, IGS-SSR 4076 IM x5 and x6) translated each bias's signal id through the MSM observation table instead of the SSR table, so only Galileo E1C landed on its own signal: GPS L1C and L2W biases were dropped, the GPS L1W bias was filed under L1C, Galileo E5a, E5b and E6 biases landed on unused names. Found by the first live AR smoke after card SSR-PB-HDR (AR candidates: Galileo L1C only). FIXED in the decoder and unit-tested. Live after the fix: AR candidates on GPS L1C and L2W and Galileo L1C, L5Q and L7Q, 0 code-bias lookups missing; 30-minute AR session 0 fixes in 1,645 searches (ratio at most 1.10), float DD fractions indistinguishable from uniform, cause open (OPEN_ITEMS).
- **dates:** 2026-10-29 (fix, unit tests, live smoke and AR session)
- **knobs / posture:** none for the fix. The live AR configuration adds `estimation.ambiguity_resolution.mode` 1 and `model_corrections.bias_coverage_requires_apply` true (section 5).
- **code:** `include/rtcm/RtcmSignalMap.hpp`, `src/core/rtcm/RtcmSignalMap.cpp` (`ssrToRinexSuffix`, `ssrFromRinexSuffix`; the MSM functions renamed `msmToRinexSuffix`, `msmFromRinexSuffix`), `src/core/rtcm/RtcmSsrDecoder.cpp` (`decodeCodeBiasBody`, `decodePhaseBiasBody`), `tests/test_rtcm.cpp` (`RtcmSignalMapTest.Ssr*`), `tests/test_rtcm_ssr_phase_bias.cpp` (`RtcmSsrSignalIdTest.*`, the corrected ids of `RtcmSsrPhaseBiasTest.*`)
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\{smoke_ar,smoke_ar2}` (exe `gnss_processor_pbhdr.exe`, before this fix); `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\{smoke_ar3,live_ge_static_ar}` (after the fix, exe `gnss_processor_ssrsig.exe`, sha256[:16] dc447feab9d92b7e)
- **reference:** cssrlib `rtcm.py` `ssig2rsig` (`D:\GnssProcessorSupport\reference_software\cssrlib\src\cssrlib\rtcm.py`), called by both `decode_cssr_cbias` and `decode_cssr_pbias` for the legacy and the IGS-SSR messages
- **source:** card [SSR-PB-HDR](SSR-PB-HEADER.md) section 6 (the per-signal census) and the first live AR smoke

**Outcome.** Why did the first live AR attempt after the phase-bias header fix find candidates on Galileo L1C only? The decoders read every SSR bias signal id through the MSM table, whose numbering differs (GPS L1C is SSR id 0 and reserved in MSM; SSR id 2 is GPS L1W and MSM id 2 is L1C). Only Galileo E1C, id 2 in both, reached its own signal. FIXED: a separate SSR table from cssrlib, the MSM functions renamed, unit cases for both bias bodies. Live after the fix, AR forms candidates on GPS L1C and L2W and Galileo L1C, L5Q and L7Q, and no code-bias lookup misses. A 30-minute AR session still fixed nothing (1,645 searches, ratio at most 1.10, floats with no integer information), so integer fixing on this stream remains open (section 7).

> Results are appended TO THIS FILE, below the text.

---

## 1. Finding

The first live AR smoke after card SSR-PB-HDR (`smoke_ar`, 60 s, `ambiguity_resolution.mode` 1) logged
"No DD ambiguity pairs available for AR" at every epoch after the 10-epoch guard. With SSR phase biases
counted as AR coverage (`smoke_ar2`, section 5), candidate pairs formed on Galileo L1C only: four pairs on
five satellites, no GPS signal and no other Galileo signal. The engine's bias attribute census of the
30-minute session of SSR-PB-HDR section 6 named a stable set of tracked signals with no bias of their own
while a same-band sibling had one (E07, E29, E33 L6C; GPS L2L and L2W).

## 2. Mechanism

`decodeCodeBiasBody` and `decodePhaseBiasBody` translated the 5-bit signal id with
`RtcmSignalMap::toRinexSuffix`, the MSM cell-mask table. SSR bias messages carry a separate signal and
tracking mode identifier. For the signals of the GPS + Galileo dual-frequency receiver:

| stream signal (SSR id) | read through the MSM table as |
| --- | --- |
| GPS L1C (0) | none (MSM id 0 is reserved) |
| GPS L1W (2) | L1C |
| GPS L2L (8) | L2C |
| GPS L2P (10) | L2W |
| GPS L2W (11) | none |
| Galileo L1C (2) | L1C (the one agreement) |
| Galileo L5Q (6) | L1Z |
| Galileo L7Q (9) | L6A |
| Galileo L6C (16) | L7X |

A bias whose suffix is empty is dropped; a bias filed under a wrong name is applied to that signal's rows.
The SSR coverage count is per satellite, so it read complete throughout.

## 3. Consequence for the live results

In every live and closed-stream session decoded from RTCM SSR before the fix, GPS C1C and L1C rows carried
the L1W biases, GPS L2W rows carried the L2P bias where the stream sends one and none otherwise, and Galileo
E5a, E5b and E6 rows carried no SSR bias. The estimated satellite biases of the float filter absorb a
constant misapplied bias, so the float positions are affected to a degree not measured here; integer
fixing outside Galileo E1C was not possible. The network producer's closed loop pushes records into the
store directly (no wire encoding) and is not affected.

## 4. Fix and test

`RtcmSignalMap` holds two table families: the MSM tables (unchanged, functions renamed `msmToRinexSuffix`
and `msmFromRinexSuffix`) and the SSR tables transcribed from cssrlib `ssig2rsig` (`ssrToRinexSuffix`,
`ssrFromRinexSuffix`). cssrlib maps QZSS id 2 to L1C, a duplicate of id 0; that id is left unassigned.
Both SSR bias bodies read through the SSR table. Tests: `RtcmSignalMapTest.SsrGpsAndGalileoEntries`,
`SsrNumberingDiffersFromMsm`, `SsrRoundTripEverySystem`; `RtcmSsrSignalIdTest.GpsCodeBiasUsesSsrNumbering`
and `GalileoCodeBiasUsesSsrNumbering` (4076 IM 25 and 65); the phase-bias cases of card SSR-PB-HDR
re-encoded with SSR ids (GPS L1C 0, L2W 11; Galileo L5Q 6). Those cases had been written with the decoder's
MSM ids and so pinned the wrong table.

## 5. The AR coverage key

With `model_corrections.bias_coverage_requires_apply` false (the loader default), `phaseBiasCoveredForAr`
counts a phase bias as AR coverage only when a .BIA file has an entry for it; SSR phase biases never count.
A live session loads no .BIA file, so every ambiguity was marked unresolvable (`smoke_ar`). The live AR
configuration sets the key true, which also changes the code-bias coverage gates of the float filter
(absorber allocation, the no-BIA downweight), so an AR session differs from a float session in more than
the AR mode.

## 6. Owed

- Unit suite and a Release build with the fix; a live AR smoke, then one 30-minute live AR session
  (`rt_pbhdr_2026-10-29\live_ge_static_ar`). Done: section 7.

## 7. Live sessions after the fix (2026-10-29)

Exe `gnss_processor_ssrsig.exe` (Release, the tree of this card on c37d4d6 + a78ba68, uncommitted at the
run; sha256[:16] `dc447feab9d92b7e`). Unit suite before the build: 3,623 cases, 3,622 passed, 1 skipped
(the fixture-gated case), 0 failed. Config: the `live_ge_static` config of card SSR-PB-HDR with
`ambiguity_resolution.mode` 1 (continuous, ratio 3.0, AR elevation mask 20 degrees, default 0.999
success-rate and 0.05 m position-sigma gates) and `model_corrections.bias_coverage_requires_apply` true.

**Smoke** (`smoke_ar3`, 60 s): AR candidates on GPS L1C and L2W and Galileo L1C, L5Q and L7Q (16 pairs in
the last epoch, against four Galileo L1C pairs before the fix). The attribute census reads 0 code-bias
lookups missing and leaves only phase lookups on GPS L1W and L2L, signals the stream sends no phase bias
for.

**Session** (`live_ge_static_ar`, 1,800 epochs, 21:45:39 to 22:15:38 GPS time, exit 0, 1,811 s wall):

| quantity | value |
| --- | --- |
| satellite-epochs with a phase bias | 28,798 of 28,798 |
| phase-bias resets | 0 |
| code-bias lookups with no bias for the tracked signal | 0 |
| first AR search (position-sigma gate open) | 21:48:14, 155 s into the session |
| MLAMBDA searches | 1,645, on 18 to 26 DD ambiguities |
| ratio (second best / best) | min 1.00, median 1.01, 99th percentile 1.05, max 1.10 (threshold 3.0) |
| fixes | 0 |
| best-candidate squared norm | 11,687 at the first search, 412,911 at the last |
| 3D position error, 1,200 to 1,800 s, median | 0.205 m (formal 3D sigma 0.004 m) |

Position error is against `receiver.reference_position_ecef` of `config/sbf_replay_covl145.json`, the
surveyed coordinate of the same roof receiver. The full search history is in `stdout.txt`; the
`ambiguity_resolver.log` sink rotates at 5 MB with one backup and keeps only the last 4 minutes.

The float DD ambiguities carry no integer information. Within each epoch the circular concentration of
the DD fractions of one (constellation, signal) group, median over the epochs after 600 s, is GPS L1C 0.18
(6 to 7 pairs; a uniform null gives 0.33 to 0.36), GPS L2W 0.38, Galileo L1C 0.42 and L5Q 0.41 (3 to 4
pairs; null 0.44 to 0.51), Galileo L7Q 0.73. The group means wander over the session, so the reference
satellite's own error does not explain the spread. A best-candidate norm four orders of magnitude above
the number of ambiguities, at a ratio of 1.0, is a float far from every integer under a covariance that
claims millimetres.

Verdict: the signal-id fix holds on the live stream (every tracked signal the stream covers now has its
own bias, and AR forms candidates on both constellations and all their covered signals). Integer fixing
on this stream still fails: 0 fixes in 30 minutes, the floats indistinguishable from uniform fractions.
The cause is not determined by these runs. Not checked: whether the stream declares its phase biases
integer (the decoder keeps `signal_integer`, and nothing logs it); the sign and observable convention
with which the real-time engine applies an SSR phase bias; the wind-up and yaw conventions against the
stream's; the 0.2 to 0.3 m float position offset seen in both live float sessions of 2026-10-29. One
float and one AR session on different hours are not an A/B of the float solution.

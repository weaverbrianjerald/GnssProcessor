# SSR-TIME: SSR reference time, high-rate clock pairing and receipt stamps

**STATUS**

- **id:** SSR-TIME
- **topic:** realtime
- **status:** closed
- **summary:** Three timing defects in the live SSR consumer, found by reading the code against IGS SSR Format v1.00. (1) The orbit rates and clock polynomial were evaluated from the SSR epoch; the standard's reference time is the epoch plus half the update interval (indicator 0, 1 s, excepted), and the decoded interval was never read. (2) A high-rate clock delta was added to whatever clock record was current, never paired with the clock message it refines, so a delta outlived its polynomial. (3) Receipt stamps came from the host clock without the 18 GPS-UTC leap seconds and were compared with GPS-time epochs, so every freshness age read 18 s high. FIXED and unit-tested. Landed on branch fix/ssr-live (c5afad5). Live check on CAS (2026-11-10, 2,085 epochs): every message kind declares and keeps a 5 s interval, no high-rate clock arrives, all rate terms are zero, and the gate's age is 2.2 to 2.3 s at the median against 20 s before the fix; the network products are byte-identical (2 h field and sim arms, real-time seam gate).
- **dates:** 2026-10-29 (finding, fix, unit tests); 2026-11-09 to 2026-11-10 (landed on branch fix/ssr-live, producer comparison, live check)
- **knobs / posture:** none. The network products are unaffected: their records declare no update interval (reference time = epoch), carry no high-rate clock, and are stamped with the epoch time.
- **code:** `include/rtcm/RtcmTypes.hpp` (`ssrUpdateIntervalSeconds`, `ssrReferenceTime`, `update_interval_s` on the orbit, clock, combined and high-rate records, `epoch_time` on the high-rate record), `src/core/rtcm/RtcmSsrDecoder.cpp`, `src/core/rtcm/RtcmNavDecoder.cpp` (receipt stamps via `TimeUtils::nowGpsTime`), `src/core/products/SsrCorrectionStore.cpp` (`getOrbitClock`: reference times, `highRateRefinesClock`; `coverageFor`), `src/core/realtime/RealTimeEngine.cpp` (`combinedOrbitPart`), `src/core/rtcm/RtcmParser.cpp` (`noteSsrUpdateInterval`), `tests/test_ssr_reference_time.cpp`
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_ssrlive_2026-11-09\{cas_static,capture}` (live check, exe `gnss_processor_ssrlive.exe`, sha256[:16] 9983adbdd7c2bbdd); `D:\GnssProcessorSupport\runs\rt_agu_2026-11-03` (the first live sessions with the fix, ages re-derived); `D:\GnssProcessorSupport\runs_ssrlive\_rap_ssrl_{f2,s2}_{main,br}` and `D:\GnssProcessorSupport\gates\ssrlive_rtseam` (producer comparison)
- **reference:** IGS SSR Format v1.00, 2020-10-05 (`https://files.igs.org/pub/data/format/igs_ssr_v1.pdf`): section 4 (reference time, update intervals, consistency), section 4.2 (high-rate clock), Table 7 IDF004 (the interval table)
- **source:** a review of how the live engine applies 5 s corrections to 1 s observations (2026-10-29)

**Outcome.** Does the live consumer evaluate SSR corrections at the times the standard defines? It did not, in three places: the polynomials ran from the SSR epoch instead of the epoch plus half the update interval, a high-rate clock delta was not tied to its clock message, and receipt stamps sat 18 s behind GPS time, so every freshness window was 18 s shorter than configured. All three are FIXED, unit-tested and landed on branch fix/ssr-live. Live on CAS the gate's age is 2.2 s at the median against 20 s before; CAS sends zero rate terms and no high-rate clock, so the other two fixes move nothing there (section 7).

> Results are appended TO THIS FILE, below the text.

---

## 1. Findings

**Reference time.** IGS SSR Format v1.00 section 4 defines the reference time of the correction terms as
the SSR epoch time plus half the SSR update interval, with indicator 0 (1 s) using the epoch itself. The
orbit correction is evaluated as the correction plus its rate times (t - t0) (section 4.1), the clock as
C0 + C1 (t - t0) + C2 (t - t0)^2 (section 4.2). The decoder stored the header's update-interval indicator
and nothing read it; `getOrbitClock` passed the SSR epoch as t0. Every polynomial was therefore evaluated
half an update interval too far ahead. The error is the rate times half the interval: nothing for a clock
sent with C1 = C2 = 0, the orbit rate times 2.5 s at a 5 s interval, times 30 s at a 60 s interval.

**High-rate clock pairing.** Section 4.2 adds the high-rate clock term to the corresponding clock
correction; section 4 makes the update interval the identifier of parameters that belong together. The
high-rate record kept only its value, receipt time and IOD-SSR (the epoch was dropped), a new clock
message left the previous delta in place, and `getOrbitClock` added any fresh delta. After a new C0
arrived, the old delta stayed on it until the next high-rate message; if high-rate messages stopped, the
last delta stayed on every new C0 until it aged out. IOD-SSR alone cannot pair the two: Table 7 (IDF007)
defines it as a change of generating configuration, not a per-epoch counter.

**Receipt stamps.** `nowReceiveTime` in both RTCM decoders converted the host clock to GPS week and
second without the GPS-UTC leap seconds (its comment called the offset irrelevant for staleness).
`freshEnough` compares the stamp with the observation epoch, which is GPS time, so a correction received
now read 18 s old: a 60 s `ssr_max_age_s` allowed about 42 s, the 30 s default about 12 s. The broadcast
ephemeris stamps had the same offset; nothing in `src/` reads them, and the SBF path already stamps
receiver GPS time.

## 2. Fix

- `ssrUpdateIntervalSeconds(index)`: the 16 values of Table 7 (IDF004), the same table as RTCM DF391.
- `ssrReferenceTime(epoch, update_interval_s)`: the epoch plus half the interval; an interval of 1 s or
  less keeps the epoch, which covers indicator 0 and the records of the network products (interval 0).
- The orbit, clock, combined and high-rate records carry the SSR epoch and the update interval as sent;
  the high-rate record now keeps its epoch. `getOrbitClock` sets the clock and orbit dt bases
  (`epoch_time`, `orbit_epoch_time`) to the reference times of their own messages.
- `highRateRefinesClock`: a delta is added only when its IOD-SSR equals the merged clock message's and
  its epoch lies in [clock epoch, clock epoch + clock update interval) (a clock without a declared
  interval is taken as 1 s). A delta without a clock part is not applied, and `coverageFor` no longer
  counts it as clock coverage.
- Both receipt stamps come from `TimeUtils::nowGpsTime` (host clock plus `realtime.leap_seconds`, the
  conversion the obs-lag measurement already used).
- `RtcmParser` logs each SSR message kind's update interval at info when first seen and when it changes
  ("SSR update interval: GPS clock (RTCM 4076 IM 22) 5 s (indicator 2)").

## 3. What does not move

The network producer's records are pushed into the store directly or replayed from the product file:
no update interval (reference time = epoch), no high-rate clock, receipt time = epoch time. The orbit
echo check compares reference times on both sides, which for those records are the epochs. No RTCM SSR
encoder exists in the code base, so no producer-side convention changes.

## 4. Unit tests

`tests/test_ssr_reference_time.cpp`: `SsrReferenceTimeTest` (the table, half the interval, the 1 s and
undeclared exceptions, the week rollover, separate and combined messages in the store, the undeclared
interval keeping the epoch), `SsrHighRateClockPairingTest` (inside the interval, replaced by a newer
clock, ahead of the clock, IOD-SSR change, paired with a combined record, alone neither applied nor
counted), `SsrTimingDecodeTest` (1058 and 1062 frames keep the epoch and interval, the decode result
names the message kind for legacy and 4076, the receipt stamp on the GPS time scale).

## 5. Owed

A live session on SSRC01CAS1 that logs the update interval of every message kind (the new info line),
then: the orbit-rate and C1 magnitudes against the interval (the size of the reference-time correction on
this stream), whether any high-rate clock subtype arrives (Galileo IM 64 is not excluded by the
sourcetable notes), and the freshness ages now that they are on one time scale (the BeiDou age of about
50 s noted on the IGS combination may include the 18 s offset).

## 6. Results, 2026-10-29

Debug build of `gnss_tests` (VS 2026 wrapper) clean, no new warnings in the touched files. The 17 new
cases pass; the related suites (SSR store, RTCM decode, store injection, phase-bias decode, real-time,
product I/O and emit) 97 of 97; the full suite 3,648 passed, 1 skipped (the SP3 fixture of
`test_math_core.cpp`, not bootstrapped), 0 failed. Nothing committed; no Release build or live session.

## 7. Results, 2026-11-09 and 2026-11-10 (UTC)

**Landed.** Branch `fix/ssr-live` (worktree `D:\GnssProcessorSupport\worktrees\ssrlive`, based on main
27d6ece): the change of sections 2 to 4 is commit c5afad5, copied file for file from the main working
tree, where it had sat uncommitted since 2026-10-29 beside the bias-sign change of card SSR-BIAS-SIGN
(commit f0edfeb, below it). Every code file of the two commits equals the main tree's copy once line
endings are ignored. Cards SSR-CLK-ORB (c6dea52) and SSR-BIAS-COV (383ec84) follow on the same branch.

**Unit suite.** Debug build of the branch at 383ec84: 4,207 cases in 558 suites, 4,170 passed, 37 skipped
(fixture-gated, none in an SSR or RTCM suite), 0 failed. The 17 cases of section 4 are among them.

**The network products do not move.** `ObservationEquation.hpp` (documentation only), `RtcmTypes.hpp` and
`SsrCorrectionStore.cpp` are shared with the network producer and its held-out consumer. 2 h arms of
`config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json`
(`scripts/rapid_arm.py make --hours 2`, run dirs `D:\GnssProcessorSupport\runs_ssrlive\_rap_ssrl_{f2,s2}_{main,br}`)
ran on the branch build at 383ec84 (`gnss_processor_ssrl_br.exe`, sha256[:16] 9983adbdd7c2bbdd) and on the
main build of the same flavour (the AVX2 Release exe `gnss_processor_srnint.exe` of the baseline, renamed,
sha256[:16] 64543d2d89994cae, built from int/srn-builds, whose C++ equals 27d6ece). `scripts/nom_td_gate.py
--whole`: RESULT IDENTICAL for both pairs, in every part: the arm configs, the products file in full, every
CSV (the consumer's `closed_loop_trace.csv`, `closed_loop_sat_trace.csv` and `closed_loop_isb.csv`
included), the run manifest, `stdout.txt`, the main_log set, `rt_engine.log` and the exit code
(`runs_ssrlive\gate_{f2,s2}_main_vs_br.txt`). The real-time seam gate (`scripts/rt_seam_gate.ps1` on both
builds, `scripts/rt_seam_compare.py main br`, arms sbf, sbf_bssd, sbf_te and cl) passes in all four arms
with 138 files identical, the cl arm's network products, `ssr_consumer_trace.csv` and closed-loop traces
included (`D:\GnssProcessorSupport\gates\ssrlive_rtseam\compare_main_br.txt`). The gate ran from a scratch
root with a junction to the main tree's `data/` and one change in its copy of `config/sbf_replay.json`,
`files.internal_data_root` set to `C:/GnssProcessor/data`: a worktree build bakes its own empty `data/` in
as the project root, and both builds read the same files. No arm moves, as expected: no arm carries a
decoded SSR stream (the cl consumer reads the in-process network products), the network records declare
no update interval, carry no high-rate clock, leave `needs_orbit_correction` false, `coverageFor`
feeds only the live coverage line, and the one edit under `src/core/realtime/` (`combinedOrbitPart`) runs only under the
`OrbitOnly` accept policy, which no arm uses.

**The ages of 2026-11-03, verified from that night's records.** Re-running `tools\ssr_ages.py` of
`D:\GnssProcessorSupport\runs\rt_agu_2026-11-03` on its two raw captures reproduces both age tables
byte for byte (the CNES file differs in its path label only). CAS (`capture\SSRC01CAS1_a.trc`, 897 s,
3,176 messages): engine age since the fix p50 2.0 to 2.3 s, p95 4.7 to 5.2 s, at most 6.5 to 8.6 s by
kind; the same gate before the fix would read 20.0 to 20.3 s at p50. CNES (`capture\SSRC00CNE1_a.trc`,
595 s, 1,679 messages): 2.5 s p50 against 20.5 s. The engine's own records agree: each of the four
sessions logged the update-interval line for its 12 message kinds (all 5 s, indicator 2) and none for a
high-rate clock, and the before/after pair of that night (`age_ab\`, live on CNES with a 15 s limit)
applied corrections on 0 of 1,920 satellite-epochs with the build before the fix (8,396 stale
rejections) and on 1,920 of 1,920 with the fixed build. The engine age is computed from the capture's
receipt stamps (host clock plus 18 s, the conversion `TimeUtils::nowGpsTime` makes), sampled every
second; the host clock was 0.063 s from time.nist.gov that night.

**Live check (owed by section 5).** Exe `gnss_processor_ssrlive.exe`, the Release build of the branch at
383ec84 (sha256[:16] 9983adbdd7c2bbdd), run beside the build's DLLs; the static configuration of
2026-11-03 (`rt_agu_2026-11-03\static\config.json`) with `processing.max_duration_s` 21,600 to 2,100 the
only change (`config_diff.txt`): the roof receiver, SSRC01CAS1, BCEP00BKG0, GPS + Galileo, float. Run
dir `D:\GnssProcessorSupport\runs\rt_ssrlive_2026-11-09\cas_static`, with a timed capture of
SSRC01CAS1 beside it (`capture\`, 2,099 s, 6,880 IGS-SSR messages; ages by `ssr_ages.py`, cadence by
`ssr_cadence.py` in the run dir). Host clock 0.048 s from time.nist.gov at 04:48 UTC. 2,085 epochs from 04:07:41 to 04:42:40 GPS time on 2026-11-10, exit 0.
A first launch at 04:04 UTC used a copy of the exe outside the build folder, without its DLLs; it never
loaded and was stopped (`attempt1\`).

| item | result |
| --- | --- |
| update interval per message kind (engine log) | 12 kinds (orbit, clock, code bias and phase bias of GPS, Galileo and BeiDou): 5 s, indicator 2 |
| transmission cadence (capture) | every kind on every 5 s epoch: 421 epochs per kind, spacing 5 s, no gap |
| high-rate clock | none: no engine line, no message in the capture |
| rate terms | orbit-rate and clock-rate terms zero on every satellite record, so the half-interval reference time moves nothing on this stream (as on 2026-11-03) |
| engine age, GPS and Galileo, p50 / p95 / max | 2.2 to 2.3 / 4.5 to 5.1 / 12.7 to 13.1 s; before the fix the same gate would read 20.2 to 20.3 s at p50 |
| latency at receipt, p50 | GPS and Galileo 0.8 to 1.0 s; BeiDou 14.8 to 15.0 s |
| content age (epoch minus SSR epoch), GPS and Galileo, p50 / max | 3 / 14 s |
| coverage (engine line, 32,187 satellite-epochs) | orbit, clock, code bias and phase bias 100 %; clock applied 99.98 % (E13 for 5 epochs, below) |
| stale rejections | 0 |
| clock applied above the in-store orbit count | 0 epochs; the start-up had every orbit and clock in store before the first epoch, so the start-up edge of card SSR-CLK-ORB was not exercised |

The freshness ages are on one time scale: the gate's age is about 2 s at the median, as the capture
predicts, not the 20 s of the build before the fix. One gap: 04:42:16 to 04:42:32 GPS, 40 s before the
end, the engine logged an observation backlog of 14 s and its skip-to-latest rule dropped the late
epochs (2,085 epochs instead of 2,100); the host was running other sessions' engines and this
branch's seam gate at the time. The cause of the stall is not identified.

**Observations, recorded as register lines** (`docs/status/OPEN_ITEMS_CONSUMER_REALTIME.md`), not built:

- A short correction gap on one satellite costs its phase for a minute. On 2026-11-03 (CNES) G16 had no
  fresh correction for 5 epochs (10:01:16 to 10:01:20 UTC); the engine opened a gap hold on its three
  phase signals, no across-gap check ran, and the hold timed out after 61 s and reset the three
  ambiguities: 56 epochs on code only. The live check repeats it on CAS: E13's clock correction was in
  store but not applied for 5 epochs (04:11:11 to 04:11:15 GPS, broadcast-grade, code only), its four
  phase signals were held, and the hold timed out after 61 s with a fresh reset. Why the corrections
  lapsed is not verified in either case (an ephemeris issue change is the likely cause).
- CAS dates BeiDou corrections in BeiDou time. CAS BeiDou corrections arrive 14.8 to 15.7 s after their
  stated epoch against 0.8 to 1.7 s for its GPS and Galileo, on both nights: the 14 s of BeiDou time
  against GPS time. CNES shows no such offset (13.0 s for every constellation). The decoder reads every
  IGS-SSR epoch as GPS time, so it would date CAS BeiDou corrections 14 s early. No session used BeiDou.
- The start-up clock warning overstates. At the first epoch, before any ephemeris has arrived, the
  geometry logger warns "has zero/invalid clock product; observations from this satellite will be
  skipped this session" for every tracked satellite (18, 11 and 21 lines in three sessions of
  2026-11-03, 20 in the live check); none is skipped, all are modelled within seconds.
- The stale-correction warning quotes another age than its gate. "SSR for G16 (GPS) REJECTED stale:
  |age|=73.9s > 60.0s" (`GnssCorrections.cpp`) prints the time since the satellite's last SSR epoch
  (`getSsrEpoch`, the content age), while the gate it reports compares receipt stamps; the two differ by
  the stream latency (about 1 s on CAS, 13 s on CNES), so on CNES the printed age is 13 s above the
  gate's.

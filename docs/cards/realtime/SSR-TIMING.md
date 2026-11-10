# SSR-TIME: SSR reference time, high-rate clock pairing and receipt stamps

**STATUS**

- **id:** SSR-TIME
- **topic:** realtime
- **status:** open
- **summary:** Three timing defects in the live SSR consumer, found by reading the code against IGS SSR Format v1.00. (1) The orbit rates and clock polynomial were evaluated from the SSR epoch; the standard's reference time is the epoch plus half the update interval (indicator 0, 1 s, excepted), and the decoded interval was never read. (2) A high-rate clock delta was added to whatever clock record was current, never paired with the clock message it refines, so a delta outlived its polynomial. (3) Receipt stamps came from the host clock without the 18 GPS-UTC leap seconds and were compared with GPS-time epochs, so every freshness age read 18 s high. FIXED and unit-tested; the live check is owed.
- **dates:** 2026-10-29 (finding, fix, unit tests)
- **knobs / posture:** none. The network products are unaffected: their records declare no update interval (reference time = epoch), carry no high-rate clock, and are stamped with the epoch time.
- **code:** `include/rtcm/RtcmTypes.hpp` (`ssrUpdateIntervalSeconds`, `ssrReferenceTime`, `update_interval_s` on the orbit, clock, combined and high-rate records, `epoch_time` on the high-rate record), `src/core/rtcm/RtcmSsrDecoder.cpp`, `src/core/rtcm/RtcmNavDecoder.cpp` (receipt stamps via `TimeUtils::nowGpsTime`), `src/core/products/SsrCorrectionStore.cpp` (`getOrbitClock`: reference times, `highRateRefinesClock`; `coverageFor`), `src/core/realtime/RealTimeEngine.cpp` (`combinedOrbitPart`), `src/core/rtcm/RtcmParser.cpp` (`noteSsrUpdateInterval`), `tests/test_ssr_reference_time.cpp`
- **run dirs:** none (no stream capture or SSR trace of a live session is on disk)
- **reference:** IGS SSR Format v1.00, 2020-10-05 (`https://files.igs.org/pub/data/format/igs_ssr_v1.pdf`): section 4 (reference time, update intervals, consistency), section 4.2 (high-rate clock), Table 7 IDF004 (the interval table)
- **source:** a review of how the live engine applies 5 s corrections to 1 s observations (2026-10-29)

**Outcome.** Does the live consumer evaluate SSR corrections at the times the standard defines? No, in three places: the polynomials ran from the SSR epoch instead of the epoch plus half the update interval, a high-rate clock delta was not tied to its clock message, and receipt stamps sat 18 s behind GPS time, so every freshness window was 18 s shorter than configured. All three are FIXED and unit-tested. The size of the first on the live CAS stream is unknown until a session logs its update intervals (section 5).

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

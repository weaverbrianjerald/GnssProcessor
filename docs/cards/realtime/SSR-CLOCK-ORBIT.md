# SSR-CLK-ORB: an SSR clock correction applied without the orbit correction it belongs to

**STATUS**

- **id:** SSR-CLK-ORB
- **topic:** realtime
- **status:** open
- **summary:** A stream that sends orbit and clock corrections in separate messages (CAS) can deliver a satellite's clock before its orbit: after a correction outage and at start-up. `SsrCorrectionStore::getOrbitClock` returned the fresh clock with no orbit part, and the engine modelled the satellite with a corrected clock on the uncorrected broadcast orbit, counted as fully corrected. On 2026-11-03 this happened for 6 epochs after the CAS outage (clocks of 17 satellites, orbits of 8) and for 5 epochs at a session start. FIXED: an RTCM or IGS-SSR clock now applies only with a fresh orbit of the same IOD-SSR; unit-tested.
- **dates:** 2026-11-09 (finding from the 2026-11-03 live sessions, fix, unit tests)
- **knobs / posture:** none (a store fix, no toggle). The network products do not move: the producer's clock records carry no orbit correction and leave the new flag false, so they apply on their own as before.
- **code:** `include/rtcm/RtcmTypes.hpp` (`SsrClockCorrection::needs_orbit_correction`), `src/core/rtcm/RtcmSsrDecoder.cpp` (`decodeClockBody` sets it), `src/core/products/SsrCorrectionStore.cpp` (`getOrbitClock`, `clockHasItsOrbit`), `include/products/SsrCorrectionStore.hpp` (`getOrbitClock` documentation), `tests/test_ssr_clock_orbit_pairing.cpp`
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_agu_2026-11-03\{kinematic,static}` (the evidence, before the fix); the session after the fix in section 5
- **reference:** IGS SSR Format v1.00, 2020-10-05 (`https://files.igs.org/pub/data/format/igs_ssr_v1.pdf`), section 4 (orbit and clock corrections of one IOD-SSR belong together)
- **source:** the live positioning sessions of 2026-11-03 (`D:\GnssProcessorSupport\presentation\agu26\figures\live_note.md`, section "Defects and observations")

**Outcome.** Can the live consumer apply a satellite's SSR clock without the orbit correction the clock was computed against? It could: on a stream with separate orbit and clock messages, a clock that arrived first was applied on the broadcast orbit and the satellite counted as corrected. After the 99 s CAS outage of 2026-11-03 this held for 17 clocks against 8 orbits during 6 epochs, while the kinematic error rose from 6 cm to 83 to 124 cm (the epochs also follow a 42 s hole in the phase record). FIXED: such a clock waits for a fresh orbit of its IOD-SSR; network clocks are unchanged. Unit-tested; results in section 5.

> Results are appended TO THIS FILE, below the text.

---

## 1. Evidence

The per-epoch coverage line of `rt_engine.log` ("SSR coverage: N sats | clk-applied | in-store orb clk
cbias pbias") counts the satellites whose clock correction was applied (`clk-applied`) and those with a
fresh orbit record in the store (`in-store orb`). A satellite can only be correctly corrected with both,
so `clk-applied` above `in-store orb` marks satellites modelled with a corrected clock on the broadcast
orbit. Over the four sessions of 2026-11-03 (GPS time stamps):

| session | stream | epochs | epochs with clk-applied > in-store orb | when |
| --- | --- | --- | --- | --- |
| kinematic | CAS (separate orbit and clock) | 8,655 | 11 | 05:57:12 to 05:57:16 (start-up, 14 clocks, 5 orbits); 08:11:47 to 08:11:52 (17 clocks, 8 orbits) |
| static | CAS | 8,663 | 6 | 08:11:47 to 08:11:52 (17 clocks, 8 orbits) |
| kinematic_cnes | CNES (one combined message) | 14,400 | 0 | |
| static_cnes | CNES | 14,400 | 0 | |

The CAS feed into the caster stopped at 08:09:50 UTC and returned at 08:11:29 UTC; the clock messages of
the first epoch back reached the store before the orbit messages of most satellites. The note quoted in
**source** gives the kinematic error during the 6 mixed epochs (83 to 124 cm against 6 cm before; E +41,
N -25, U -115 cm at 08:11:34 UTC) and the 42 s hole in the phase record that the same outage made, so
the size of the error cannot be assigned to the mixed epochs alone. The static position did not move.

## 2. The defect

`getOrbitClock` merged the newest fresh orbit and the newest fresh clock record of a satellite
independently. A fresh clock with a stale or missing orbit returned `has_clock` without `has_orbit`;
the corrections layer then added the clock correction to the broadcast clock, marked the satellite
cm-grade (not broadcast-grade), and the geometry kept the broadcast orbit. An RTCM or IGS-SSR clock
correction is computed for the orbit corrected by the orbit message of the same solution; on the
broadcast orbit the radial part of the orbit correction is missing from the modelled range.

## 3. Fix

- `SsrClockCorrection::needs_orbit_correction`: true when the clock was computed against the orbit
  correction of its own SSR solution. `decodeClockBody` sets it for every decoded clock (legacy 1058,
  1064 and the other clock messages, and IGS-SSR 4076 clocks share that function). Every other clock
  record leaves it false: the network producer's records (`NetworkProductEmitter`, the product-file
  replay) carry no orbit correction and are defined against the orbit the consumer already holds.
- `getOrbitClock`, separate orbit and clock records: a clock with the flag set is merged only when an
  orbit part is merged in the same call and that orbit's IOD-SSR equals the clock's
  (`clockHasItsOrbit`). Otherwise the clock is withheld: the satellite gets the orbit part alone (an
  orbit of another IOD-SSR) or nothing (no fresh orbit), and the existing broadcast-grade path takes it
  until the orbit arrives, at most one update interval later on a running stream. The high-rate clock
  of a withheld clock is not added (it has no clock message to refine).
- A combined record is unchanged: it carries its own orbit.
- No counter is added. The coverage line already shows each withheld clock at both edges: during the
  wait `clk-applied` drops below `in-store clk` while `in-store orb` shows the missing orbits, and the
  two meet again when the orbits arrive.

## 4. Unit tests

`tests/test_ssr_clock_orbit_pairing.cpp`, suite `SsrClockOrbitPairingTest`: a decoded 1058 clock carries
the flag; a clock with no orbit is withheld and applies once the orbit arrives (start-up); after a 99 s
outage a returning clock waits for a fresh orbit (the 2026-11-03 sequence); a clock of another IOD-SSR
is not paired with the orbit; the high-rate clock of a withheld clock is not added; a network clock
(flag false) applies on its own; a combined record beside a stale separate orbit is unaffected.

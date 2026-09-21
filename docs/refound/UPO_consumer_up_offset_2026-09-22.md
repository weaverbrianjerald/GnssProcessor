# UPO: the consumers' standing Up offset, and the roster held at the monument

**STATUS**

- **id:** UPO
- **topic:** network-vehicle
- **status:** open
- **summary:** READ 2026-09-22 (manager; config-only arms and consumer-only replays, no build): the standing Up offset of WTZZ00DEU (-0.085 m, scatter 0.010 m) is a DEFECT of the network's station coordinates. The roster is held fixed at MONUMENT coordinates while the observations refer to the antenna; 48 of the 59 field stations carry an eccentricity (median 0.053 m, mean 0.128 m, GRAZ00AUT 1.964 m, KRGG00ATF 1.033 m). Each station's wet delay absorbs about a quarter of its eccentricity (GRAZ00AUT 0.656 m of zenith wet delay) and the rest leaks into the wire regionally. With the roster reduced to the antenna reference points the consumer at WTZZ00DEU reads 0.52 0.04 0.01 0.01 0.02 0.01 m by hour (0.48 0.05 0.06 0.09 0.09 0.09 before), its mean Up is within 0.005 m of zero from hour 4, and the clock wire on its satellites goes from 0.0072 to 0.0034 m. Across the 16 census sites the effect is regional (DAV100ATA 0.101 to 0.056 m, WUH200CHN 0.273 to 0.161 m); the remaining offsets are site-specific. Build pre-registered in section 4 (`network.station_marker_to_arp`, default OFF); nothing built.
- **source:** [EV7](EV7_all_sat_clock_decouple_2026-09-21.md) section 7.7 (the standing Up offset is the largest term left at WTZZ00DEU)
- **knobs / posture:** `network.station_marker_to_arp` (to build, default OFF); the RF-0hv working bases with `network.phase_datum_gate_drop_erase` armed (baseline pair `runs/_rf0hv6_{field,sim}6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv6_field6`, `runs/_rap_arp_roster`, census keys `ev7fix`, `arp`, `tq9`, `tq8`, `tq7`, `up_*`; exe `gnss_processor_ev7.exe`

## 1. The offset

On the baseline wire `runs/_rf0hv6_field6` the consumer at WTZZ00DEU has, over hours 4 to 6,
a mean East of +0.021 m, North 0.000 m and Up -0.085 m, with a scatter of 0.010 m in Up:
a constant, not a wander. Across 15 census sites (QUIN00USA diverges and is excluded) the
hours 4 to 6 mean Up is negative at 11, with a median of -0.031 m; KOKB00USA reads -0.219 m
and WUH200CHN -0.247 m. Tools: `runs/_msc/scripts/msc_replay_set.py` (consumer-only replays
of the 16 sites and of the wire's own consumer with extra consumer-side settings, 12 to 25 s
each) and `runs/_msc/scripts/msc_up.py` (mean E, N, U and 3D p50 per site and key).

## 2. What it is not (consumer-only replays on the fixed wire)

| key | the one consumer-side change | WTZZ00DEU mean Up (m) | sites with a smaller absolute mean Up | median change of the absolute mean Up (m) |
| --- | --- | --- | --- | --- |
| `tq9` | none (control; reproduces the closed loop) | -0.085 | | |
| `tq8` | zenith-delay process noise 1e-9 to 1e-8 m^2/s | -0.094 | 7 of 16 | +0.0020 |
| `tq7` | the same, 1e-7 | -0.101 | 7 of 16 | +0.0018 |
| `up_rpcv` | receiver antenna phase-centre variations off | -0.066 | 4 of 16 | +0.0156 |
| `up_rpco` | receiver antenna phase-centre offset off | +0.080 | 7 of 16 | +0.0111 |
| `up_spcv` | satellite antenna phase-centre variations off | -0.120 | 6 of 16 | +0.0126 |
| `up_pole` | pole tide off | -0.083 | 6 of 16 | +0.0005 |
| `up_wu` | phase wind-up off | -0.086 | 6 of 16 | +0.0021 |
| `up_m10` | consumer elevation mask 15 to 10 degrees | -0.073 | 11 of 16 | -0.0074 |
| `up_m20` | consumer elevation mask 15 to 20 degrees | -0.094 | 5 of 16 | +0.0123 |
| `up_set` | solid Earth tide off | -0.227 | 1 of 16 | +0.1349 |

- The stiff zenith-delay state of the consumer is not the cause.
- The marker to antenna reduction of the consumer's TRUTH is in place (memory of 2026-07-29;
  HARB00ZAF and STPM00SPM carry 3.05 m and 3.04 m of eccentricity and read -0.014 and
  -0.038 m).
- No single consumer-side model term removes the offset. The offset grows with the mask
  angle, which is the signature of an elevation-dependent inconsistency between the wire
  and the consumer's geometry.
- Side finding for card MSC section 5: QUIN00USA does not diverge with a 10 degree mask
  (42 m to 0.076 m): its excursion is a thin-geometry failure of the consumer.

## 3. What it is: the roster is held at the monument

`src/core/app/ApplicationNetworkSetup.cpp` sets each station's fixed position from the
configured `position_ecef` with no reduction, and the configured values are the ITRF2020
monument coordinates (`scripts/resolve_itrf_ecef.py`). The antenna reference point is
`ANTENNA: DELTA H/E/N` away from the monument, and the antenna model (phase-centre offsets
and variations) is relative to that point. The consumer's truth has been reduced since
2026-07-29; the roster never was.

Field roster, from the RINEX headers: 48 of 59 stations carry an eccentricity, 37 above
0.01 m, 16 above 0.10 m, 2 above 1 m; median 0.053 m, mean 0.128 m. Largest: GRAZ00AUT
1.964 m, KRGG00ATF 1.033 m, GUAM00GUM 0.623 m, SEYG00SYC 0.519 m, DJIG00DJI 0.440 m,
CPVG00CPV 0.430 m. Near the consumer: GRAZ00AUT 1.964 m, GOPE00CZE 0.111 m, WTZR00DEU
0.071 m. Every sim station has a zero eccentricity, so the sim bed cannot show the defect.

A station held too low by H misfits every range by H times the sine of the elevation.
The station's clock and zenith delay absorb most of it; the zenith wet delay takes about
a quarter of H. Measured, hours 4 to 6, `network_zwd.csv`:

| station | eccentricity H (m) | zenith wet delay, roster at the monument (m) | roster at the antenna reference point (m) |
| --- | --- | --- | --- |
| GRAZ00AUT | 1.964 | 0.656 | 0.176 |
| KRGG00ATF | 1.033 | 0.364 | 0.045 |
| GUAM00GUM | 0.623 | 0.405 | 0.277 |
| SEYG00SYC | 0.519 | 0.509 | 0.355 |
| DJIG00DJI | 0.440 | 0.327 | 0.231 |
| CPVG00CPV | 0.430 | 0.211 | 0.110 |
| GOPE00CZE | 0.111 | 0.175 | 0.151 |
| WTZR00DEU | 0.071 | 0.167 | 0.152 |

What the station cannot absorb is an elevation-dependent pattern, and the satellite
clocks take the regional mean of it. A consumer in that region, with a free position,
is pulled down.

**The arm.** `runs/_rap_arp_roster`: the armed base with every roster `position_ecef`
moved by its header eccentricity along the local up, east and north (a scripted edit of
the arm's config; 48 stations moved; nothing else changed).

| | 3D p50 by hour at WTZZ00DEU (m) | mean Up, hours 3 to 6 (m) | below 0.1 m | clock wire, all / tracked (m) | refloats |
| --- | --- | --- | --- | --- | --- |
| `_rf0hv6_field6` | 0.48 0.05 0.06 0.09 0.09 0.09 | -0.057 -0.087 -0.089 -0.080 | 577 | 0.0091 / 0.0072 | 69 |
| `_rap_arp_roster` | 0.52 0.04 0.01 0.01 0.02 0.01 | +0.011 +0.000 -0.005 -0.003 | 612 | 0.0069 / 0.0034 | 66 |

Census (key `arp` against `ev7fix`, hours 4 to 6): the absolute mean Up is smaller at 9 of
15 sites and the 3D p50 better at 9; DAV100ATA 0.101 to 0.056 m (Up -0.074 to +0.001;
KRGG00ATF is its region's large eccentricity), WUH200CHN 0.273 to 0.161 m, ARHT00ATA
0.049 to 0.028 m; median count of epochs below a decimetre 235 to 282; mean hourly p50
over hours 2 to 6 better at 5 sites and worse at 2 (median -0.004 m). The effect is
regional, as the mechanism says. What remains is site-specific: KOKB00USA -0.197 m and
WUH200CHN -0.149 m in Up, and horizontal means of 0.04 to 0.12 m at POHN00FSM, UTQI00USA,
SCRZ00BOL, ANMG00MYS, BAKE00CAN, ASCG00SHN and CHTI00NZL. Three Antarctic roster stations
keep a negative zenith wet delay (CAS100ATA -0.090 m, SCTB00ATA -0.088 m), which is not
physical and is unread.

The ruling of 2026-07-29 left the roster untouched and asked for no eccentricity in the
grading. This card does not touch the grading. It reports that the roster's monument
coordinates are a modelling error of the network itself, measured in its own zenith
delays, and that it carried the whole standing offset of the project's primary consumer.

## 4. The build, pre-registered

**Knob.** `network.station_marker_to_arp` (bool, default false): every roster station's
configured `position_ecef` is a monument coordinate and is reduced to the antenna
reference point at setup with that station's `ANTENNA: DELTA H/E/N`, through the helper
the consumer's truth already uses (`Coordinates::markerToArp`).

**Requirements.**

- U1. The reduction happens once, at setup, before any use of the station position. A
  station whose header has no eccentricity record is left unchanged and warned.
- U2. Telemetry in both knob states, products untouched when OFF: one line per station
  with a nonzero eccentricity (name, H, E, N), and a summary (stations reduced, largest
  H). With the knob OFF and any eccentricity above 0.01 m the summary is a warning that
  names the count and the largest value.
- U3. The closed-loop consumer is unaffected (its truth and its seed are already reduced).
  Appended to the warm-resume fingerprint only when armed.
- U4. Default OFF with byte identity. A unit test for the reduction at the roster site
  (order H, E, N; exact no-op at zero; agreement with the consumer-truth path).
- U5. Tool: a `rapid_arm.py` op that gives a station an eccentricity on the sim bed by
  writing it into the arm's RINEX header copy and lowering the configured coordinate by
  the same vector, so that the true antenna stays where the simulator put it.

**Gates and bars (frozen).**

- G0. Knob absent: the three graded files identical to `runs/_rf0hv6_{field,sim}6`.
- G1. Field ON reproduces the scripted arm `runs/_rap_arp_roster`: every hourly p50 within
  0.002 m, the zenith wet delays of the eight stations of section 3 within 0.002 m, 48
  stations reported as reduced.
- G2. Sim truth, the ingredient added: four sim stations nearest the sim consumer given
  eccentricities of 2.0, 1.0, 0.5 and 0.1 m by U5. Knob OFF: the defect appears (the
  four zenith wet delays rise by 0.15 to 0.35 of H and the consumer's mean Up over hours 3
  to 6 moves by more than 0.01 m). Knob ON: every hourly p50 of the consumer within
  0.002 m of the sim baseline and the four zenith wet delays within 0.002 m of theirs.
- B1. WTZZ00DEU: every hourly p50 from hour 3 at most 0.03 m; hour 1 and 2 within 0.05 m
  of the baseline; clock wire at most 0.0091 / 0.0072 m.
- B2. Census against `ev7fix`: mean hourly p50 over hours 2 to 6 better at more sites
  than worse with a median change at most 0; the absolute mean Up over hours 4 to 6
  smaller at more sites than larger.
- B3. Only if B1 and B2 pass: 12 h field, same reading against a 12 h OFF arm.

**Reading.** G0 to G2 are build gates. B1 to B3 passing gives a recommendation to arm
the knob in the working bases; the decision is the author's. The two goldens carry the
same monument roster; they are out of scope here and join the deferred golden re-base.

**Invariants pre-flight.** 3: no deferral that blocks the field (a station without a
header record is unchanged and warned). 4: U2. 5: graded on consumer position, station
zenith delays against a physical expectation, and the clock wire against an external
product within arcs. 6: G0.

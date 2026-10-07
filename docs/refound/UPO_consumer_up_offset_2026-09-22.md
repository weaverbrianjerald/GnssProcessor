# UPO: the consumers' standing Up offset, and the roster held at the monument

**STATUS**

- **id:** UPO
- **topic:** network-vehicle
- **status:** closed
- **summary:** ARMED 2026-09-22 (ledger RECOMMEND_SCOPED): the consumers' standing Up offset was a DEFECT, the network roster held at monument coordinates; `network.station_marker_to_arp` reduces each station to its antenna reference point.
- **source:** [EV7](EV7_all_sat_clock_decouple_2026-09-21.md) section 7.7 (the standing Up offset is the largest term left at WTZZ00DEU)
- **knobs / posture:** `network.station_marker_to_arp` (built main `95c7ea8`, default OFF; armed in the RF-0hv working bases on 2026-09-22 at the author's instruction, later pinned on the goldens by card GRB); the base: the RF-0hv working bases with `network.phase_datum_gate_drop_erase` armed (baseline pair `runs/_rf0hv6_{field,sim}6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv6_field6`, `runs/_rap_arp_roster`, census keys `ev7fix`, `arp`, `tq9`, `tq8`, `tq7`, `up_*`; exe `gnss_processor_ev7.exe`; build main `95c7ea8`, exe `gnss_processor_upo.exe`; armed baseline pair `runs/_rf0hv7_{field,sim}6`

**Outcome.** The question: what holds WTZZ00DEU's consumer 0.085 m low. The network held its roster at monument coordinates while the observations refer to the antenna (48 of 59 stations eccentric, GRAZ00AUT 1.964 m); reduced, WTZZ00DEU's 3D p50 is 0.01 to 0.02 m from hour 3, and on the sim, injected eccentricities reproduce the defect and the knob removes it byte for byte. G0, G1, B1 and B2 PASS; G2 misses one OFF sub-bar (0.406 of H at the 0.1 m station). Card [GRB](GRB_golden_rebase_2026-09-22.md) pinned the knob on the goldens; the residual offsets of section 5.7 are owed under card [SCU](SCU_station_coordinates_unknowns_2026-09-22.md), parked ([OPEN_ITEMS](../status/OPEN_ITEMS_A_current_program.md)).

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

## 5. The build: result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`)

### 5.1 The build

Main `95c7ea8`. The reduction sits in `buildStation` of
`src/core/app/ApplicationNetworkSetup.cpp`, immediately after the configured coordinate
is assigned and before every reader of the station position (the worker's list: the
filter's initial state, the models' receiver position and with it tides, antenna terms,
wind-up and troposphere; the per-epoch geometry of the ionosphere, fast-clock and
slip seams; the roster trace). Three grid lookups keep the header's approximate position
(atmospheric and ocean loading grids, ocean pole tide): a shift of metres is null at
their resolution. `RinexHeader` gained a record-present flag so that an absent
`ANTENNA: DELTA H/E/N` is left unchanged and warned. Telemetry in both knob states: a
line per station with an eccentricity, a summary that WARNS when the knob is OFF (the
field base reads: 37 of 59 stations above 0.01 m, largest +1.9640 m at GRAZ00AUT), the
run-end readout and a `marker_to_arp` object in the run manifest. Armed-only fingerprint
append. The closed-loop consumer is untouched (its truth line is character-identical OFF
and ON). Suite `StationMarkerToArpTest`, 6 cases; `gnss_tests` 2420 ran, 2419 passed, 1
known skip. Exe `gnss_processor_upo.exe` (`E2E177AB...`). Loader parity 0 gaps, golden
verify PASS, `rapid_arm.py selftest` 58 of 58 (the `ecc` op of U5).

### 5.2 Gates and bars

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knob absent, three files identical on both beds | identical | PASS |
| G1 | hourly p50 within 0.002 m of the scripted arm | 0.517 0.042 0.014 0.011 0.017 0.010 m on both, largest difference 0.0000 m | PASS |
| G1 | eight zenith wet delays within 0.002 m | largest difference 2.5e-5 m | PASS |
| G1 | 48 stations reported reduced | 48 | PASS |
| G2 OFF | wet delays rise by 0.15 to 0.35 of H | 0.263, 0.233, 0.255 and 0.406 (the 0.1 m station: a rise of 0.041 m) | 3 of 4 |
| G2 OFF | consumer mean Up over hours 3 to 6 moves by more than 0.01 m | 0.25 to 0.36 m | PASS |
| G2 ON | hourly p50 and the four wet delays within 0.002 m of the sim baseline | the three graded files byte-identical to `runs/_rf0hv6_sim6` | PASS |
| B1 | every hourly p50 from hour 3 at most 0.03 m | 0.014 0.011 0.017 0.010 | PASS |
| B1 | hours 1 and 2 within 0.05 m of the baseline | +0.033, -0.003 | PASS |
| B1 | clock wire at most 0.0091 / 0.0072 m | 0.0069 / 0.0034 m | PASS |
| B2 | census, hours 2 to 6: more sites better than worse, median at most 0 | 5 / 2, -0.004 m | PASS |
| B2 | absolute mean Up over hours 4 to 6 smaller at more sites | 9 smaller, 6 larger of 15 | PASS |
| B3 | 12 h against a 12 h OFF arm | section 5.4 | read delivered |

The one number outside its band is the OFF rise at the 0.1 m station (0.406 of H). Its
absolute size, 0.041 m, is at the scale of the between-station spread of the wet delays
on the bed; the three larger eccentricities sit at 0.23 to 0.26 of H, like the field.

### 5.3 The truth bed

`runs/_rap_u2_sim_ecc_off` and `_on`: the four sim stations nearest the sim consumer
(S48000SIM at 579 km, then 2425, 2547 and 3196 km) given eccentricities of 2.0, 1.0, 0.5
and 0.1 m by the `ecc` op, which writes the header record and lowers the configured
coordinate, so the true antenna stays where the simulator put it.

| | 3D p50 by hour (m) | mean Up by hour (m) | below 0.1 m |
| --- | --- | --- | --- |
| sim baseline | 0.601 0.104 0.096 0.056 0.055 0.049 | -0.560 +0.080 +0.087 +0.047 +0.046 +0.036 | 448 |
| eccentricities, knob OFF | 0.564 0.127 0.274 0.279 0.202 0.222 | -0.522 -0.114 -0.276 -0.269 -0.199 -0.212 | 41 |
| eccentricities, knob ON | 0.601 0.104 0.096 0.056 0.055 0.049 | -0.560 +0.080 +0.087 +0.047 +0.046 +0.036 | 448 |

The defect reproduces on the truth bed with the field's signature (a negative standing
Up at the consumer, wet delays raised by a quarter of H), and the knob removes it exactly.

### 5.4 Twelve hours (field)

| | 3D p50 by hour (m) | mean Up by hour (m) | below 0.1 m | clock wire, hours 2 to 6 (m) |
| --- | --- | --- | --- | --- |
| OFF | 0.484 0.045 0.056 0.088 0.090 0.085 0.092 0.072 0.101 0.124 0.176 0.125 | +0.780 -0.011 -0.057 -0.087 -0.089 -0.080 -0.084 -0.059 -0.085 -0.082 -0.124 -0.049 | 875 | 0.0131 / 0.0086 |
| ON | 0.517 0.042 0.014 0.011 0.017 0.010 0.013 0.032 0.032 0.095 0.139 0.124 | +0.830 +0.030 +0.011 +0.000 -0.005 -0.003 +0.011 +0.008 +0.004 +0.006 -0.063 -0.013 | 1046 | 0.0098 / 0.0059 |

Eleven of twelve hours are better, by 0.03 to 0.08 m from hour 3 to hour 9; hour 1 is
0.033 m worse. Hours 10 to 12 stay soft in both arms (0.095, 0.139 and 0.124 m with the
knob ON; the mean Up of hour 11 is -0.063 m): an older open item, not this card's.

### 5.5 Card SCC on this base (discriminator)

`runs/_rap_u4_s10`: the knob ON plus the second code channel at scale 10.

| | 3D p50 by hour at WTZZ00DEU (m) | mean Up, hours 4 to 6 (m) | clock wire (m) | census median p50 by hour (m) | median count | code-bias dropouts | QUIN00USA epochs above 5 m after hour 2 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `u1_field_on` | 0.517 0.042 0.014 0.011 0.017 0.010 | +0.000 -0.005 -0.003 | 0.0069 / 0.0034 | 0.829 0.265 0.086 0.095 0.102 0.090 | 282 | 50 | 265 |
| `u4_s10` | 0.549 0.057 0.027 0.030 0.033 0.029 | -0.022 -0.026 -0.025 | 0.0068 / 0.0033 | 0.677 0.235 0.118 0.095 0.097 0.078 | 331.5 | 4 | 0 |

Against the `upo` replays the channel is better on hours 2 to 6 at 7 sites and worse at 3
(median -0.007 m) and takes hour 1 down by a median of 0.136 m (11 better, 5 worse);
QUIN00USA no longer diverges (3D p50 0.080 m over hours 4 to 6). It costs WTZZ00DEU
0.015 to 0.02 m from hour 3 (a mean Up of -0.025 m), which still fails that card's
frozen bar on the mean over hours 3 to 6. Recorded in card SCC.

### 5.6 Reading and recommendation

G0, G1 and G2 pass, with one sub-bar of the G2 OFF arm outside its band at the smallest
eccentricity; B1 and B2 pass; the 12 h read supports it. Recommendation: arm
`network.station_marker_to_arp` in `config/refound/{field,sim}_gps_dual_rf0hv.json`
(the sim is byte-identical with it: no sim station has an eccentricity). The field
baseline would become `runs/_rap_u1_field_on`: 0.52 0.04 0.01 0.01 0.02 0.01 m by hour,
clock wire 0.0069 / 0.0034 m. The decision is the author's; it changes what the
ruling of 2026-07-29 left alone. The two goldens carry the same monument roster and
join the deferred golden re-base.

### 5.7 What stays open

- Site-specific offsets of the census: KOKB00USA (-0.197 m Up) and WUH200CHN (-0.149 m),
  horizontal means of 0.04 to 0.12 m at seven sites, UTQI00USA and KOKB00USA never below
  a decimetre.
- The negative zenith wet delays of CAS100ATA and SCTB00ATA (-0.09 m with the knob ON).
- The soft hours 10 to 12 of the 12 h arm.
- The consumer's elevation mask (10 degrees: 11 of 16 sites better, QUIN00USA cured) and
  the consumer's unbounded reaction at thin geometry.
- Hour 1 is 0.03 m worse with the knob ON at WTZZ00DEU (0.517 against 0.484 m).

## 6. 2026-09-22: ARMED in the RF-0hv working bases

At the author's instruction `config/refound/{field,sim}_gps_dual_rf0hv.json` now carry
`network.station_marker_to_arp: true`, in the same commit as card SCC's second code
channel (three keys plus dated comment lines). Working bases only: no loader default, no
golden. The promotion-ledger rows and the loader's posture rows land in a separate commit.

Confirmation arms from the armed bases with only the product path changed, exe
`gnss_processor_rf7.exe` (`97547306...`), both exit 0: `runs/_rf0hv7_field6` is
byte-identical to `runs/_rap_u4_s10` on `products.txt` (lines starting with `#`
removed), `closed_loop_trace.csv` and `network_epoch_status.csv`, and
`runs/_rf0hv7_sim6` is byte-identical to `runs/_rf0hv6_sim6` on the same three files.
Both knobs are inert on the sim and the run says so: 0 of 60 sim stations carry an
eccentricity, and the second code channel's run-end line reads that no companion code
row was ever added (one band-1 code per sim station). Field setup line: 48 of 59
stations carry an eccentricity (37 above 0.01 m), 48 reduced, largest H +1.9640 m at
GRAZ00AUT, no station without a header record.

**The baseline pair from this date: `runs/_rf0hv7_{field,sim}6`.** Field: consumer 3D p50
by hour 0.549 0.057 0.027 0.030 0.033 0.029 m, mean Up by hour +0.726 +0.017 +0.004
-0.022 -0.026 -0.025 m, 610 epochs below a decimetre (reported, not a bar), clock wire
0.0068 / 0.0033 m, clock-anchor trips 1, post-fit refloats 87. Sim: unchanged, 0.601
0.104 0.096 0.056 0.055 0.049 m. Census replays of the field wire: key `upos10`
(identical wire). The 0.02 m of Up in hours 4 to 6 belongs to the second code channel
(card SCC section 9), not to this knob.

### 6.1 The reduction graded at the roster's own stations (59 leave-in replays)

Instrument: every roster station replayed as a closed-loop consumer on a wire
(`runs/_msc/scripts/msc_roster_audit.py`; the station's data helped form the wire with
its coordinate held, so the replay is leave-in; mean E / N / U over hours 4 to 6 against
the configured coordinate reduced to the antenna reference point, 14 s per station).
Key `ros6` = the wire of `runs/_rf0hv6_field6` (roster at the monument), `ros7u` = the
wire of `runs/_rap_u1_field_on` (this card's knob alone). Over the 56 stations whose
replay converges on both wires: the absolute mean Up is smaller at 40, median change
-0.0174 m; the 3D p50 over hours 4 to 6 is better at 41, median change -0.0139 m; median
absolute mean Up 0.062 to 0.024 m; stations with an absolute mean Up above 0.05 m 34 to
19.

### 6.2 The defect had been detected all along, by an instrument nothing read

The measure-only station-congruence screen (`docs/SPEC_redundancy_census.md`, armed in
every one of these runs, run-end table in `stdout.txt`) ranks the monument roster by
eccentricity. Run-median statistic geo(t) on `runs/_rf0hv6_field6`, roster median 0.91:
GRAZ00AUT 26.10 (1.964 m), KRGG00ATF 8.99 (1.033 m), GUAM00GUM 3.87 (0.623 m), CPVG00CPV
2.83 (0.430 m), SEYG00SYC 2.75 (0.519 m). With the knob ON (`runs/_rap_u1_field_on`):
0.53, 0.68, 2.54, 1.27, 1.17. Its coordinate ESTIMATE does not carry the size (GRAZ00AUT
-0.100 m Up against 1.964 m): on the float vehicle the carried ambiguities absorb the
constant part of a coordinate error at birth and the post-fit rows keep only its change
with the geometry. The statistic detects; it does not measure. The same table flags
MIZU00JPN at 21.6 to 29.1 on every run, which is a second roster defect: card
[SCU](SCU_station_coordinates_unknowns_2026-09-22.md).

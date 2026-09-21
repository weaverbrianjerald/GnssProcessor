# EV7: the all-satellite clock-anchor decouple of 05:22 UT (TOW 278520)

**STATUS**

- **id:** EV7
- **topic:** clock
- **status:** open
- **summary:** FIX BUILT 2026-09-21 (main `1935079`, `network.phase_datum_gate_drop_erase`, default OFF), EVERY FROZEN GATE PASSES, NOT ARMED (the decision is the author's; recommendation: arm it in the working bases). Field 6 h: 13 drop edges all handled, carried jumps above 0.5 m 8 to 0, clock-anchor trips 50 to 1, no all-satellite epoch (3 before), refloats 183 to 69, WTZZ00DEU cost -0.011 against +0.019 m, clock wire 0.0091 / 0.0072 against 0.0184 / 0.0107 m; census 13 of 16 sites better on hours 2 to 6, the last half hour's across-site median 0.171 to 0.099 m; sim byte-identical, the relabelled truth arm cured; 12 h: trips 134 to 8. With the trigger fixed the guard made inert changes nothing, so no response-side build is needed. Earlier summary: RESULT 2026-09-21: the trigger is found and is a DEFECT of the coverage gate on the float vehicle. When `network.phase_datum_min_stations` removes a phase-bias column because only one station still tracks the signal, that station's CARRIED ambiguity is left defined against the removed column and absorbs the whole bias in one epoch (G30 at VOIM00MDG: +15.8 m against a 1.7 cm prior, no refloat, no variance inflation). Removing the edge removes the episode (0 of 26 clock trips, 0 of 72 refloats, WTZZ00DEU cost -0.011 m against +0.019 m, clock wire 0.0064 against 0.0107 m, 12 of 16 census sites better in the last half hour, several by metres). All three all-satellite decouple epochs of the run sit on this edge (13 such edges in 6 h). The G19 reset at URUM00CHN is a false alarm and costs nothing here. With the guard made inert the cost also vanishes, so the guard's response inflicts most of the consumer loss; that is read again on the fixed base. Fix pre-registered in section 6 (`network.phase_datum_gate_drop_erase`, default OFF); nothing built yet. Original summary: Pre-registered 2026-09-21 (event card: one event, a 60-epoch window, five-minute one-change arms). At TOW 278520 every emitted satellite clock trips the Q3 clock-anchor gate in one epoch on every field run since RF-0hv; on the corrected baseline `runs/_rap_r6_dual` the consumer at WTZZ00DEU steps from 0.089 to 0.196 m and then holds a plateau of 0.108 m to the end of the run. The manager's read of existing runs fixes the trigger epoch's identities (one satellite-signal column loses its only station, one anchor arc is refloated) and shows the clock innovations are one fixed satellite pattern whose amplitude changes tenfold with the roster. Trigger arms, a response ladder and a code read are frozen below; nothing is built.
- **source:** [RXP](RXP_rinex3_blank_prn_2026-09-21.md) section 3 (the loss of the corrected baseline is this episode); [EV6](EV6_hour6_decouple_event_2026-09-15.md) (the same mechanism class on the older posture); [PBH](PBH_phase_bias_hold_2026-09-19.md) section 7 item 4 (with the phase bias held, the decouple excursion is the largest remaining wire term on the field)
- **knobs / posture:** none armed; config-only discriminators `network.clock_anchor_resolve`, `network.clock_anchor_k`, `network.clock_anchor_recouple_epochs`
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** `runs/_rap_r6_dual`, `runs/_rf0hv5_field6`, `runs/_rap_r7_*`, `runs/_rap_r6_drop4_new`; exe `build/x64-Release/gnss_processor_rxp.exe`

## 1. What the existing runs already say (read by the manager, no new run)

Bed: field 2024-129, 59 stations, GPS L1/L2, 6 h cold start, 30 s, the RF-0hv working
posture with the three knobs of 2026-09-20. Runs read: the corrected baseline
`runs/_rap_r6_dual`, the pre-fix baseline `runs/_rf0hv5_field6`, and the roster arms of
card RXP.

**1.1 The epoch's identity rows are the same in every run** (seven runs, `network_events.csv`,
TOW 278520):

| tier | event | identity | detail |
| --- | --- | --- | --- |
| T1_if_slip | refsat_reset | G19 / URUM00CHN | refloat L1W;L2W |
| PHASE_ANCHOR | anchor_slip_handover | G19_L1W / URUM00CHN | to ULAB00MNG, branch continuous |
| STRANSFORM | sts_applied | G19_L1W / ULAB00MNG | +0.697 m (pre-fix run +0.326 m), marked=0 |
| TOPOLOGY | row_kinds | G30 / VOIM00MDG | obs_code+obs_phase becomes obs_code+obs_phase+obs_other |
| TOPOLOGY | col_removed | `CB:G30:C1W`, `PB:G30:L1W` | |
| READMIT | park | G30 | "D2 clock + code-bias carry parked at departure" |

VOIM00MDG is the only station that carries G30 on the L1W family at TOW 278490
(`network_amb_state.csv`). Its raw file tracks C1C, C1W, L1C, L1W, C2W and L2W for G30
without a gap through the epoch; the one thing that changes at 05:22:00 is `S1W`, which
falls to 34.75 dB-Hz (37.0 three minutes earlier, 35.5 one epoch later). The trigger is
in the data and is the same in every run.

**1.2 The clock innovations are one fixed pattern with a roster-dependent amplitude.**
All 26 emitted clocks trip `CLK_ANCHOR cta_decouple reason=nis` (23 at pass 0), with
first-solve innovations from -0.219 m (G30) to +0.202 m (G29) against a gate of about
1.4 cm. Across the 22 satellites common to the two baselines the corrected run's
innovations are the pre-fix run's times 1.84 (correlation 0.994).

| run | CAS100ATA | LCK400IND | trips (pass 0) | sum of abs innovation (m) | post-fit refloats 278520 to 278640 | WTZZ PRE / PEAK / PLATEAU (m) | cost (m) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` (old reader) | no satellite below G10 | no satellite below G10 | 22 (16) | 0.739 | 21 | 0.070 / 0.114 / 0.055 | -0.015 |
| `_rap_r6_drop4_new` | absent | absent | 23 (17) | 0.758 | 22 | 0.071 / 0.120 / 0.060 | -0.011 |
| `_rap_r7_only_cas1` | present | absent | 24 (18) | 0.894 | 26 | 0.071 / 0.134 / 0.067 | -0.004 |
| `_rap_r7_no_lck4` | present | absent | 24 (19) | 0.892 | 27 | 0.066 / 0.130 / 0.061 | -0.005 |
| `_rap_r7_only_lck4` | absent | present | 5 (5) | 0.130 | 0 | 0.052 / 0.049 / 0.044 | -0.008 |
| `_rap_r7_no_cas1` | absent | present | 5 (5) | 0.129 | 0 | 0.086 / 0.089 / 0.073 | -0.014 |
| `_rap_r6_dual` (baseline) | present | present | 26 (23) | 1.368 | 72 | 0.089 / 0.196 / 0.108 | +0.019 |

The consumer statistic is frozen in section 3. The amplitude is not monotonic in the
roster: LCK400IND without CAS100ATA nearly removes the episode, the two together double it.

**1.3 In the same epoch the network moves along the ambiguity and slant-ionosphere
direction.** 470 prior-residual events at 37 stations (9 the epoch before). The carried
ambiguities move by decimetres to a metre in one epoch, in band ratios near
(f1/f2)^2 = 1.647: MBAR00UGA G05 L1W -1.067 m and L2W -1.759 m (ratio 1.649). The satellite
code biases of the C1W family step by up to 0.15 m (G03 +0.150, G14 -0.140, G13 -0.128) and
relax by a factor of about five per epoch over three epochs; C1C and C2W step by 0.04 m or
less; before and after, every family moves by 0.002 m per epoch or less.

**1.4 The response.** The guard withdraws the temporal prior of every tripped satellite and
re-solves, so at this epoch no satellite keeps its clock prior. The largest de-commoned
wire clock excursion is 0.561 m and it does not reverse at the next epoch
(`scripts/ev6_event_read.py --section census`: reversal -6 %). All 26 satellites stay
loose for four epochs and re-couple together at TOW 278640. With the phase bias held
(card PBH) the wire's phase correction moves with the clock, and the consumer ignores the
marks on held keys by design.

**1.5 The consumer.** WTZZ00DEU 3D: 0.087 m at TOW 278490, 0.196 at 278520, 0.163, 0.146,
0.121, then 0.105 to 0.117 m to the end of the run (Up -0.082 m before, -0.110 m after).
The pre-fix baseline returns to 0.065 m within two epochs. The deficit of 70 epochs below
a decimetre between the two baselines is the 76 epochs from the event to the end of the
run sitting just above the threshold; the count is a threshold statistic here. Separately,
the corrected baseline is already 0.02 m higher BEFORE the event (0.089 against 0.070 m,
in Up). That difference is not this card's event and is recorded as an open read.

## 2. Questions (frozen)

**Trigger.** Which of the epoch's two identity events carries the kick, and through which
carried quantity?

- T-a: the departure of a satellite-signal column that hangs on one station (G30 L1W and
  C1W at VOIM00MDG), through whatever the departure re-realizes (the family's code datum
  rows, the satellite's pin reference, the parked carry, the phase-bias anchor).
- T-b: the reference-satellite reset of G19 at URUM00CHN, which refloats an anchor arc and
  forces a continuous handover.
- T-c: neither; a regional ionosphere or data event that the two rows merely accompany.

**Response.**

- R-a: the loss is inflicted by the same-epoch withdraw and re-solve when EVERY satellite
  is withdrawn (no temporal clock datum is left in the epoch).
- R-b: the loss is inflicted by the four-epoch loose era of all satellites together.
- R-c: the guard limits the damage; without it the trigger costs as much or more.

## 3. Instruments and arms (frozen)

All arms: base `runs/_rap_r6_dual/arm.json`, 6 h, exe `gnss_processor_rxp.exe`, about
five minutes each, `scripts/rapid_arm.py`. No C++ change.

**Tool change.** The `strip` op of `scripts/rapid_arm.py` gains `tow_from=` and `tow_to=`
selectors (GPS seconds of week, inclusive, either may be omitted), with a selftest case.
A subtraction in a time window is the field bed's equivalent of an injected event.

**Trigger arms.**

| tag | change | prediction under T-a | under T-b |
| --- | --- | --- | --- |
| `v1_g30_early` | strip G30 at VOIM00MDG from TOW 277200 | the all-satellite trip moves to 277200 (within one epoch); 278520 shrinks to the T-b share | unchanged at 278520 |
| `v2_g30_never` | strip G30 at VOIM00MDG for the whole run | no trip at 278520 beyond the T-b share | unchanged |
| `v3_g19_window` | strip G19 at URUM00CHN, TOW 278400 to 278700 | unchanged | the trip moves to 278400 or disappears |
| `v4_both` | `v1` and `v3` | nothing left at 278520 if T-a and T-b are the whole trigger | same |

**Response ladder.** These are discriminators. Following EV6 section 8.3, no gate value is
re-tuned to silence the instrument and none of these is an adoption candidate by itself.

| tag | change |
| --- | --- |
| `v5_resolve_off` | `network.clock_anchor_resolve: false` |
| `v6_k12` | `network.clock_anchor_k: 12` |
| `v7_k_inert` | `network.clock_anchor_k: 1000000` |
| `v8_recouple1` | `network.clock_anchor_recouple_epochs: 1` |

**Consumer statistic (frozen).** PRE = mean 3D over TOW 278220 to 278490 (10 epochs);
PEAK = maximum over 278520 to 278640; PLATEAU = mean over 278820 to 280770; cost =
PLATEAU minus PRE. Also reported for the record: 3D p50 by hour, epochs below a decimetre,
the clock wire (`scripts/ftc_step_attrib.py --r3`, both lines).

**Event table per arm.** Rows by tier at TOW 278460 to 278640; the trip count, pass split
and the sum of absolute innovations at the trip epoch (and at any new all-satellite epoch);
the slope and correlation of the innovation pattern against `_rap_r6_dual`; post-fit
refloats in 278520 to 278640; the largest de-commoned wire clock excursion and its reversal;
the C1W family's largest code-bias step.

**Census.** Any arm with cost at or below +0.005 m is replayed at the 16 census sites
(`runs/_msc/scripts/msc_add_wire.py`), graded by the paired per-site change against the
`r6` wire replays of (i) the mean 3D over the PLATEAU window and (ii) the mean hourly p50
over hours 2 to 6.

**Code read (file and line for each answer).**

1. What the row kind `obs_other` is, and what happens to G30 at VOIM00MDG at TOW 278520 in
   the measurement build (a signal-strength mask, a fallback signal, a gate).
2. What `READMIT park` parks when one code-bias column of a satellite departs while the
   satellite stays observed on its other family, and whether the satellite's clock carry
   or clock-anchor state is touched.
3. In `NetworkSbasis::buildCodeBiasDatumRows` and the carry install: what changes for a
   satellite and for its family when the column leaves (pin reference signal, ISC state,
   sum-to-zero membership, time link, reseed), and which prior rows are dropped or re-seeded.
4. The order of these steps inside the epoch relative to the first solve and the
   clock-anchor guard.
5. Estimator-free, from the raw file: whether URUM00CHN G19 has a real cycle slip at
   05:22:00 (time differences of the geometry-free and Melbourne-Wubbena combinations over
   ten epochs either side).

## 4. The reading (frozen before the runs)

- The trip following the G30 strip to TOW 277200 in `v1` confirms T-a. The build candidate
  is then the departure edge of a column that hangs on one station (invariant 7: the
  decoupling edge must bound what it releases), specified after the code read. Card SCC
  (every tracked band-1 code per station) is expected to remove the code half of that
  class, since `CB:G30:C1W` would no longer hang on one station; the phase half stays.
- `v3` quiet and `v1` not: T-b; the candidate is the reference-satellite reset on an anchor
  arc, with the raw-data check deciding whether the slip is real.
- Neither: T-c, and the next instrument is a raw geometry-free rate census by region.
- `v7` with cost at or below +0.005 m and no more than 21 refloats: the response inflicts
  the loss (R-a or R-b, separated by `v5` and `v8`), and the build candidate is a rule for
  a majority trip (a frame event is not 26 satellite faults). `v7` as bad or worse: R-c,
  and only the trigger is worth curing.
- Nothing is armed on WTZZ00DEU alone; the census must agree.

**Invariants pre-flight.** 1: the clock anchor is a stiff time-linked quantity, its jump
response is the decouple, and the re-coupling edge after the loose era re-seeds from a
value that the era may have displaced (measured here by PLATEAU). 5: wire quantities are
graded de-commoned. 6: not applicable (no code change; the tool change is covered by the
selftest). 7: both edges of the decouple are events today; the departure of a column has
a TOPOLOGY row and no bound on what it releases, which is this card's question.

## 5. Result (2026-09-21; Opus worker, every number below re-graded by the manager from `runs/`)

Exe `gnss_processor_rxp.exe`. Twelve new arms and the eight existing runs, all exit 0 with
720 emitted epochs; every `arm.json` differs from the base in the intended keys only.
Tools: `scripts/rapid_arm.py` (`tow_from=` / `tow_to=` on the `strip` op, selftest 31 of
31, a windowed strip never edits the header), `scripts/ev7_event_read.py` (reproduces the
table of section 1.2 exactly), `runs/_msc/scripts/msc_plateau.py`.

### 5.1 The arms

Trips, the sum of absolute innovations and refloats are at TOW 278520 (refloats over
278520 to 278640). Wire is the clock wire over hours 2 to 6, all satellites and
consumer-tracked.

| tag | the one change | trips (pass 0) | sum abs e (m) | refloats | PRE / PEAK / PLATEAU (m) | cost (m) | below 0.1 m | wire (m) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `r6_dual` | baseline | 26 (23) | 1.3679 | 72 | 0.0893 / 0.1955 / 0.1082 | +0.0189 | 533 | 0.0184 / 0.0107 |
| `v1_g30_early` | G30 stripped at VOIM00MDG from TOW 277200 | 0 | 0 | 0 | 0.0896 / 0.0878 / 0.0785 | -0.0111 | 593 | 0.0114 / 0.0064 |
| `v2_g30_never` | G30 stripped at VOIM00MDG, whole run | 0 | 0 | 0 | 0.0897 / 0.0880 / 0.0788 | -0.0110 | 593 | 0.0115 / 0.0064 |
| `v3_g19_window` | G19 stripped at URUM00CHN, 278400 to 278700 | 26 (23) | 1.3672 | 72 | 0.0892 / 0.1962 / 0.1085 | +0.0193 | 533 | 0.0180 / 0.0105 |
| `v4_both` | `v1` and `v3` | 0 | 0 | 0 | 0.0895 / 0.0876 / 0.0803 | -0.0092 | 593 | 0.0114 / 0.0062 |
| `x1_g30_b1w_early` (exploratory) | only L1W, C1W, D1W, S1W of G30 at VOIM00MDG from 277200 | 0 | 0 | 0 | 0.0896 / 0.0878 / 0.0785 | -0.0111 | 593 | 0.0114 / 0.0064 |
| `x2_g30_1ep` (exploratory) | G30 at VOIM00MDG absent for the one epoch 277200 | 26 | 1.3656 | 72 | 0.0890 / 0.1965 / 0.1101 | +0.0211 | 528 | 0.0187 / 0.0107 |
| `x3_g30_l1w_only` (exploratory) | only the L1W PHASE of G30 at VOIM00MDG from 277200 | 0 | 0 | 0 | 0.0895 / 0.0879 / 0.0787 | -0.0108 | 593 | 0.0114 / 0.0064 |
| `x4_cusv_g30_early` (exploratory) | G30 stripped at CUSV00THA from 277200 (null control) | 26 | 1.3677 | 72 | 0.0893 / 0.1956 / 0.1082 | +0.0188 | 533 | 0.0185 / 0.0107 |
| `v5_resolve_off` | `clock_anchor_resolve: false` | 23 (23) | 1.3142 | 35 | 0.0893 / 0.1476 / 0.1135 | +0.0242 | 516 | 0.0142 / 0.0093 |
| `v6_k12` | `clock_anchor_k: 12` | 22 (14) | 1.4166 | 70 | 0.0896 / 0.1957 / 0.1078 | +0.0182 | 534 | 0.0176 / 0.0100 |
| `v7_k_inert` | `clock_anchor_k: 1000000` | 0 | 0 | 21 | 0.0922 / 0.1252 / 0.0929 | +0.0007 | 566 | 0.0138 / 0.0082 |
| `v8_recouple1` | `clock_anchor_recouple_epochs: 1` | 26 (23) | 1.3091 | 66 | 0.0965 / 0.1963 / 0.1226 | +0.0261 | 481 | 0.0178 / 0.0091 |

No arm moves the all-satellite epoch to another time: in `v1`, `v2`, `v4`, `x1` and `x3` it
is gone.

### 5.2 The trigger: T-a confirmed in its PHASE half; T-b and T-c refuted

- `v1`, `v2` and `v4` remove the episode. `v3` leaves it intact to the fourth digit
  (innovation pattern slope 1.000, correlation 1.000).
- The raw file of URUM00CHN shows no slip on G19 at 05:22:00 (geometry-free step 0.0087 m
  inside a smooth ramp, Melbourne-Wubbena step -0.045 cycles, no loss-of-lock flag): the
  reference-satellite reset is a false alarm. It refloats an anchor arc and forces a
  continuous handover, and it costs nothing measurable here.
- `x3` strips only the L1W phase: `CB:G30:C1W` still departs at TOW 278520 and the epoch is
  silent. The phase column is the carrier. This agrees with `runs/_rap_r6_nofollow`, where
  the C1W code column is supported by many stations, only `PB:G30:L1W` is removed, and the
  episode is as large (26 trips, 1.496 m, 79 refloats).
- `x2`: a one-epoch absence of the family's only arc at a quiet time does nothing. `x4`: a
  third tracker that leaves by itself earlier is irrelevant.

### 5.3 The mechanism

Code read by the worker with file and line; the gate and the erase path re-read by the
manager.

The coverage gate `network.phase_datum_min_stations: 2`
(`src/core/filter/NetworkPhaseDatum.cpp`, the block "Coverage-threshold gating of the
anchored union") removes a (satellite, signal) phase-bias column when fewer than two
present stations track it. G30 at SGOC00LKA passes through the 15 degree elevation mask
between TOW 278490 (15.18 degrees) and 278520, the L1W family goes from two trackers to
one, and the column is removed. The signal-strength dip of section 1.1 is a coincidence:
the code has no signal-strength mask, and no per-satellite fallback signal either.

The gate's comment states its assumption: "A gated key's PHASE rows still serve the
satellite clock: each station's float ambiguity absorbs the bias". On the float vehicle the
surviving station's ambiguity is a CARRIED state. VOIM00MDG G30 L1W (age 420 epochs,
variance 2.8e-4 m^2) moves by +15.829 m in the epoch, with no refloat, no re-seed and no
variance inflation; its rows lose their shared column and are classified `obs_other`,
which is the `row_kinds` delta of section 1.1. A 15.8 m change of a row's model against a
1.7 cm prior is the kick. What the prior does not yield goes into the satellite's clock
and the station, and through them into the network (section 1.3). The amplitude follows
the value of the removed bias and the stiffness of the survivor, which is why it moves
with the roster and not monotonically.

When the survivor is the family's anchor arc (pinned, not carried), removing the column
re-births that arc and the edge is silent. That is what `v1`, `x1` and `x3` do at TOW
278250, where the column leaves with SGOC00LKA as the survivor (`event=birth`, age 0).

The return edge is consistent today: the family is re-born at a zero pin and every
station's carry of the family is erased through the re-reference set
(`networkApplyPhaseAnchorCarryReref`). Only the drop edge has no response.

The class in the baseline run, from `network_events.csv` (`row_kinds` edges that add
`obs_other`) and `network_amb_state.csv`: 13 edges in 6 h.

| TOW | satellite | survivor | jump of the carried L1 ambiguity (m) | variance before (m^2) | age (epochs) | clock trips | refloats, next 5 epochs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 261720 | G32 | KOKV00USA | none carried | | | 0 | 0 |
| 262560 | G16 | KOKV00USA | +6.398 | 6.9e-3 | 105 | 0 | 4 |
| 262980 | G04 | VOIM00MDG | +18.781 | 7.8e-3 | 119 | 0 | 3 |
| 263970 | G32 | KOKV00USA | +35.787 | 4.2e-4 | 68 | 1 | 0 |
| 268050 | G09 | VOIM00MDG | none (L2W only) | | | 0 | 1 |
| 268680 | G29 | STHL00GBR | +36.094 | 7.1e-3 | 38 | 8 | 5 |
| 268710 | G18 | BOGT00COL | none carried | | | 0 | 0 |
| 269400 | G27 | KOKV00USA | +7.123 | 2.2e-4 | 333 | 2 | 0 |
| 272700 | G08 | KOKV00USA | none carried | | | 0 | 0 |
| 272760 | G01 | PALM00ATA | -5.698 (L1C) | 2.0e-4 | 300 | 0 | 0 |
| 274170 | G07 | CUSV00THA | none (L2W only) | | | 0 | 0 |
| 278520 | G30 | VOIM00MDG | +15.829 | 2.8e-4 | 420 | 26 | 72 |
| 280440 | G21 | KOKV00USA | +14.329 | 2.1e-7 | 605 | 12 | 22 |

Every epoch of the run with eight or more `reason=nis` trips (268680, 278520, 280440) is
one of these edges, and so are the single trips at 263970 and 269400. Card EV6's step of
TOW 276810 on the older posture names the same column event (`PB:G23:L1W` removed).

Two side answers of the code read. `READMIT park` is triggered by one COLUMN leaving, parks
only the clock and code-bias carries of that column, and touches neither the satellite's
clock carry nor its clock-anchor state; a phase-bias column is never parked. The code
column has the same two-station rule (`ApplicationNetworkAssembly.cpp`), and a surviving
code row likewise continues without its column (a bias of 2.78 m in one code row here);
`x3` shows that this is silent at this event.

### 5.4 The response

`v7` (the guard inert for the whole run) meets the frozen criterion: 0 trips, 21 refloats
(the pre-fix run's count), cost +0.0007 m, clock wire 0.0138 / 0.0082 m. The guard's
response inflicts most of the consumer loss of this event, and R-c is excluded. `v5`
(+0.0242 m) and `v8` (+0.0261 m) are both worse than the baseline and `v6` equals it, so
neither the same-epoch re-solve nor the loose era is separately the damage, and no value of
an existing knob is a candidate. `v7` is not an adoption candidate: the guard is the jump
response of a stiff quantity (invariant 1). The trigger is fixed first; the guard is read
again on the fixed base, where it should rarely fire.

### 5.5 Census (16 sites, dual consumers replayed; manager's own recomputation)

| wire | mean 3D over TOW 278820 to 280770: better / worse | median change (m) | median across sites (m), `r6` 0.1710 | mean hourly p50, hours 2 to 6: better / worse | median change (m) |
| --- | --- | --- | --- | --- | --- |
| `v1` | 12 / 4 | -0.0164 | 0.1116 | 5 / 0 | -0.002 |
| `x3` | 12 / 4 | -0.0165 | 0.1116 | 5 / 0 | -0.002 |
| `v7` | 11 / 5 | -0.0214 | 0.1315 | 7 / 1 | -0.008 |

The episode costs the census far more than it costs WTZZ00DEU. On the `v1` wire the last
half hour goes from 3.05 to 0.04 m at ASCG00SHN, 1.62 to 0.13 m at SCRZ00BOL, 0.90 to
0.05 m at HARB00ZAF and 0.29 to 0.10 m at DAV100ATA. QUIN00USA stays at 83 m on the `v1`
and `x3` wires and falls to 0.68 m on the `v7` wire: its excursion is driven by another
clock-anchor declaration of the run, not by this episode (a lead for the consumer's
unbounded reaction, card MSC section 5). Code-bias coverage on the wire is unchanged by
every arm.

### 5.6 Corrections to sections 1, 2 and 4

- Section 1.1: the fall of `S1W` is not the gate. The column leaves because SGOC00LKA's
  G30 passes through the elevation mask and coverage falls from two stations to one.
- Section 2, T-a: nothing is re-realized at the departure; the defect is that nothing is.
  The code datum, the parked carry and the clock anchor are not involved.
- Section 4, first bullet: card SCC removes most single-station CODE columns; the code
  half carries nothing at this episode, so SCC does not cure it.

### 5.7 What stays open

- The corrected baseline is already 0.02 m higher at WTZZ00DEU before this episode. The
  earlier edges of the table (263970, 268680, 269400) are candidates; the fix arm decides.
- The guard's response on the fixed base, and the QUIN00USA lead.
- The false reference-satellite reset at URUM00CHN.
- Code rows that continue without their column.
- Tool detail: a record with every field blanked is written as the bare satellite id.

## 6. The fix, pre-registered (2026-09-21): the gate's drop edge erases the family's carries

**Knob.** `network.phase_datum_gate_drop_erase` (bool, default false).

**Requirements.**

- F1. The drop edge is a (satellite, signal) key that the coverage gate excludes in this
  epoch and that had an anchor incumbent from the previous epoch (the family was alive).
  At that edge the family is added to the epoch's re-reference set, so that
  `networkApplyPhaseAnchorCarryReref` erases every station's carried ambiguity of the
  family before assembly, exactly as at the return edge. The erase happens at the edge
  only, not at every gated epoch: the re-born ambiguity absorbs the bias once and is
  carried normally afterwards.
- F2. Both edges are events (invariant 7): a `PHASE_ANCHOR gate_drop` row (key, surviving
  stations, trackers before and now), the erased arcs in `network_amb_state.csv` through
  the existing capture with cause `gate_drop`, run-end counters for edges and erased arcs.
  With the knob OFF the same edges are counted as unhandled (telemetry only, solve
  untouched), so that an OFF run reports the size of the class.
- F3. Default OFF with byte identity. The edge predicate is a pure function with its own
  unit suite. Not fingerprinted (a forward-only response; no carried state changes
  meaning).

**Gates and bars (frozen).** Arms from `runs/_rap_r6_dual/arm.json` and
`runs/_rf0hv5_sim6/arm.json`, 6 h.

- G0. Knob absent: `products.txt` without `#` lines, `closed_loop_trace.csv` and
  `network_epoch_status.csv` identical to the two baselines.
- E1, field ON. At every gate-drop edge no carried ambiguity of the family moves by more
  than 0.5 m in the edge epoch (baseline: 9 of 13 edges, 5.7 to 36 m). No epoch with eight
  or more `reason=nis` clock trips (baseline 3).
- E2, WTZZ00DEU. Cost at TOW 278520 at most +0.005 m (baseline +0.0189); every hourly p50
  within 0.03 m of the baseline or better; clock wire at most 0.0184 / 0.0107 m.
- E3, census. Against the `r6` replays, the mean 3D over TOW 278820 to 280770 and the mean
  hourly p50 over hours 2 to 6 are each better at more sites than worse, with a median
  change at most 0.
- E4, sim ON. Every hourly p50 within 0.03 m of the sim baseline; the number of edges and
  the E1 jump check are reported.
- E5, only if E1 to E4 pass: the same reading at 12 h on both beds against 12 h OFF arms of
  the same exe.

**Reading.** G0 is a build gate. E1 to E5 passing gives a recommendation to arm the knob in
the working bases; the decision is the author's.

**Deferrals, triaged (invariant 3).** The exact alternative, an S-transformation that adds
the removed bias to the surviving carried ambiguity and keeps the arc's maturity, is not
built: field-inert if E1 to E3 pass with the erase (`v1` measures a re-born survivor as
silent). Code rows without a column: `x3` measures them silent at this episode; card SCC
removes most of them; re-read after SCC.

**Invariants pre-flight.** 1: this is the missing response of a stiff carried quantity to a
change of its definition. 7: the decoupling edge is bounded by the erase; the re-coupling
edge is the existing re-birth of the family. 4: counters in both knob states. 6: G0.

### 6.1 Amendment before any build or run (2026-09-21): the sim gate, and the size of the class

Measured on the two baselines after section 6 was frozen (`network_events.csv`):

| bed | edges where rows survive their column | clock-anchor trips (`reason=nis`), 6 h | of which in an edge epoch | epochs with 8 or more trips |
| --- | --- | --- | --- | --- |
| field `runs/_rap_r6_dual` | 13 | 50 | 49 | 3 |
| sim `runs/_rf0hv5_sim6` | 0 | 1 | 0 | 0 |

Nearly the whole clock-anchor trip population of the field bed is this defect, and the sim
bed cannot show it: every sim station tracks the same signals, so no family ever thins to
one tracker beside a mature carry. E4 as frozen would be vacuous. It is replaced by:

- E4a. Unmodified sim, knob ON: the three files of G0 identical to the sim baseline.
- E4b. The ingredient ADDED on the truth bed: five sim stations relabelled to the W family
  on band 1 (`relabel;from=?1C;to=?1W`, the station list chosen once so that the OFF arm
  shows at least eight edges with a carried survivor in 6 h, and recorded). Knob OFF must
  reproduce the class (edges with a carried jump above 0.5 m, clock trips in those epochs).
  Knob ON: no carried jump above 0.5 m at an edge, no epoch with eight or more trips, and
  every hourly consumer p50 within 0.03 m of the OFF arm or better.

### 6.2 The OFF arm of E4b, run before the build (2026-09-21, manager; exe `gnss_processor_rxp.exe`)

`runs/_rap_w5_sim_off`: the sim base with
`relabel;from=?1C;to=?1W;stations=S30000SIM,S22000SIM,S17000SIM,S38000SIM,S43000SIM`
(five neighbouring stations, 1 S to 25 N, 8 E to 48 E). The station list is fixed by this
run. The truth bed reproduces the class:

| TOW | satellite | survivor | jump of the carried ambiguity (m) | variance before (m^2) | age (epochs) | clock trips | refloats, next 5 epochs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 268170 | G14 | S38000SIM | L1W +16.168 | 3.2e-4 | 291 | 1 | 10 |
| 271500 | G22 | S38000SIM | L1W +7.335 | 2.2e-4 | 402 | 2 | 1 |
| 274830 | G30 | S17000SIM | L1W -4.967 | 5.2e-4 | 236 | 2 | 1 |
| 276600 | G25 | S43000SIM | L1W +2.152, L2W +20.082 | 6.9e-4, 1.8e-3 | 188 | 0 | 0 |

Six more edges have a survivor whose L1W arc is not carried (jumps of 0.1 m or less on
L2W). The run has 6 clock trips (baseline 1) and 12 post-fit refloats (baseline 0). The
consumer goes from 0.60 0.10 0.10 0.06 0.06 0.05 m (448 epochs below a decimetre) to
1.08 0.39 0.24 0.12 0.08 0.06 m (215). How much of that is the edge class and how much is
the thinner family itself is what the ON arm measures.

## 7. The fix: result (2026-09-21; Opus build worker, every number re-graded by the manager from `runs/`)

### 7.1 The build

Main `1935079`. `include/filter/NetworkPhaseGateDrop.hpp` holds the edge predicate (a key
gated in this epoch that had an anchor incumbent in the previous one); suite
`PhaseGateDropEdgeTest`, 6 cases; `gnss_tests` 2414 ran, 2413 passed, 1 known skip. The
gated key set is exported from `networkBuildPhaseDatumCandidates`; the election detects the
edge before the incumbent swap and, with the knob ON, adds the family to the re-reference
set, so the existing erase removes its carries before assembly. This posture runs the
anchor election, not the ensemble datum. Events `PHASE_ANCHOR gate_drop` (survivors,
trackers, previous anchor, handled) and the run-end readout exist in both knob states; the
erased arcs carry the cause `erase:gate_drop`. Exe `gnss_processor_ev7.exe`
(`8AEC0281...`). Loader parity check 0 gaps; golden verify PASS.

Two properties to know. The knob rides `network.phase_anchor_carry_reref` (armed in both
working bases; the loader warns when it is absent). The erase is the return edge's erase,
so it is whole-family: 17 arcs over the 13 edges at 6 h, including arcs at stations that
are not the survivor.

### 7.2 Gates

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knob absent, three files identical on both beds | identical (field and sim) | PASS |
| E1 | no carried jump above 0.5 m at an edge | 0 of 13 (OFF 8 of 13, 5.7 to 36.1 m) | PASS |
| E1 | no epoch with eight or more clock trips | 0 (OFF 3); trips in the run 50 to 1 | PASS |
| E2 | WTZZ00DEU cost at most +0.005 m | -0.0110 m (OFF +0.0189) | PASS |
| E2 | every hourly p50 within 0.03 m or better | largest +0.009 m (hour 4) | PASS |
| E2 | clock wire at most 0.0184 / 0.0107 m | 0.0091 / 0.0072 m | PASS |
| E3 | last half hour, more sites better than worse, median change at most 0 | 12 / 4, -0.0322 m | PASS |
| E3 | mean hourly p50, hours 2 to 6, same rule | 13 / 3, -0.019 m | PASS |
| E4a | unmodified sim, knob ON, identical | identical, 0 edges | PASS |
| E4b | relabelled sim: OFF reproduces, ON cures, consumer within 0.03 m of OFF | OFF identical to `w5_sim_off`; carried jumps 4 to 0; trips 6 to 1; refloats 12 to 0; largest hourly change +0.005 m | PASS |
| E5 | 12 h, same reading | section 7.4 | read delivered |

The count in section 6 ("9 of 13 edges") is corrected to 8 of 13: the table of section 5.3
lists eight carried L1 jumps.

Field 6 h, OFF against ON: 3D p50 by hour 0.48 0.05 0.05 0.08 0.09 0.10 against 0.48 0.05
0.06 0.09 0.09 0.09 m; epochs below a decimetre 533 against 577; post-fit refloats by hour
16 4 24 15 4 120 against 12 2 19 15 4 17; `CLK_ANCHOR` rows 274 against 127. At the edge
of TOW 278520 the carried VOIM00MDG G30 L1W arc is erased and re-born (-26.998 to +8.304 m,
age 0) and the L2W arc of the same satellite continues without a step (-0.3590 to
-0.3582 m; it moved by -2.17 m in the OFF run). The re-born value shows that the removed
bias was about 35 m: the stiff prior had let the arc absorb 15.8 m of it and pushed the
rest into the station and the network.

### 7.3 Census (16 sites, against the `r6` replays)

Median 3D p50 by hour 0.827 0.282 0.101 0.104 0.113 0.169 to 0.827 0.273 0.096 0.093 0.098
0.096 m; median count of epochs below a decimetre 206.5 to 235. Last half hour, per site:
QUIN00USA 83.4 to 0.61 m, ASCG00SHN 3.05 to 0.08, SCRZ00BOL 1.62 to 0.10, HARB00ZAF 0.90 to
0.02, DAV100ATA 0.29 to 0.10, CHTI00NZL 0.10 to 0.04; worse: ANMG00MYS by 0.039 m and three
sites by 0.004 m or less. QUIN00USA still has 240 epochs above 5 m after hour 2 (380
before); code-bias dropouts are unchanged at 50, as expected (this card does not touch the
code families).

### 7.4 Twelve hours

Field, OFF against ON: 25 edges; carried jumps above 0.5 m 11 to 0; clock trips 134 to 8;
epochs with eight or more trips 6 (268680, 278520, 280440, 288540, 294240, 299760) to 0;
post-fit refloats 528 to 219; clock wire 0.0305 / 0.0129 to 0.0131 / 0.0086 m; epochs below
a decimetre 863 to 875. Sim: OFF and ON identical.

WTZZ00DEU by hour (3D p50, m): OFF 0.48 0.05 0.05 0.08 0.09 0.10 0.12 0.04 0.07 0.08 0.18
0.18, ON 0.48 0.05 0.06 0.09 0.09 0.09 0.09 0.07 0.10 0.12 0.18 0.12. Hours 8 to 10 are
worse by 0.03 to 0.04 m and hours 6, 7 and 12 better by 0.02 to 0.06 m. The mean Up offset
explains both: in the OFF run it wanders from -0.10 m (hours 6 and 7) through +0.00 and
+0.06 m (hours 8 and 9) to -0.11 m (hour 11), so the run passes through zero after a kick;
in the ON run it holds -0.06 to -0.09 m from hour 4 to hour 10. The site's 3D error is its
standing Up offset, and a quieter wire shows as a steadier offset, not as a smaller one.

### 7.5 The guard and card SCC on the fixed base (discriminators)

| arm (all with the fix ON) | 3D p50 by hour (m) | below 0.1 m | cost (m) | clock wire (m) | trips | census, last half hour against `ev7fix` | census, hours 2 to 6 against `ev7fix` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `g1_field_on` | 0.48 0.05 0.06 0.09 0.09 0.09 | 577 | -0.0110 | 0.0091 / 0.0072 | 1 | | |
| `fx_k_inert` (guard inert) | 0.48 0.04 0.06 0.09 0.09 0.09 | 555 | -0.0109 | 0.0093 / 0.0072 | 0 | 12 / 4, -0.0001 m | 1 / 0, 0.000 m |
| `fx_s1` (card SCC, scale 1) | 0.40 0.07 0.07 0.11 0.11 0.10 | 316 | -0.0155 | 0.0092 / 0.0067 | 1 | 6 / 10, +0.0071 m | 6 / 3, -0.007 m |
| `fx_s10` (card SCC, scale 10) | 0.52 0.07 0.07 0.11 0.11 0.11 | 260 | -0.0103 | 0.0098 / 0.0069 | 1 | 5 / 11, +0.0025 m | 6 / 4, -0.006 m |

With the trigger fixed, the guard made inert changes nothing at WTZZ00DEU or across the
census: the response of section 5.4 only costs when it is given a kick, and no
response-side build is needed now. Card SCC on the fixed base is recorded in that card.

### 7.6 Reading and recommendation

Every frozen gate passes and the 12 h read supports it. Recommendation: arm
`network.phase_datum_gate_drop_erase` in `config/refound/{field,sim}_gps_dual_rf0hv.json`
(working bases only; the sim is byte-identical with it). The field baseline would become
`runs/_rap_g1_field_on`: 0.48 0.05 0.06 0.09 0.09 0.09 m, 577 epochs below a decimetre,
clock wire 0.0091 / 0.0072 m. The decision is the author's. A promotion-ledger row is owed
with the arming.

### 7.7 What stays open

- The standing Up offset of the consumers (WTZZ00DEU -0.08 m; on the `r6` wire the median
  over 15 census sites of the hours 4 to 6 Up medians is -0.033 m, 11 of them negative,
  QUIN00USA excluded): now the largest term at WTZZ00DEU, and what the count of epochs
  below a decimetre measures there.
- The exact alternative to the erase (an S-transformation that keeps the arc's maturity),
  and an erase scoped to the surviving station: not needed by any gate.
- The inert combination `phase_datum_held_basis` without `phase_datum_held_cold_release`
  (the driver clears the re-reference set) is not warned by the loader.
- The false reference-satellite reset at URUM00CHN; code rows that continue without their
  column (13 to 1 such edges with card SCC armed); QUIN00USA's remaining 240 epochs.
- Gross code rows enter the solve at full weight (BSHM00ISR G22 C1C ramps to 712 m over ten
  epochs from TOW 273390; SGOC00LKA has 262 code rows above 10 m in 6 h); the existing
  gross line-of-sight screen needs two gross rows on a line of sight and does not declare a
  single-signal ramp (config-only arm `runs/_rap_gl_on`: WTZZ00DEU unchanged). This belongs
  to the code-row QC of RF-3.

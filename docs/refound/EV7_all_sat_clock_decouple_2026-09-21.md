# EV7: the all-satellite clock-anchor decouple of 05:22 UT (TOW 278520)

**STATUS**

- **id:** EV7
- **topic:** clock
- **status:** open
- **summary:** Pre-registered 2026-09-21 (event card: one event, a 60-epoch window, five-minute one-change arms). At TOW 278520 every emitted satellite clock trips the Q3 clock-anchor gate in one epoch on every field run since RF-0hv; on the corrected baseline `runs/_rap_r6_dual` the consumer at WTZZ00DEU steps from 0.089 to 0.196 m and then holds a plateau of 0.108 m to the end of the run. The manager's read of existing runs fixes the trigger epoch's identities (one satellite-signal column loses its only station, one anchor arc is refloated) and shows the clock innovations are one fixed satellite pattern whose amplitude changes tenfold with the roster. Trigger arms, a response ladder and a code read are frozen below; nothing is built.
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

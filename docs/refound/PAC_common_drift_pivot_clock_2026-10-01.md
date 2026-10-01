# PAC: is the constellation-common drift the clock pivot's a priori clock?

**STATUS**

- **id:** PAC
- **topic:** network-vehicle
- **status:** closed
- **summary:** ANSWERED 2026-10-01 (sections 2, 4 and 5): the constellation-common drift is NOT the clock pivot's a priori clock. The pivot's own phase-visible clock moves with the fleet, the wire's D contrast stays flat while the clock contrast moves, and two hydrogen masers (PTBB, WTZR) carry the drift one for one; G01's a priori drift is bounded at about 3 cm over two hours. It is the pinned pivot's LEVEL converging: the pin holds only the pivot's clock column, so its code-determined clock/phase-bias split moves its phase biases and the phase data carry that to every satellite and receiver; it appears only on card CTU's posture, which frees the levels. On the field the unhealthy, sparsely tracked, code-misfit G01 makes it large; one 2 h arm with G14 pinned (24 to 29 stations) cuts the hour-2 drift from -0.180 to -0.063 m/h, the sim's size, consumer unchanged. The outside review's handover check is null (the drift is over by 04:42:30, and the emitted clocks cannot show it under card CLK-R's frame hold). Harmless to a float consumer; the pivot election (card HLT's health rule, a tracked-count rule, or the sum-to-zero datum) is the lever when the clock product is the target. Nothing armed.
- **source:** [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) sections 4 to 6; [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) section 5; card CLK-R (the emission frame hold); card HLT (the satellite health latch and the pivot); `NetworkSbasis.cpp` 466 to 501 (the pivot pin); the outside review's note of 2026-10-01
- **knobs / posture:** `network.clock_datum_satellite` on one arm only (the field's 2 h control `runs/_rap_wsx_ctl_f` with the pivot pinned on G14); nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** traces of `runs/_rap_rev1_blk6_{f,s}` and `runs/_rap_wsx_ctl_f`; the arm `runs/_rap_pac_g14_f` (`gnss_processor_wsx1.exe` `e3d6c4955054e1f9`); scripts `scripts/clock_level/pac_slope.py`, `pac_arm.py`

## 1. The two readings and what each predicts

The clock gauge pins one satellite's clock column per constellation, the pivot, at a correction of
zero (`NetworkSbasis.cpp` 500 to 501: `Pin{clockColumn(pivot), 0.0}`), elected sticky and lexically
first (G01 on both beds until it sets at 04:42:30, then G02). If the pivot's a priori clock carried an
error e(t), the phase data would either move every other clock (satellites and receivers) by -e and
leave the pivot's phase-visible clock still, or move the pivot's own phase biases by +e and nothing
else; any mix gives the pivot's motion and the fleet's opposite signs. If instead the network's
phase-visible time scale floats, every phase-visible quantity moves together against the pinned
code clock: every satellite's D including the pivot's, and every receiver clock.

## 2. The traces (card REV's 6 h arms, no run)

- **The handover check** (the outside review's): the slope of the fleet-common clock across the
  pivot handover at 04:42:30. The emitted clock cannot show it: card CLK-R's emission frame hold
  removes each constellation's per-epoch median from the emitted clocks (the fleet-common emitted
  slope reads -0.028 to -0.001 m/h in every hour on the field, -0.037 to 0.000 on the sim). In the
  solve frame (card FXA's frame-free common step of D) the slope is +0.033 m/h in the hour before
  and +0.012 m/h in the hour after (+0.001 and +0.007 over 30 minutes), within its hour-to-hour
  variation late in the run; the large drift sits in run hours 1 to 3 (+0.194, -0.196, -0.101 m/h on
  the field; +0.334 in hour 1 on the sim) and the handover falls in a quiet stretch. The check is
  null and has little power.
- **The pivot moves with the fleet.** Per run hour, the fleet's median D step summed against the
  pivot's own: field hour 2 -15.0 against -10.3 cm, hour 3 -12.5 against -13.2 cm; sim hour 1
  +15.5 against +21.5 cm. On 1 h differences the pivot's D tracks the fleet's with r +0.97 (slope
  0.75) on the field and +0.74 (0.45) on the sim. An a priori error of the pivot would put them in
  opposite directions.
- **The smoothest receiver clocks move with it** (field, from the first hour on, each clock's
  quadratic removed): PTBB00DEU and WTZR00DEU (hydrogen masers) against the linearly detrended
  drift r +0.96 and +0.91, slope 1.02 and 0.94; the seven smoothest stations' median r +0.985,
  slope 0.81; removing the drift takes PTBB's residual from 35 to 9 mm. The receiver clocks move
  with the satellites' D, which is the network time scale moving, not a satellite.
- **The field's pivot is a weak anchor.** G01 has no healthy broadcast record all day (the card HLT
  latch); it is tracked on L2W by 6.5, 6.1, 3.9, 2.6, 1.4 and 1.8 field stations per epoch by hour
  (the fewest of all satellites; G02 9.6 to 14.7; about 15 on the sim), and its ionosphere-free code
  post-fit residual averages -0.55, -0.81, -0.90, -0.42, -0.12 and -1.14 m by hour (the other
  satellites' median +0.006 to +0.041 m; the sim's within 0.135 m).

**Reading.** The common drift is not the pivot's a priori clock. It is the network's phase-visible
time scale: the pin fixes the pivot's clock column only, the phase-visible clock D of every satellite
and every receiver's phase-side clock are tied to that pinned code clock through the code rows, and
the whole phase-visible frame moves against it while those ties converge (on both beds, larger and
longer on the field). Because it moves every satellite and receiver alike, a float consumer absorbs
it (card SMC); it matters to the clock product and to every instrument that is not datum-invariant.

## 3. The arm, predicted before it ran

One 2 h field arm, `runs/_rap_wsx_ctl_f` with `network.clock_datum_satellite: G14` (tracked by 24 to
29 field stations at every epoch of the first 2 h, against G01's 4 to 5), the same exe
(`gnss_processor_wsx1.exe`). If the field's larger float is the weak, code-anomalous tie of an
unhealthy, sparsely tracked pivot:

- **P1.** The frame-free common D slope in run hour 2 at most half the control's in magnitude
  (control -0.196 m/h, bar 0.10).
- **P2.** The new pivot's ionosphere-free code residual in hours 1 and 2 within 0.1 m of zero, and
  its own D still moving with the fleet (the time scale still floats, more tightly tied).

If P1 fails, the float is a generic property of the pinned-code-clock datum on this posture, its
field size not set by G01, and the sum-to-zero clock datum (validated on the sim, its field arm owed)
is the candidate when the clock product is the target.

## 4. The arm and the re-grades (2026-10-01)

`runs/_rap_pac_g14_f` (`gnss_processor_wsx1.exe` `e3d6c4955054e1f9`; the S line reads `G:G14`, no
handover in 2 h) against `runs/_rap_wsx_ctl_f` (G01), both 2 h on the field:

| | control, pivot G01 | pivot G14 |
| --- | --- | --- |
| common D slope, hour 1 (ignition) | +0.210 m/h (sum +12.1 cm) | +0.174 m/h (sum -40.7 cm) |
| common D slope, hour 2 | -0.180 m/h (sum -15.3 cm) | -0.063 m/h (sum -5.6 cm) |
| the pivot's own D, hour 1 / 2 | -19.6 / -10.5 cm | -44.1 / -5.2 cm |
| the pivot's IF code residual, hour 1 / 2 (others' median) | -607 / -433 mm (+54 / +42) | +149 / +16 mm (+30 / +41) |
| IF code spread hour 2, trips, re-floats | 0.164 m, 0, 7 / 23 | 0.163 m, 0, 7 / 23 |
| WTZZ00DEU hours 1 / 2 | 0.109 / 0.022 m | 0.106 / 0.019 m |

**P1 holds**: the hour-2 drift falls to a third of the control's (bar a half), the sim's size (its
hour 2 +0.066 m/h). **P2 holds in hour 2 and fails in hour 1**: the new pivot's own D still moves with
the fleet (the time scale still floats on the pivot) and its code fits from hour 2 (+16 mm), but not
during ignition (+149 mm against the 0.1 m bar). Hour 1 is ignition under either pivot and larger
with G14.

The re-grades of section 2's two load-bearing numbers: the maser clocks against the drift depend on
the detrending, and only a matched one is meaningful (a quadratic on the clock against a line on the
drift strips the drift's curvature from one side: PTBB +0.46). Matched: PTBB00DEU r +0.99, slope
1.10 (linear on both) and +0.96, 1.01 (quadratic on both); WTZR00DEU +0.98 / 1.01 and +0.91 / 0.95;
BOR100POL, SVTL00RUS, STJO00CAN, CEBR00ESP +0.95 to +0.98 (linear). G01's carried L2W ambiguities on
the field number 5.5, 5.1, 2.9, 1.6, 1.0 and 3.7 per epoch by hour (`network_amb_state.csv`; section
2's 6.5 to 1.8 counted post-fit phase rows), G14's 24 to 29.

## 5. Reading and decision (2026-10-01)

**The common drift is not the pivot's a priori clock.** An a priori error of the pivot would move
the pivot-against-fleet phase-visible contrast and leave the pivot's D still; the pivot's D moves with
the fleet, the wire's D contrast stays flat while the clock contrast moves, and two hydrogen masers
carry the drift one for one. That bounds G01's a priori drift at about 3 cm over hours 1.25 to 3.25.

**It is the pinned pivot's level converging.** The pin holds the pivot's clock column at its a priori
(`Pin{clockColumn(pivot), 0.0}`), so the code-determined split of the pivot's clock and phase biases
can only move its phase biases; that moves its D, and the phase data carry the move to every
satellite's D and every receiver's phase-side clock. It is the clock-level convergence card CTU freed
(the exact update), seen through a pin that holds the code clock alone: the drift appears in every
arm on CTU's posture and in none on the control and golden postures, where the levels are frozen. Its
size is set by the pivot's code tie. The field's G01 is broadcast-unhealthy, carried by 1 to 5 field
stations and misfit by 0.4 to 0.6 m in its ionosphere-free code; a healthy pivot carried by 24 to 29
stations cuts the settled drift to the sim's size.

**What it reaches.** Every satellite and receiver moves alike, so a float consumer absorbs it (card
SMC; WTZZ00DEU unchanged here) and card CLK-R's emission frame hold removes it from the wire's clocks.
It reaches the clock product's own time scale and every instrument that is not datum-invariant (card
REV section 8, card FXA's raw structure).

**The decision.** Nothing is armed. When the clock product is the target, the pivot election is the
lever: a rule that elects a healthy, well-tracked pivot (card HLT's `network.sat_health` in its pivot
mode, built and not armed, elects G02 on DOY 129; a tracked-count rule would elect G14 or G22) or the
sum-to-zero clock datum (validated on the sim, its field arm owed), which fixes the fleet's mean
instead of one satellite's code. The election is the author's product decision.

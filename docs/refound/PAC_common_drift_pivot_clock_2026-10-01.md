# PAC: is the constellation-common drift the clock pivot's a priori clock?

**STATUS**

- **id:** PAC
- **topic:** network-vehicle
- **status:** open
- **summary:** Opened 2026-10-01 (the author's question after card FXA; the outside review asked for one check, the slope of the fleet-common clock across the pivot handover). Card FXA found the field's excess phase-visible motion common to the constellation and read it, unverified, as the pinned clock pivot's a priori clock error. The traces of card REV's 6 h arms answer the question without a run (section 2): the pivot's own phase-visible clock moves with the fleet, and so do the smoothest receiver clocks, which an a priori error of the pivot cannot produce; the drift is the network's phase-visible time scale, which floats on the pivot's phase-bias estimate because only the pivot's clock column is pinned. On the field that pivot (G01) is broadcast-unhealthy, tracked by 1 to 6 stations and misfit by 0.4 to 1.1 m in its ionosphere-free code. One 2 h field arm with a well-tracked pivot pinned (section 3, predicted before it ran) tests whether that weak tie is what lets the field's time scale float.
- **source:** [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) sections 4 to 6; [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md) section 5; card CLK-R (the emission frame hold); card HLT (the satellite health latch and the pivot); `NetworkSbasis.cpp` 466 to 501 (the pivot pin); the outside review's note of 2026-10-01
- **knobs / posture:** `network.clock_datum_satellite` on one arm only (the field's 2 h control `runs/_rap_wsx_ctl_f` with the pivot pinned on G14); nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** traces of `runs/_rap_rev1_blk6_{f,s}` and `runs/_rap_wsx_ctl_f`; the arm `runs/_rap_pac_g14_f` (section 4)

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

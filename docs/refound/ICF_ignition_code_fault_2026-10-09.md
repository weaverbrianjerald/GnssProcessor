# ICF: a receiver code fault in the cold start moves a satellite clock

**STATUS**

- **id:** ICF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered. On the field golden, a faulty L1 receiver channel at BSHM00ISR (G07, C1C/C1W/L1C drifting together by up to 340 m from the first epoch) moves the network's G07 clock by metres during the cold start; on the golden, prior-driven T4 refloats stop it at -3 m. No tier designed for it responds: the gross line-of-sight screen is held for its first 120 epochs, the Q3 guard decides on the phase statistic, the clock step mark is output-only. The card attributes the conduit first, then tests the responses.
- **source:** card [RPA](RPA_raim_pool_data_rows_2026-10-08.md) sections 4 and 5 (the event, its numbers and the read-only investigation); card [RF-3](RF-3_code_row_qc_2026-09-22.md) (the gross line-of-sight screen and its maturity horizon); card Q3 (`docs/cards/clock/`, the guarded anchor); card [CTU](CTU_coupling_time_update_2026-09-28.md) (the exact update that lets the clock level move on code); standing invariants 1 and 7
- **knobs / posture:** the goldens; candidate responses default off, one knob each, named in section 3
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** the event runs `runs/_rap_rpa_{off,on}_f6`, `runs/_rap_rpa1_{off,on}_f2`; gate A `runs/_rap_icfa_{off,on}_f2`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered. Gate A, first read (section 5): the right-hand-side dump cannot decide between the readings (its row class lumps the three carried-prior kinds, and a right-hand-side share is not a solution share); the per-row dump at BSHM00ISR shows the line-of-sight carry holding most of the tension, with the station-level carry taking part, and the existing refloat of that carry halting the runaway. The instrument is extended (kinds per row, solution share at named columns) before any response is built.

## 1. What is known (card RPA section 5, read-only)

- BSHM00ISR tracks G07 on a faulty L1 channel from the run's first epoch (C1C minus C2L -94.6 m at 259200, +245.4 m at 259620, rising 9 to 38 m per epoch; L1C minus C1C flat; clean after reacquisition at 259770). In the first epochs the line of sight has C1C and L1C only, with no redundancy; the station's G07 slant ionosphere absorbs the fault (-91 m at 259200).
- At 259380 to 259410 the receiver loses the W channels; the LLI-scoped response refloats L2W and keeps the faulty L1C carry (`NetworkCommonBandScope.cpp` around 251 to 265); from 259440 the line of sight is single-frequency.
- From 259410 the network's G07 clock moves about -1.6 m per epoch. All three of G07's phase biases move with it (the phase-visible clock is unchanged) and G07's C1W code bias absorbs most of the rest (-4.5 m), so the 9 stations on C1W see 0.3 sigma; the 18 stations on C1C see 7.4 sigma; BSHM00ISR's code row reaches 739 sigma. The other stations absorb the shift in their young float ambiguities and slant ionosphere.
- On the golden, T4 refloats triggered by BSHM00ISR's carried-prior rows (259470, 259530, 259590) stop it at -2.98 m and it recovers; card RPA's knob removes them and it reaches -11.67 m. The residue on G07's clock is -0.34 m after 6 h.
- After the first hour the same fault on G22 and G13 moves those clocks by less than 4 mm: the gross line-of-sight and admission screens respond once mature.
- No similar event on the golden outside the first epochs of a run (four days).

## 2. The question

Which state carries the fault into the shared clock, and which response, at which edge, stops it without the prior-row side effect card RPA measured? Two readings, pre-registered:

- **R-a (the carry conduit):** the carried state of BSHM00ISR's G07 line of sight (its ambiguity and slant ionosphere) holds the fault's history, and its prior pulls the shared states; erasing that carry when the code row is gross stops the runaway.
- **R-b (the soft direction):** BSHM00ISR's in-block receiver code bias (swinging tens of metres, in opposite directions between the two arms) is the conduit, and the decisive factor is how loose the G07 clock direction is at ignition (anchor square root of S 0.2 to 0.3 m against 0.033 m settled); a response must act on the code row, not on the carry.

## 3. Candidate responses (each default off, one knob, built only after the attribution read)

1. The gross line-of-sight screen declares in the cold start for a code row far beyond any plausible residual (an absolute bound for the first 120 epochs), so a 700-sigma row is withheld at once (`NetworkGrossLos.cpp`).
2. The Q3 guard tests persistence in clock space as well as the phase statistic: a clock-space statistic above k on consecutive epochs decouples the anchor (`NetworkClockAnchor.cpp`).
3. The LLI-scoped verdict keeps both bands' carries under review when the other band's code disagrees with its phase by more than a bound (`NetworkCommonBandScope.cpp`), or admits the station's second L2 signal so the line of sight stays two-band.

## 4. Gates (frozen)

- **A (attribution, first, no build).** A right-hand-side attribution dump (`network.rhs_attrib_tow`) at TOW 259500 and 259590 on the 2 h field golden in both arms of card RPA: the share of G07's clock column by contribution kind (card NOM2's kinds), naming the conduit. Decides between R-a and R-b before any build.
- **G0 (off identity)** per response, on both 2 h goldens.
- **G1 (the event).** On the field golden with card RPA's knob ON (so the prior-row refloats cannot mask the result): the G07 clock excursion over 259410 to 259650 at most 0.5 m, against 11.67 m.
- **G2 (the golden).** With card RPA's knob off: the 6 h field golden's WTZZ00DEU within 0.010 m in every hour, the clock wire not worse, the 16-site census paired change over hours 2 to 6 at or below 0; the sim golden within 0.010 m.
- **G3 (no false alarms).** The response's trigger count on the slip-free sim at 0 and on the field golden after the first hour within the existing screens' counts.

Arming is the author's decision.

<!-- Results are appended below this line under dated headings. -->

## 5. 2026-10-09: gate A, first read (not decisive; the instrument is extended)

Runs `runs/_rap_icfa_{off,on}_f2`: the 2 h field golden with `network.rhs_attrib_tow` 259500 and `rhs_attrib_n_ep` 4 (dumps at 259500, 259530, 259560, 259590), without and with card RPA's knob, exe `gnss_processor_icfa.exe` (main `3c097ca`, SHA256 `9581055644bdd0ec...`). `products.txt` is byte-identical to `runs/_rap_rpa1_{off,on}_f2` on both arms, so the event is the one card RPA measured; the attribution closes at 9e-12 to 6e-11 on every dump.

**What the dump can say.** The class totals of the reduced right-hand side at `CLK:G07` are sums of order 1e6 that cancel (at 259590 on the off arm: coupling and carried-prior rows +2.74e6, observation phase -2.59e6, observation code -1.26e5, the dense border carry -7.8e5, the reduced total -9.24e5), and the two arms differ by less than 0.3 % in every class. The dump groups rows by the row classifier's class, and the class `cpl_row` holds the six carried-prior block kinds together (ambiguity, ionosphere and receiver code bias), which are exactly the two readings; the per-station split is written only at the C00 columns, which do not exist on this posture. A right-hand-side share is also not a solution share: the clock moves by `N^-1 b`. Gate A as frozen cannot decide between R-a and R-b.

**What the per-row dump shows at BSHM00ISR, G07** (prefit `y`, whitened rows have unit weight):

| row | off 259500 | off 259590 | on 259500 | on 259590 |
| --- | --- | --- | --- | --- |
| code C1C (weight 4.7, sigma 0.46 m) | 102 m | 207 m | 102 m | 207 m |
| phase L1C | 0 | 37 m | 106 m | 211 m |
| carried-prior row of the G07 line of sight | -8.6 | 308 | 134 | 453 |
| carried-prior rows with no satellite (station-level) | -5.4, -2.1 | -32.7, -2.1 | -69.7, 0.5 | -175.5, -0.8 |

The faulty code row's prefit residual doubles over the window on both arms. On the off arm the isolated-arc refloat at 259470 erased the line of sight's carry (its row reads -8.6 at 259500, the phase residual 0), the carry builds back to 308 by 259590, and the refloat fires again. On the on arm nothing erases it: the line-of-sight carry row reaches 453 and the station-level rows reach -176. Both carries hold tension; the line-of-sight carry holds more, and erasing it is what halts the runaway on the golden. This leans to R-a with R-b taking part, and does not meet gate A's bar (a share of the clock column by kind).

**Instrument extension A' (diagnostic only, the solve byte-identical; built before any response).** (1) The row dump writes each row's contribution kind (card NOM2's N2a tag) beside its class. (2) A knob `network.rhs_attrib_columns` (a list of border labels, e.g. `CLK:G07`) writes, for every (station, kind) pair, the right-hand-side contribution at each named column and its solution share `(N_red^-1 b)` at that column under the solve's own datum, with the closure of the shares against the solved increment. Gate A then reads the solution share of `CLK:G07` by (station, kind) at the four epochs on both arms and names the conduit: R-a if BSHM00ISR's line-of-sight carry kinds dominate the increment, R-b if its receiver code-bias carry does.

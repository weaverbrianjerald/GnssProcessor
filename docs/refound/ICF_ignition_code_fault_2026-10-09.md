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

**Outcome.** Pre-registered. Gate A, first read (section 5): the right-hand-side dump cannot decide between the readings (its row class lumps the three carried-prior kinds, and a right-hand-side share is not a solution share); the per-row dump at BSHM00ISR shows the line-of-sight carry holding most of the tension, with the station-level carry taking part, and the existing refloat of that carry halting the runaway. The instrument is extended (kinds per row, solution share at named columns) before any response is built. Gate A decided (section 6): R-a. The faulty line of sight puts about -2.2 m per epoch into G07's clock through its code and phase rows, made redundant by its carried ambiguity, and the border carry keeps it; the receiver code-bias carry opposes the move. Candidate response 1 is built first.

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

## 6. 2026-10-09: gate A decided (R-a, through the operator)

Instrument A' landed on main (`086d595`): the row dump names each row's contribution kind, and `network.rhs_attrib_columns` writes `cols_<tow>.csv`, the solution share of every (station, kind) and every (station, kind, satellite, signal) at the named columns under the solve's own datum (the estimator's pinned and rescaled system, factorized as `solveSharedConstrained` does). Default empty, diagnostics only. Runs `runs/_rap_icfa2_{off,on}_f2` (exe `gnss_processor_icfa2.exe`, `4d929358...`): `products.txt` byte-identical to `_rap_icfa_{off,on}_f2`; the recomputed solution equals the solver's `x_s` at 10 significant digits in all 40 (dump, column) pairs; the shares close on `CLK:G07` to 1.6e-6 to 6.2e-6 m (parts reach 99 m). Unit suite 2 966 run, 2 942 passed, 24 skipped, 0 failed. The solved quantity is G07's clock correction against the a priori precise product (the shared columns are linearized at the product every epoch), so a share is a part of the estimate, and the history enters through the dense border carry.

**The solution share of `CLK:G07`, in metres** (TOW 259500, 259530, 259560, 259590; re-graded from the `cols_` files):

| part | on arm (RPA knob on, no refloat) | off arm (the golden) |
| --- | --- | --- |
| the estimate `x_s` | -4.942, -6.637, -8.261, -9.901 | -3.072, -2.900, -2.675, -2.505 |
| BSHM00ISR, all kinds | -1.964, -2.109, -2.204, -2.273 | -0.061, -0.038, -0.019, -0.012 |
| its G07 code row C1C | -1.130 at 259500, -1.400 at 259590 | -0.001, -0.004 |
| its G07 phase row L1C | -1.174, -1.426 | 0.000, -0.001 |
| its G07 carried ambiguity row | +0.156, +0.297 | -0.002, +0.021 |
| its receiver code-bias carry rows | +0.03 to +0.04 (all four) | at most 2e-4 |
| the dense border carry | -9.735, -9.729, -10.354, -11.208 | -9.770, -8.061, -6.950, -6.068 |
| the other 57 stations | +6.758, +5.200, +4.297, +3.581 | +6.758, +5.198, +4.293, +3.574 |

**Reading.** The arms differ only at BSHM00ISR and in the border carry (the other stations agree to 7 mm). On the on arm, BSHM00ISR's G07 line of sight puts about -2.2 to -2.5 m into the estimate every epoch, split about equally between its code row and its phase row, and the border carry keeps most of each epoch's injection (its arm gap grows by 0.79 to 0.89 of BSHM00ISR's previous share each epoch): that is the ratchet. On the off arm, after the refloat at 259470 erased the line of sight's carried ambiguity, the same line of sight with the same faulty prefit contributes about 0, and the border relaxes. The receiver code-bias carry contributes +0.03 to +0.04 m, against the move. **R-b is not supported; R-a is, with a correction to its wording:** the carried ambiguity's own row carries a small share that opposes the move (+0.16 to +0.30 m); the carry acts through the operator. The fault moves C1C and L1C together (card RPA section 5: L1C minus C1C flat). On a single-frequency line of sight with the slant ionosphere free, the ionosphere absorbs the code-minus-phase part, and with the ambiguity held by its carry the sum of code and phase is redundant: it measures the shared states, the fault twice. With the ambiguity free, the line of sight has no redundancy and says nothing about the shared states. Card section 2's "the carried state holds the fault's history and its prior pulls the shared states" is therefore replaced by: the carried ambiguity makes the faulty line of sight informative about the shared states, and the border carry accumulates what it injects.

**What this means for the responses.** A response must take the line of sight's redundancy away while it is faulty: withhold its rows, or free (refloat) its carried ambiguity. Candidate 1 (the gross line-of-sight screen, an absolute cold-start bound) does that if the screen's action covers the line of sight's phase row, or if the line of sight has no second band (as here from 259440); candidate 3 does not trigger on this event, since code and phase agree on the faulty band and the line of sight is single-frequency; candidate 2 acts on the clock after the injection, where the border carry has already kept it. Candidate 1 is built first.

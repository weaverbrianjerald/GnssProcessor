# ICF: a receiver code fault in the cold start moves a satellite clock

**STATUS**

- **id:** ICF
- **topic:** network-vehicle
- **status:** closed
- **summary:** KEEP_OFF, FAILED G2's clock-wire clause. A faulty L1 receiver channel at BSHM00ISR (G07, code and phase drifting together by up to 340 m) moves the network's G07 clock in the cold start; on the golden only the carried-prior rows' refloats stop it, at -3 m (card RPA). Gate A (instrument A', `network.rhs_attrib_columns`): the faulty line of sight, made redundant by its carried ambiguity, injects about -2.2 m per epoch and the border carry keeps it (reading R-a). Response 1, the gross line-of-sight screen's cold-start bound (`network.gross_los_cold_bound_m`, 12 m), holds G07 within 0.03 m and passes G0, G1, G3 and the consumer, sim and census clauses of G2; the hours 2 to 6 clock-wire median is 12 % / 6 % worse (three near-median arcs, not noise by a sham control).
- **source:** card [RPA](RPA_raim_pool_data_rows_2026-10-08.md) sections 4 and 5 (the event, its numbers and the read-only investigation); card [RF-3](RF-3_code_row_qc_2026-09-22.md) (the gross line-of-sight screen and its maturity horizon); card Q3 (`docs/cards/clock/`, the guarded anchor); card [CTU](CTU_coupling_time_update_2026-09-28.md) (the exact update that lets the clock level move on code); standing invariants 1 and 7
- **knobs / posture:** the goldens; `network.rhs_attrib_columns` (A', diagnostics only, `086d595`); `network.gross_los_cold_bound_m` (response 1, default off, `7cca805`)
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** the event runs `runs/_rap_rpa_{off,on}_f6`, `runs/_rap_rpa1_{off,on}_f2`; gate A `runs/_rap_icfa_{off,on}_f2`, `runs/_rap_icfa2_{off,on}_f2`; response 1 `runs/_rap_icfr1_*`, sham controls `runs/_rap_icfs_sham{1,2,3}_f6`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: which state carries a faulty receiver channel into a shared satellite clock during the cold start, and which response stops it? Gate A (section 6): the faulty line of sight's carried ambiguity makes it redundant, so its code and phase rows inject about -2.2 m per epoch into G07's clock and the border carry keeps it; the receiver code-bias carry is not the conduit. Response 1 (section 7), a 12 m cold-start bound on the gross line-of-sight screen, holds G07 within 0.03 m against 11.7 m, but the hours 2 to 6 clock-wire median is 12 % worse (three near-median arcs, a real effect by a sham control): FAILED G2, KEEP_OFF as frozen; the trade is the author's.

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

## 7. 2026-10-09: response 1, the cold-start bound (FAILED G2's clock-wire clause; KEEP_OFF)

**What was built** (`7cca805`, landed default off; exe `gnss_processor_icfr1.exe`). `network.gross_los_cold_bound_m` (metres, 0 = off; resolved off without `network.gross_los_screen` and under `network.gross_los_bound`; not fingerprinted). Inside the screen's maturity horizon, a line of sight whose largest post-fit code residual exceeds the bound is declared at once, with the screen's own response: its carried ambiguities and per-line-of-sight carries are erased and its code rows are folded at 100 sigma until release. Three additions: a standing cold declaration whose bound is exceeded again erases the carried ambiguity again (a re-stashed ambiguity would make the line of sight redundant through its phase rows); the erase also sweeps a carried ambiguity of a signal absent this epoch; every exit (the screen's release test, an expiry, a satellite stand-down) erases once more, so the line of sight reconnects on a fresh ambiguity (invariant 7's re-coupling edge). A station guard, added in the build and not pre-registered, holds the cold declarations of a station when most of its lines of sight are over the bound in one epoch (a station fault, where folding every code row would remove its clock). Each edge is a `GROSSLOS` event with a counter; the run-end readout gives the counts, the bound's margin on the run, and an INERT clause.

**The bound, chosen before G1.** The statistic is the largest post-fit code residual of the line of sight in metres (the prefit residual is unusable: on clean rows of the golden it reaches 232 km, the receiver clock's linearization offset). Clean population over the first 120 epochs: field maximum 6.70 m (BSHM00ISR G09 at 259470, collateral of the same fault; 3.43 m without that station), 99.9th percentile 2.18 m; sim maximum 4.88 m, 2.91 m. The faulty line of sight: 0 from 259200 to 259350 (no redundancy), 2.5 m at 259380, 24.2 m at 259410 (its first visible epoch), then 66 m and 109 m. Bound 12 m: the geometric mean of 6.70 and 24.2, rounded down.

**Gates** (re-graded by the manager: G1's C records, WTZZ00DEU, the clock wire):

| gate | result | verdict |
| --- | --- | --- |
| G0 | knob off on both 2 h goldens: products, closed-loop trace and epoch status identical to the A' build, `products.txt` byte-identical, CSVs differing in wall-clock columns only | PASS |
| G1 | with card RPA's knob on, G07's emitted clock record over 259410 to 259650 stays within 0.032 m (the table below), against -11.674 m without the bound; the change from 259380 is 0.444 m, the 259410 epoch's own move, before the first declaration | PASS |
| G2, WTZZ00DEU 6 h | 0.113 0.023 0.016 0.020 0.030 0.026 m by hour against 0.108 0.024 0.016 0.020 0.030 0.026 (largest change +0.005 m, hour 1) | PASS |
| G2, sim 6 h | identical to the reference (0 triggers) | PASS |
| G2, census, hours 2 to 6 | median paired change -0.0000 m; ARHT00ATA 0.039 m better (card RPA's G07 residue site), QUIN 0.014 m and KOKB 0.011 m worse, neither tracking G07 (their replays with G07 excluded are identical) | PASS |
| G2, clock wire, hours 2 to 6 | `ftc_step_attrib.py --r3`, `e_clk`, de-commoned p50: 0.0216 / 0.0186 to 0.0242 / 0.0198 m all / tracked (+12 / +6 %) | **FAIL** (frozen: not worse) |
| G3 | sim 0 triggers (INERT clause printed); field: 1 declaration at 259410, re-erases at 259470, 259530, 259590 and 259770, the release at 259830, all inside the horizon and 0 after it; the sigma-bar screen's counts after the horizon unchanged (4 declared, 2 released, 2 expired); T4 isolated-arc refloats 26 to 18 | PASS |

Unit suite 2 976 run, 2 952 passed, 24 skipped, 0 failed (10 new cases, `tests/test_network_gross_los_cold_bound.cpp`).

G07's emitted clock record (m):

| TOW | golden | RPA knob on | RPA knob on, bound on | golden, bound on |
| --- | --- | --- | --- | --- |
| 259380 | 0.440 | 0.440 | 0.440 | 0.440 |
| 259410 | -0.004 | -0.004 | -0.004 | -0.004 |
| 259440 | -1.364 | -1.364 | -0.001 | -0.001 |
| 259470 | -2.981 | -2.981 | 0.018 | 0.018 |
| 259530 | -2.558 | -6.465 | 0.022 | 0.022 |
| 259590 | -2.208 | -9.958 | 0.027 | 0.027 |
| 259620 | -2.071 | -11.674 | 0.022 | 0.022 |
| 259650 | -1.934 | -11.009 | 0.030 | 0.030 |

With the bound on, the arm with card RPA's knob and the golden are identical through the window: the bound replaces the prior-row refloats as the check of this event.

**The failed clause, read.** The rest of the clock-wire distribution improves: the hours 2 to 6 90th percentile 0.0633 to 0.0584 m, the all-hours p50 0.0583 to 0.0428 m, the all-hours maximum 0.427 to 0.103 m, G07's own arc 0.0659 to 0.0200 m. Paired per arc over 36 arcs, the median change is -0.0001 m and the mean -0.0013 m (8 better, 10 worse at 1 mm). The p50 rise comes from three arcs near the median: G05 +0.0097, G21 +0.0059, G13 +0.0053 m. A sham control measured whether the statistic is noise under a comparable perturbation: three clean lines of sight withheld for the same window on the 6 h golden (`rapid_arm.py` windowed strips, 259410 to 259830, of DJIG00DJI G07, URUM00CHN G07 and CEBR00ESP G13; `runs/_rap_icfs_sham{1,2,3}_f6`) give hours 2 to 6 p50 0.0217, 0.0216, 0.0216 / 0.0186 m. The statistic is stable under a one-line-of-sight withhold in the cold start, so the rise is the response's own consequence: removing G07's early excursion (the golden's refloats left it at -3 m and the border carry kept it) changes how the other satellites' clock levels settle, with three near-median arcs settling 5 to 10 mm worse.

**Verdict.** Response 1 removes the defect the card is about (the cold-start runaway, 0.03 m against 3 m on the golden and 11.7 m without the prior-row refloats) and no longer depends on the prior rows to stop it; it fails the frozen G2 clause on the hours 2 to 6 clock-wire median (+12 / +6 %). **KEEP_OFF as frozen.** The trade for the author: the cold start and the run's worst clock arc against three near-median arcs 5 to 10 mm worse in hours 2 to 6. Candidates 2 and 3 are not built (section 6: candidate 3 does not trigger on this event; candidate 2 acts after the border carry has kept the injection). The card closes.

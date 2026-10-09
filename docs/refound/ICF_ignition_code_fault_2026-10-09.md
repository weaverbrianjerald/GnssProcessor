# ICF: a receiver code fault in the cold start moves a satellite clock

**STATUS**

- **id:** ICF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered. On the field golden, a faulty L1 receiver channel at BSHM00ISR (G07, C1C/C1W/L1C drifting together by up to 340 m from the first epoch) moves the network's G07 clock by metres during the cold start; on the golden, prior-driven T4 refloats stop it at -3 m. No tier designed for it responds: the gross line-of-sight screen is held for its first 120 epochs, the Q3 guard decides on the phase statistic, the clock step mark is output-only. The card attributes the conduit first, then tests the responses.
- **source:** card [RPA](RPA_raim_pool_data_rows_2026-10-08.md) sections 4 and 5 (the event, its numbers and the read-only investigation); card [RF-3](RF-3_code_row_qc_2026-09-22.md) (the gross line-of-sight screen and its maturity horizon); card Q3 (`docs/cards/clock/`, the guarded anchor); card [CTU](CTU_coupling_time_update_2026-09-28.md) (the exact update that lets the clock level move on code); standing invariants 1 and 7
- **knobs / posture:** the goldens; candidate responses default off, one knob each, named in section 3
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** the event runs `runs/_rap_rpa_{off,on}_f6`, `runs/_rap_rpa1_{off,on}_f2`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered; no result yet.

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

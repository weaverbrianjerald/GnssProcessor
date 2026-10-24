# ALD: an anchor station's level held as part of the phase datum (L5 return plan step R-1d, continued)

**STATUS**

- **id:** ALD
- **topic:** network-vehicle
- **status:** open
- **summary:** Derived and checked offline 2026-10-23; not built. Successor of card R1D for step R-1d. An anchor station's ambiguity pins define the phase biases of the satellites it anchors, and its data-null level directions are transported into those phase biases: with the pins held, the station's level plus a matching shift of the anchored phase biases is invisible to its data (residual 2.8e-11 on MAJU00MHL's block) and is held only by its carried coupling, conditionally on the shared state. The card derives a hold of an anchor's level directions as part of the phase datum (a local pin row along each level direction, continuous through handovers), checks its premises on MAJU00MHL's dumps, and states the gates for a build. L5 SEQUENCED.
- **source:** [R1D](R1D_station_level_datum_2026-10-23.md) sections 7, 9, 13 and 15 (the coupling read, the whole-run anchor bar that removes the event, the coupling projections that fail, the re-seed handover that fails); the author's direction of 2026-10-23
- **knobs / posture:** none built; proposed `network.phase_datum_anchor_level_hold` (section 5)
- **dates in the text:** 2026-10-23
- **hashes / run dirs:** block dumps `runs/_rap_l5d_loc_blk/logs/rhs_attrib/blk_260010_MAJU00MHL.csv` and `anchored/blk_260040_MAJU00MHL.csv` (branch `card/l5b-diag`, commit `92785d4`); offline scripts in the session scratchpad (`anchor_check.py`, `blk_read.py`)
- **gates (frozen at pre-registration):** section 5

**Outcome.** Question: can the anchor loop of card R1D be closed in the datum itself, by holding an anchor station's level directions as the pins hold its ambiguities? The derivation says the anchor's level is a near-null direction of the network that the pins hand to the anchored phase biases, closed today only by the station's carried coupling (conditional on the shared state) and a DCB prior that holds nothing along it. MAJU00MHL's dumps confirm the premises: the level does not touch the clock gauge, the transport is exact, and the event's phase-bias changes follow its pattern. Not built; the build is the author's decision.

## 1. The question

Card R1D found that the field event of card L5B is transmitted by the phase-datum anchor role: with MAJU00MHL barred from anchoring for the whole run the event vanishes and the three-band wire is as clean as the dual one (R1D section 9). Cutting the coupling of the station's carried columns either broke the gauge or cost the dual network 40 % of its clock wire (sections 11 and 13), and moving the role away at a re-seed came too late (section 15). What remains is the datum itself: what the anchor contributes to the phase-bias datum besides its pinned ambiguities.

## 2. The datum as built (code read)

- **Pins.** The anchor election (`networkElectPhaseAnchors`, `src/core/network/solve/PhaseDatum.cpp`) pins, for each (satellite, signal) family, the anchor station's ambiguity with a row of weight about 1e10 (`ContributionKind::AmbiguityPin`), which realizes the family's phase bias.
- **Held basis.** Under `network.phase_datum_held_basis` (armed) a retained incumbent re-pins at the value frozen at its election (the basis ledger; `n_value_drift` must read 0).
- **Continuous handover.** A successor is pinned at its own ledger estimate when that estimate passes the variance screens, so the family does not step; "each continuous handover extends the same original frame". A refused estimate takes the cold path (pin at 0, a declared step).
- **The declared null space.** The S-basis declares one clock-gauge direction per constellation (dimension 2 on the goldens); the phase-bias gauge is removed by the pins.
- **The station level.** Each station's data-only local normals have one null direction per constellation (R1D section 1), closed by the zero-mean receiver DCB prior (weak along it: DCB share 0.0074 on MAJU00MHL's GPS level) and by the coupling rows of the station's carried columns, which tie it to the shared state (R1D section 7).

## 3. Derivation

**Transport.** Let `c` be an anchor station's level direction for one constellation (unit, data-null for its rows at fixed shared state) and `P` its pinned ambiguity columns. Moving the station along `c` would move the pinned ambiguities, which the pins forbid; but the joint direction

`v = (c with its entries on P set to 0;  on each anchored phase-bias column PB_j, the shift that keeps the pinned rows unchanged)`

is invisible to the station's data rows: the pinned rows see the phase-bias shift in place of the ambiguity move, and the other rows are untouched because `c` is data-null. At every other station the phase-bias shift is absorbed by its free ambiguities of those satellites, so `v` extends to a direction invisible to all data. It is not declared (it carries no clock-gauge component, below), so it is closed by priors alone: the anchor's DCB prior (negligible along it) and the coupling rows of the anchor's carried columns, which hold it only relative to the shared state. A push on the anchor's level is therefore a push on the phase biases it defines, and a phase-bias move is a push on its level: the loop of card R1D.

**The hold.** While a station anchors at least one family of a constellation, add one local pin row per such constellation along its level direction:

`a_r = c_t restricted to the columns that carry a previous posterior,  a_s = 0,  y = a_r^T (x_prev - x_lin),  w = the pins' weight`,

where `c_t` is this epoch's level direction from the station's data rows (the predicate and threshold of `stationLevelDirection`), `x_prev` the previous epoch's posterior of those columns (the carried values), and `x_lin` this epoch's linearization point. The row holds the station's level at its carried value; new columns (a returning line of sight) are free in it. With the pins it closes `v` exactly, so the anchored phase biases cannot inherit a move of the anchor's level. At a handover, the successor's hold starts at its own carried level (continuous, as its pin value is), and the outgoing station's hold ends when it anchors no family of the constellation.

**What the hold does not touch.**

- *The clock gauges.* `c` carries no receiver clock, inter-system bias or wet-delay loading (section 4), so the hold observes no declared direction; the rank gate's declared dimension is unchanged.
- *The phase-bias gauge.* Already removed by the pins; the hold constrains `c` restricted to the unpinned columns, a direction independent of the pins.
- *The data.* `c` is data-null, so the hold adds no information on any quantity the data determine; it chooses the value of an inestimable combination, as the pins do.
- *The coupling algebra.* Unlike the projections of R1D sections 10 to 13, the carried rows are unchanged.

**Scope.** (H1) the anchors only, during tenure, is the minimal form. R1D section 15 is a caution: MAJU00MHL handed every family over by TOW 259980 and the event still came at 260040, so a station can transmit after it leaves the role (by its carried coupling to the phase biases it defined). (H2) every station, every epoch, completes the minimum constraints for all station levels and covers that case; it replaces the DCB prior's weak closure along `c` everywhere. H1 is the first build; H2 is its fallback, graded the same way.

## 4. Offline check on MAJU00MHL's dumps (session script `anchor_check.py`)

At TOW 260010 MAJU00MHL pins G01 (L1C, L2W), G10 (L1C, L2W, L5Q), G16 (L1C, L2W), G26 (L1C, L2W, L5Q) and G27 (L1C, L2W, L5Q).

| premise | measured | verdict |
| --- | --- | --- |
| `c` has no clock-gauge loading | `c[CLK]` 0, `c[ISB_Galileo]` 1.7e-11, `c[ZWD]` 1.2e-11 | holds |
| transport: `v` is invisible to the station's data rows | max over 60 data rows 2.8e-11 (the pinned columns alone would leave 0.24); phase-bias shifts per unit `v`: L1C +0.133, L2W +0.220, L5Q +0.239 | holds exactly |
| the station's own block holds `v` | `v^T N v` = 110 per unit `v` (sigma about 0.10 m): carried ambiguity coupling 109, carried receiver code biases 0.28, DCB prior 7e-5, data 0 | only the carried coupling, conditionally on the shared state |
| the event follows `v` | level coordinate 260010 to the anchored 260040: -21.92 m; the 13 anchored phase biases all move the predicted way, with the band pattern of `v` (L1C -3.1 to -4.9 m, prediction -2.9; L2W -8.6 to -10.8, prediction -4.8; L5Q -6.0 to -11.0, prediction -5.2; G27 L1C -0.2) | the pattern holds; about twice the size on L2W and L5Q, so a second contribution is present |

## 5. The proposed build and its gates (frozen)

`network.phase_datum_anchor_level_hold` (bool, default false; H1) and, behind it, `network.phase_datum_anchor_level_hold_all_stations` (bool, default false; H2), the rows through the pins' channel (a new contribution kind, counted), the hold's start, end and handover as events at both edges (invariant 7), run-end counts. Gates, in order: (1) the datum-direction ratio of invariant 7 at most 0.01 on every family with no breach and no withheld epoch, on the 16 min windows; (2) knob off identical to the base exe; (3) 33 of 33 window epochs emitted, clock-datum dimension 2; (4) M as R1D section 10 (MAJU00MHL's move within a tenth of `r1d_t1on_f2`'s, at most 3 decouples at TOW 260040); (5) the 2 h arms of R1D section 9's form with the dual bar (WTZZ00DEU within 0.01 m, clock wire at most 1.10 times `r1d_ctl_f2`). Reading: H1 passing is the response candidate (6 h, census, a second field day next); H1 failing M with MAJU00MHL an anchor at TOW 260040 refutes the transport reading; H1 failing only where the role moved earlier sends H2 next.

<!-- Results are appended below this line under dated headings. -->
